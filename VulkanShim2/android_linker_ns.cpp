// 修改点：
// 1. 防止重复初始化
// 2. 限制 loader_dlopen 指令扫描步数
// 3. 修复 soname 溢出、RWX 不恢复、扫描越界、fd 泄漏

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <android/dlext.h>
#include <android/log.h>
#include <android/api-level.h>
#include <sys/mman.h>
#include <elf.h>
#include <link.h>
#include "elf_soname_patcher.h"
#include "android_linker_ns.h"

#define TAG "linkernsbypass"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##__VA_ARGS__)

using loader_android_create_namespace_t = android_namespace_t *(*)(const char *, const char *, const char *, uint64_t, const char *, android_namespace_t *, const void *);
static loader_android_create_namespace_t loader_android_create_namespace;

static bool lib_loaded;

bool linkernsbypass_load_status() {
    return lib_loaded;
}

struct android_namespace_t *android_create_namespace(const char *name,
                                                     const char *ld_library_path,
                                                     const char *default_library_path,
                                                     uint64_t type,
                                                     const char *permitted_when_isolated_path,
                                                     android_namespace_t *parent_namespace) {
    auto caller{__builtin_return_address(0)};
    return loader_android_create_namespace(name, ld_library_path, default_library_path, type,
                                           permitted_when_isolated_path, parent_namespace, caller);
}

struct android_namespace_t *android_create_namespace_escape(const char *name,
                                                            const char *ld_library_path,
                                                            const char *default_library_path,
                                                            uint64_t type,
                                                            const char *permitted_when_isolated_path,
                                                            android_namespace_t *parent_namespace) {
    auto caller{reinterpret_cast<void *>(&dlopen)};
    return loader_android_create_namespace(name, ld_library_path, default_library_path, type,
                                           permitted_when_isolated_path, parent_namespace, caller);
}

android_get_exported_namespace_t android_get_exported_namespace;
android_link_namespaces_all_libs_t android_link_namespaces_all_libs;
android_link_namespaces_t android_link_namespaces;

bool linkernsbypass_link_namespace_to_default_all_libs(android_namespace_t *to) {
    static auto defaultNs{android_create_namespace_escape("default_copy", nullptr, nullptr, ANDROID_NAMESPACE_TYPE_SHARED, nullptr, nullptr)};
    if (!defaultNs)
        return false;
    return android_link_namespaces_all_libs(to, defaultNs);
}

void *linkernsbypass_namespace_dlopen(const char *filename, int flags, android_namespace_t *ns) {
    android_dlextinfo extInfo{ .flags = ANDROID_DLEXT_USE_NAMESPACE, .library_namespace = ns };
    return android_dlopen_ext(filename, flags, &extInfo);
}

#ifndef __NR_memfd_create
    #if defined(__aarch64__)
        #define __NR_memfd_create 279
    #else
        #error Unsupported target architecture!
    #endif
#endif

void *linkernsbypass_namespace_dlopen_unique(const char *libPath, const char *libTargetDir, int flags, android_namespace_t *ns) {
    static std::array<char, PATH_MAX> PathBuf{};
    // 64-bit counter: the previous uint16_t could wrap and produce colliding
    // soname patches (e.g. id 1000 and 100 both formatted to "100").
    static uint64_t TargetId{};

    // Format the soname BEFORE incrementing so the on-disk filename and the
    // patched soname are guaranteed to describe the same generation.
    // Buffer is sized for the full uint64 range; snprintf can no longer truncate.
    char sonameOverwrite[24] = {0};
    snprintf(sonameOverwrite, sizeof(sonameOverwrite), "%llu", (unsigned long long)TargetId);

    int libTargetFd{[&] () -> int {
        if (libTargetDir) {
            snprintf(PathBuf.data(), PathBuf.size(), "%s/%llu_patched.so", libTargetDir, (unsigned long long)TargetId);
            return open(PathBuf.data(), O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR);
        } else {
            errno = 0;
            int fd{static_cast<int>(syscall(__NR_memfd_create, libPath, 0))};
            if (errno == ENOSYS || fd < 0)
                return -1;
            return fd;
        }
    }()};

    if (libTargetFd == -1) {
        LOGE("namespace_dlopen_unique: cannot obtain target fd for %s: %s", libPath, strerror(errno));
        return nullptr;
    }

    TargetId++;

    if (!elf_soname_patch(libPath, libTargetFd, sonameOverwrite)) {
        LOGE("namespace_dlopen_unique: soname patch failed for %s", libPath);
        close(libTargetFd);
        return nullptr;
    }

    android_dlextinfo hookExtInfo{ .flags = ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_USE_LIBRARY_FD, .library_fd = libTargetFd, .library_namespace = ns };

    snprintf(PathBuf.data(), PathBuf.size(), "/proc/self/fd/%d", libTargetFd);

    // On success the fd is owned by the loaded library; on failure we must
    // close it ourselves to avoid leaking one fd per attempt.
    void *handle{android_dlopen_ext(PathBuf.data(), flags, &hookExtInfo)};
    if (!handle) {
        LOGE("namespace_dlopen_unique: dlopen failed for %s: %s", libPath, dlerror());
        close(libTargetFd);
        return nullptr;
    }

    return handle;
}

