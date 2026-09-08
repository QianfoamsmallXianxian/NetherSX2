// 修改点：
// 1. 精确识别 Vulkan 驱动
// 2. 日志降级
// 3. 增加 Turbo 接口

#include <initializer_list>
#include <string>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <android_linker_ns.h>
#include <android/dlext.h>
#include <android/log.h>
#include "kgsl.h"
#include "hook_impl_params.h"
#include "hook_impl.h"

#define TAG "hook_impl"
#define DEBUG_VULKAN_HOOK 0
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__)

const HookImplParams *hook_params;
int (*gsl_memory_alloc_pure_sym)(uint32_t, uint32_t, void *);
int (*gsl_memory_alloc_pure_64_sym)(uint64_t, uint32_t, void *);
int (*gsl_memory_free_pure_sym)(void *);
int kgsl_fd;

static bool turbo_mode = false;

using gsl_memory_alloc_pure_t = decltype(gsl_memory_alloc_pure_sym);
using gsl_memory_alloc_pure_64_t = decltype(gsl_memory_alloc_pure_64_sym);
using gsl_memory_free_pure_t = decltype(gsl_memory_free_pure_sym);

__attribute__((visibility("default"))) void adrenotools_set_turbo(bool turbo) {
    turbo_mode = turbo;
}

static bool is_vulkan_driver(const char *filename) {
    if (!filename) return false;

    static const char *targets[] = {
        "libvulkan.so",
        "libvulkan_freedreno.so",
        "libvulkan_adreno.so",
        "vulkan.adreno.so",
        "vulkan.freedreno.so",
        nullptr
    };

    for (int i = 0; targets[i] != nullptr; ++i) {
        if (strstr(filename, targets[i])) return true;
    }
    return false;
}

__attribute__((visibility("default"))) void init_hook_param(const void *param) {
    hook_params = reinterpret_cast<const HookImplParams *>(param);
}

__attribute__((visibility("default"))) void init_gsl(void *alloc, void *alloc64, void *free) {
    gsl_memory_alloc_pure_sym = reinterpret_cast<gsl_memory_alloc_pure_t>(alloc);
    gsl_memory_alloc_pure_64_sym = reinterpret_cast<gsl_memory_alloc_pure_64_t>(alloc64);
    gsl_memory_free_pure_sym = reinterpret_cast<gsl_memory_free_pure_t>(free);
}

__attribute__((visibility("default"))) void *hook_android_dlopen_ext(const char *filename, int flags, const android_dlextinfo *extinfo) {
    auto fallback{[&]() {
        return android_dlopen_ext(filename, flags, extinfo);
    }};

#if DEBUG_VULKAN_HOOK
    LOGI("hook_android_dlopen_ext: filename: %s", filename);
#endif

    if (!is_vulkan_driver(filename))
        return android_dlopen_ext(filename, flags, extinfo);

    if (extinfo->library_namespace == nullptr || !(extinfo->flags & ANDROID_DLEXT_USE_NAMESPACE)) {
        return fallback();
    }

    auto driverNs{android_create_namespace(filename, hook_params->customDriverDir.c_str(),
                                           hook_params->hookLibDir.c_str(), ANDROID_NAMESPACE_TYPE_SHARED,
                                           nullptr, extinfo->library_namespace)};
    if (!driverNs)
        return fallback();

    android_link_namespaces(driverNs, nullptr, "libandroid.so");

    auto hookImpl{linkernsbypass_namespace_dlopen("libhook_impl.so", RTLD_NOW, driverNs)};
    if (!hookImpl)
        return nullptr;

    auto initHookParam{reinterpret_cast<void (*)(const void *)>(dlsym(hookImpl, "init_hook_param"))};
    if (!initHookParam)
        return nullptr;

    initHookParam(hook_params);

    if (hook_params->featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) {
        if (!linkernsbypass_namespace_dlopen("libfile_redirect_hook.so", RTLD_GLOBAL, driverNs))
            return fallback();
    }

    auto newExtinfo{*extinfo};
    newExtinfo.library_namespace = driverNs;

    if (hook_params->featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) {
        if (!linkernsbypass_namespace_dlopen("libgsl_alloc_hook.so", RTLD_GLOBAL, driverNs))
            return fallback();

        auto libgslHandle{android_dlopen_ext("vkbgsl.so", RTLD_NOW, &newExtinfo)};
        if (!libgslHandle) {
            libgslHandle = android_dlopen_ext("notgsl.so", RTLD_NOW, &newExtinfo);
            if (!libgslHandle)
                libgslHandle = android_dlopen_ext("libgsl.so", RTLD_NOW, &newExtinfo);
        }

        if (libgslHandle) {
            gsl_memory_alloc_pure_sym = reinterpret_cast<decltype(gsl_memory_alloc_pure_sym)>(dlsym(libgslHandle, "gsl_memory_alloc_pure"));
            gsl_memory_alloc_pure_64_sym = reinterpret_cast<decltype(gsl_memory_alloc_pure_64_sym)>(dlsym(libgslHandle, "gsl_memory_alloc_pure_64"));
            gsl_memory_free_pure_sym = reinterpret_cast<decltype(gsl_memory_free_pure_sym)>(dlsym(libgslHandle, "gsl_memory_free_pure"));

            if ((gsl_memory_alloc_pure_sym || gsl_memory_alloc_pure_64_sym) && gsl_memory_free_pure_sym) {
                auto initGsl{reinterpret_cast<void (*)(gsl_memory_alloc_pure_t, gsl_memory_alloc_pure_64_t, gsl_memory_free_pure_t)>(dlsym(hookImpl, "init_gsl"))};
                if (!initGsl)
                    return fallback();

                initGsl(gsl_memory_alloc_pure_sym, gsl_memory_alloc_pure_64_sym, gsl_memory_free_pure_sym);
                hook_params->nextGpuMapping->gpu_addr = ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC;
            }
        }

        if (!((gsl_memory_alloc_pure_sym || gsl_memory_alloc_pure_64_sym) && gsl_memory_free_pure_sym))
            return fallback();
    }

    if (hook_params->featureFlags & ADRENOTOOLS_DRIVER_CUSTOM) {
        void *handle{android_dlopen_ext(hook_params->customDriverName.c_str(), flags, &newExtinfo)};
        if (!handle)
            return fallback();

        return handle;
    } else {
        return android_dlopen_ext(filename, flags, &newExtinfo);
    }
}

