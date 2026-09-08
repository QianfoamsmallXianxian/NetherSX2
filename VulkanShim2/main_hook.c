#include "hook_impl.h"
#include <stdbool.h>

bool linkernsbypass_load_status(void);

__attribute__((visibility("default")))
void *android_dlopen_ext(const char *filename, int flags, const android_dlextinfo *extinfo) {
    if (!linkernsbypass_load_status())
        return NULL;

    return hook_android_dlopen_ext(filename, flags, extinfo);
}

__attribute__((visibility("default")))
void *android_load_sphal_library(const char *filename, int flags) {
    if (!linkernsbypass_load_status())
        return NULL;

    return hook_android_load_sphal_library(filename, flags);
}