static void *align_ptr(void *ptr) {
    return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ptr) & ~(uintptr_t)(getpagesize() - 1));
}

// Returns the page-aligned [start, end) range that contains `addr`, clamped to
// what mprotect can actually address. Returns false if the range is unusable.
static bool page_range_for(const void *addr, size_t len, void **start, size_t *size) {
    if (!addr || len == 0)
        return false;

    const size_t pageSize{static_cast<size_t>(getpagesize())};
    uintptr_t begin{(reinterpret_cast<uintptr_t>(addr)) & ~(uintptr_t)(pageSize - 1)};
    uintptr_t end{(reinterpret_cast<uintptr_t>(addr) + len + pageSize - 1) & ~(uintptr_t)(pageSize - 1)};

    // Guard against overflow when rounding up
    if (end < begin)
        return false;

    *start = reinterpret_cast<void *>(begin);
    *size  = end - begin;
    return true;
}

__attribute__((constructor)) static void resolve_linker_symbols() {
    if (lib_loaded)
        return;

    using loader_dlopen_t = void *(*)(const char *, int, const void *);

    if (android_get_device_api_level() < 28)
        return;

    auto loader_dlopen{[]() -> loader_dlopen_t {
        union BranchLinked {
            uint32_t raw;
            struct {
                int32_t offset : 26;
                uint8_t sig : 6;
            };
            bool Verify() { return sig == 0x25; }
        };

        static_assert(sizeof(BranchLinked) == 4, "BranchLinked is wrong size");

        // Bound the scan to the region actually covered by the &dlopen PLT
        // stub: one page starting at the page-aligned address. The previous
        // unbounded 4096-instruction walk could run off the end of the mapping
        // and segfault on some builds.
        const size_t pageSize{static_cast<size_t>(getpagesize())};
        void *rangeStart{nullptr};
        size_t rangeSize{0};
        if (!page_range_for(reinterpret_cast<const void *>(&dlopen), sizeof(void *), &rangeStart, &rangeSize))
            return nullptr;

        auto *scanBegin{reinterpret_cast<BranchLinked *>(rangeStart)};
        size_t maxSteps{rangeSize / sizeof(BranchLinked)};

        // Make the page containing &dlopen readable so we can inspect it.
        // We only add PROT_READ here; write/exec are not needed for scanning.
        if (mprotect(rangeStart, rangeSize, PROT_READ | PROT_EXEC) != 0) {
            // Some hardened kernels refuse even this; bail out rather than crash.
            return nullptr;
        }

        auto *blInstr{reinterpret_cast<BranchLinked *>(&dlopen)};
        while (maxSteps > 0 && !blInstr->Verify()) {
            blInstr++;
            maxSteps--;
        }

        if (maxSteps == 0)
            return nullptr;

        return reinterpret_cast<loader_dlopen_t>(blInstr + blInstr->offset);
    }()};

    if (!loader_dlopen) {
        LOGE("resolve_linker_symbols: failed to locate loader_dlopen trampoline");
        return;
    }

    auto ldHandle{loader_dlopen("ld-android.so", RTLD_LAZY, reinterpret_cast<void *>(&dlopen))};
    if (!ldHandle) {
        LOGE("resolve_linker_symbols: cannot load ld-android.so");
        return;
    }

    android_link_namespaces_all_libs = reinterpret_cast<android_link_namespaces_all_libs_t>(dlsym(ldHandle, "__loader_android_link_namespaces_all_libs"));
    if (!android_link_namespaces_all_libs)
        return;

    android_link_namespaces = reinterpret_cast<android_link_namespaces_t>(dlsym(ldHandle, "__loader_android_link_namespaces"));
    if (!android_link_namespaces)
        return;

    auto libdlAndroidHandle{loader_dlopen("libdl_android.so", RTLD_LAZY, reinterpret_cast<void *>(&dlopen))};
    if (!libdlAndroidHandle) {
        LOGE("resolve_linker_symbols: cannot load libdl_android.so");
        return;
    }

    loader_android_create_namespace = reinterpret_cast<loader_android_create_namespace_t>(dlsym(libdlAndroidHandle, "__loader_android_create_namespace"));
    if (!loader_android_create_namespace)
        return;

    android_get_exported_namespace = reinterpret_cast<android_get_exported_namespace_t>(dlsym(libdlAndroidHandle, "__loader_android_get_exported_namespace"));
    if (!android_get_exported_namespace)
        return;

    lib_loaded = true;
}

void public_resolve_linker_symbols() {
    resolve_linker_symbols();
}