__attribute__((visibility("default"))) void *hook_android_load_sphal_library(const char *filename, int flags) {
    for (const char *name : {"sphal", "vendor", "default"}) {
        if (auto vendorNs{android_get_exported_namespace(name)}) {
            android_dlextinfo dlextinfo{
                .flags = ANDROID_DLEXT_USE_NAMESPACE,
                .library_namespace = vendorNs,
            };

            return hook_android_dlopen_ext(filename, flags, &dlextinfo);
        }
    }

    return nullptr;
}

__attribute__((visibility("default"))) FILE *hook_fopen(const char *filename, const char *mode) {
    if (!strncmp("/proc", filename, 5) || !strncmp("/sys", filename, 4))
        return fopen(filename, mode);

    auto replacement{hook_params->fileRedirectDir + filename};
    return fopen(replacement.c_str(), mode);
}

static constexpr uintptr_t GslMemDescImportedPrivMagic{0xdeadb33f};

struct GslMemDesc {
    void *hostptr;
    uint64_t gpuaddr;
    uint64_t size;
    uint64_t flags;
    uintptr_t priv;
};

__attribute__((visibility("default"))) int hook_gsl_memory_alloc_pure_64(uint64_t size, uint32_t flags, void *memDesc) {
    auto gslMemDesc{reinterpret_cast<GslMemDesc *>(memDesc)};

    if (hook_params->nextGpuMapping && hook_params->nextGpuMapping->size == size && (hook_params->nextGpuMapping->flags & flags) == hook_params->nextGpuMapping->flags) {
        auto &nextMapping{*hook_params->nextGpuMapping};

        gslMemDesc->hostptr = nextMapping.host_ptr;
        gslMemDesc->gpuaddr = nextMapping.gpu_addr;
        gslMemDesc->size = nextMapping.size;
        gslMemDesc->flags = nextMapping.flags;
        gslMemDesc->priv = GslMemDescImportedPrivMagic;
        hook_params->nextGpuMapping->size = 0;
        hook_params->nextGpuMapping->gpu_addr = ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC;
        return 0;
    } else {
        if (gsl_memory_alloc_pure_64_sym)
            return gsl_memory_alloc_pure_64_sym(size, flags, gslMemDesc);
        else
            return gsl_memory_alloc_pure_sym((uint32_t)size, flags, gslMemDesc);
    }
}

__attribute__((visibility("default"))) int hook_gsl_memory_free_pure(void *memDesc) {
    auto gslMemDesc{reinterpret_cast<GslMemDesc *>(memDesc)};

    if (gslMemDesc->priv == GslMemDescImportedPrivMagic) {
        if (!kgsl_fd)
            kgsl_fd = open("/dev/kgsl-3d0", O_RDWR);

        kgsl_gpumem_get_info info{ .gpuaddr = gslMemDesc->gpuaddr };

        if (ioctl(kgsl_fd, IOCTL_KGSL_GPUMEM_GET_INFO, &info) < 0)
            return 0;

        kgsl_gpuobj_free args{ .id = info.id };

        if (ioctl(kgsl_fd, IOCTL_KGSL_GPUOBJ_FREE, &args) < 0) {}

        return 0;
    } else {
        return gsl_memory_free_pure_sym(memDesc);
    }
}
