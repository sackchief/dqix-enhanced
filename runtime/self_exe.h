/* The running program's own path (Linux: /proc/self/exe; macOS has no /proc). */
#pragma once
#ifndef _WIN32
#include <stddef.h>
#include <unistd.h>
#ifdef __APPLE__
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <string.h>
#endif
/* the path into buf (NUL-terminated); its length, or -1 */
static inline ssize_t rt_self_exe(char* buf, size_t n) {
    if (n == 0) return -1;
#ifdef __APPLE__
    char raw[PATH_MAX]; uint32_t size = sizeof raw;
    char real[PATH_MAX];
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, real)) { buf[0] = 0; return -1; }
    size_t l = strlen(real);
    if (l >= n) { buf[0] = 0; return -1; }
    memcpy(buf, real, l + 1);
    return (ssize_t)l;
#else
    ssize_t l = readlink("/proc/self/exe", buf, n - 1);
    buf[l > 0 ? l : 0] = 0;
    return l > 0 ? l : -1;
#endif
}
#endif
