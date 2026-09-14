// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2021 Billy Laws

#include <initializer_list>
#include <cstdint>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>
#include <elf.h>
#include "elf_soname_patcher.h"

bool elf_soname_patch(const char *libPath, int targetFd, const char *sonamePatch) {
    struct stat libStat{};
    if (stat(libPath, &libStat))
        return false;

    if (ftruncate(targetFd, libStat.st_size) == -1)
        return false;

    // Map the memory so we can read our elf into it
    auto mappedLib{reinterpret_cast<uint8_t *>(mmap(nullptr, libStat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, targetFd, 0))};
    if (mappedLib == MAP_FAILED)
        return false;

    // Ensure the mapping is released on every exit path
    struct MappingGuard {
        uint8_t *ptr;
        size_t   size;
        ~MappingGuard() {
            if (ptr && ptr != MAP_FAILED)
                munmap(ptr, size);
        }
    } mapGuard{mappedLib, static_cast<size_t>(libStat.st_size)};

    int libFd{open(libPath, O_RDONLY)};
    if (libFd < 0)
        return false;

    // Read lib elf into target file
    ssize_t readBytes{read(libFd, mappedLib, libStat.st_size)};
    close(libFd);

    if (readBytes != static_cast<ssize_t>(libStat.st_size))
        return false;

    auto eHdr{reinterpret_cast<ElfW(Ehdr) *>(mappedLib)};

    // Sanity-check ELF magic before trusting any header offset
    if (memcmp(eHdr->e_ident, ELFMAG, SELFMAG) != 0)
        return false;

    // Guard against section header table running past the mapping
    if (eHdr->e_shoff == 0 ||
        eHdr->e_shoff + (ElfW(Off))eHdr->e_shnum * eHdr->e_shentsize > (ElfW(Off))libStat.st_size)
        return false;

    auto sHdrEntries{reinterpret_cast<ElfW(Shdr) *>(mappedLib + eHdr->e_shoff)};

    // Iterate over section headers to find the .dynamic section
    for (ElfW(Half) i{}; i < eHdr->e_shnum; i++) {
        auto &sHdr{sHdrEntries[i]};
        if (sHdr.sh_type == SHT_DYNAMIC) {
            // Bounds-check the linked string table section
            if (sHdr.sh_link >= eHdr->e_shnum)
                return false;

            auto strTab{reinterpret_cast<char *>(mappedLib + sHdrEntries[sHdr.sh_link].sh_offset)};
            auto dynHdrEntries{reinterpret_cast<ElfW(Dyn) *>(mappedLib + sHdr.sh_offset)};

            // Iterate over .dynamic entries to find DT_SONAME
            for (ElfW(Xword) k{}; k < (sHdr.sh_size / sHdr.sh_entsize); k++) {
                auto &dynHdrEntry{dynHdrEntries[k]};
                if (dynHdrEntry.d_tag == DT_SONAME) {
                    // Bound the soname offset to the mapped region
                    if (dynHdrEntry.d_un.d_val >= libStat.st_size)
                        return false;

                    char *soname{strTab + dynHdrEntry.d_un.d_val};

                    // Partially replace the old soname with the soname patch
                    size_t charIdx{};
                    for (; soname[charIdx] != 0 && sonamePatch[charIdx] != 0; charIdx++)
                        soname[charIdx] = sonamePatch[charIdx];

                    return true;
                }
            }
        }
    }

    return false;
}
