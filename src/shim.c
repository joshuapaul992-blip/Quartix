/*
 * Quartix - resolver and implementations for the Android/libc symbols
 * libroblox.so imports. Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * libroblox.so imports ~585 symbols from the Android system libraries. This
 * file provides the first, foundational slice: the pure libc string/memory
 * routines, the libm math routines, the non-variadic liblog calls, and libdl.
 * The stateful and platform-bound families - pthreads, EGL/GLES, OpenSL/media,
 * and the libandroid NativeActivity surface - come in later phases; until a
 * symbol is implemented, qx_shim_resolve returns NULL and the loader binds the
 * import to a trap.
 *
 * ABI: the Android code uses the System V AMD64 calling convention, so every
 * function the engine can call is declared __attribute__((sysv_abi)). Each thunk
 * may then call the Microsoft-ABI C runtime normally; the compiler bridges the
 * two conventions. qsort/bsearch take a comparator supplied by the engine, so
 * that pointer is typed sysv_abi and they are implemented here rather than
 * forwarded to the CRT (which would call the comparator with the wrong ABI).
 */
#include "quartix.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi))

/* --- trap for everything not implemented yet --- */
static SYSV void qx_shim_trap(void)
{
    qx_err("libroblox.so called an Android symbol Quartix does not implement yet");
    ExitProcess(3);
}

void *qx_shim_trap_addr(void) { return (void *)qx_shim_trap; }

/* --- libc: memory and strings (pure) --- */
static SYSV void *sh_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static SYSV void *sh_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static SYSV void *sh_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static SYSV int sh_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static SYSV void *sh_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
static SYSV size_t sh_strlen(const char *s) { return strlen(s); }
static SYSV int sh_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static SYSV int sh_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static SYSV char *sh_strcpy(char *d, const char *s) { return strcpy(d, s); }
static SYSV char *sh_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static SYSV char *sh_strcat(char *d, const char *s) { return strcat(d, s); }
static SYSV char *sh_strchr(const char *s, int c) { return strchr(s, c); }
static SYSV char *sh_strrchr(const char *s, int c) { return strrchr(s, c); }
static SYSV char *sh_strstr(const char *h, const char *n) { return strstr(h, n); }
static SYSV long sh_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
static SYSV unsigned long sh_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }

/* qsort/bsearch: the comparator is engine code, so it is SysV-ABI. */
typedef int(SYSV *sh_cmp)(const void *, const void *);

static SYSV void sh_qsort(void *base, size_t n, size_t sz, sh_cmp cmp)
{
    unsigned char *a = (unsigned char *)base, *tmp;
    size_t gap, i, j;
    if (n < 2 || sz == 0)
        return;
    tmp = (unsigned char *)malloc(sz);
    if (!tmp)
        return;
    for (gap = n / 2; gap > 0; gap /= 2) {
        for (i = gap; i < n; i++) {
            memcpy(tmp, a + i * sz, sz);
            for (j = i; j >= gap && cmp(a + (j - gap) * sz, tmp) > 0; j -= gap)
                memcpy(a + j * sz, a + (j - gap) * sz, sz);
            memcpy(a + j * sz, tmp, sz);
        }
    }
    free(tmp);
}

static SYSV void *sh_bsearch(const void *key, const void *base, size_t n, size_t sz, sh_cmp cmp)
{
    const unsigned char *a = (const unsigned char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, a + mid * sz);
        if (r == 0)
            return (void *)(a + mid * sz);
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* --- libc: process/runtime --- */
static int sh_errno_storage; /* single-threaded for now; pthread phase makes it per-thread */
static SYSV int *sh_errno(void) { return &sh_errno_storage; }
/* exported so the CRT-backed shims (e.g. src/stdio.c) can propagate host errno
 * into the location the engine reads through __errno */
int *qx_shim_errno(void) { return &sh_errno_storage; }
static SYSV void sh_abort(void) { qx_err("libroblox.so called abort()"); ExitProcess(134); }
static SYSV void sh_exit(int code) { ExitProcess((UINT)code); }
static SYSV void sh_stack_chk_fail(void) { qx_err("stack smashing detected in libroblox.so"); ExitProcess(134); }
static SYSV char *sh_getenv(const char *name) { (void)name; return NULL; }

/* string -> number (non-variadic). atol returns long long because the engine is
 * LP64, where long is 64-bit (it is 32-bit on this Win64 host). */
static SYSV double sh_strtod(const char *s, char **e) { return strtod(s, e); }
static SYSV float sh_strtof(const char *s, char **e) { return strtof(s, e); }
static SYSV long long sh_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
static SYSV unsigned long long sh_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
static SYSV double sh_atof(const char *s) { return atof(s); }
static SYSV int sh_atoi(const char *s) { return atoi(s); }
static SYSV long long sh_atol(const char *s) { return strtoll(s, NULL, 10); }

/* --- liblog (non-variadic) --- */
static SYSV int sh_log_write(int prio, const char *tag, const char *text)
{
    (void)prio;
    qx_log("[roblox] %s: %s", tag ? tag : "", text ? text : "");
    return 0;
}
static SYSV int sh_log_buf_write(int buf, int prio, const char *tag, const char *text)
{
    (void)buf; (void)prio;
    qx_log("[roblox] %s: %s", tag ? tag : "", text ? text : "");
    return 0;
}

/* --- libm (pure) --- */
static SYSV double sh_sin(double x) { return sin(x); }
static SYSV double sh_cos(double x) { return cos(x); }
static SYSV double sh_tan(double x) { return tan(x); }
static SYSV double sh_asin(double x) { return asin(x); }
static SYSV double sh_acos(double x) { return acos(x); }
static SYSV double sh_atan(double x) { return atan(x); }
static SYSV double sh_atan2(double y, double x) { return atan2(y, x); }
static SYSV double sh_exp(double x) { return exp(x); }
static SYSV double sh_exp2(double x) { return exp2(x); }
static SYSV double sh_log(double x) { return log(x); }
static SYSV double sh_log2(double x) { return log2(x); }
static SYSV double sh_pow(double x, double y) { return pow(x, y); }
static SYSV double sh_fmod(double x, double y) { return fmod(x, y); }
static SYSV double sh_round(double x) { return round(x); }
static SYSV double sh_frexp(double x, int *e) { return frexp(x, e); }
static SYSV double sh_ldexp(double x, int e) { return ldexp(x, e); }
static SYSV void sh_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

/* --- libdl --- */
static SYSV void *sh_dlsym(void *handle, const char *name) { (void)handle; return qx_shim_resolve(name); }
static SYSV void *sh_dlopen(const char *path, int flag) { (void)path; (void)flag; return (void *)1; }
static SYSV int sh_dlclose(void *handle) { (void)handle; return 0; }
static SYSV char *sh_dlerror(void) { return NULL; }
static SYSV int sh_dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; }

/* --- resolver table --- */
struct shim_sym {
    const char *name;
    void *fn;
};

static const struct shim_sym g_table[] = {
    {"memcpy", (void *)sh_memcpy}, {"memmove", (void *)sh_memmove}, {"memset", (void *)sh_memset},
    {"memcmp", (void *)sh_memcmp}, {"memchr", (void *)sh_memchr}, {"strlen", (void *)sh_strlen},
    {"strcmp", (void *)sh_strcmp}, {"strncmp", (void *)sh_strncmp}, {"strcpy", (void *)sh_strcpy},
    {"strncpy", (void *)sh_strncpy}, {"strcat", (void *)sh_strcat}, {"strchr", (void *)sh_strchr},
    {"strrchr", (void *)sh_strrchr}, {"strstr", (void *)sh_strstr}, {"strtol", (void *)sh_strtol},
    {"strtoul", (void *)sh_strtoul}, {"qsort", (void *)sh_qsort}, {"bsearch", (void *)sh_bsearch},
    {"__errno", (void *)sh_errno}, {"abort", (void *)sh_abort}, {"exit", (void *)sh_exit},
    {"__stack_chk_fail", (void *)sh_stack_chk_fail}, {"getenv", (void *)sh_getenv},
    {"__android_log_write", (void *)sh_log_write}, {"__android_log_buf_write", (void *)sh_log_buf_write},
    {"sin", (void *)sh_sin}, {"cos", (void *)sh_cos}, {"tan", (void *)sh_tan}, {"asin", (void *)sh_asin},
    {"acos", (void *)sh_acos}, {"atan", (void *)sh_atan}, {"atan2", (void *)sh_atan2}, {"exp", (void *)sh_exp},
    {"exp2", (void *)sh_exp2}, {"log", (void *)sh_log}, {"log2", (void *)sh_log2}, {"pow", (void *)sh_pow},
    {"fmod", (void *)sh_fmod}, {"round", (void *)sh_round}, {"frexp", (void *)sh_frexp},
    {"ldexp", (void *)sh_ldexp}, {"sincos", (void *)sh_sincos},
    {"dlsym", (void *)sh_dlsym}, {"dlopen", (void *)sh_dlopen}, {"dlclose", (void *)sh_dlclose},
    {"dlerror", (void *)sh_dlerror}, {"dladdr", (void *)sh_dladdr},
    {"strtod", (void *)sh_strtod}, {"strtof", (void *)sh_strtof}, {"strtoll", (void *)sh_strtoll},
    {"strtoull", (void *)sh_strtoull}, {"atof", (void *)sh_atof}, {"atoi", (void *)sh_atoi},
    {"atol", (void *)sh_atol},
};

void *qx_shim_resolve(const char *name)
{
    size_t i;
    void *r;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++)
        if (strcmp(name, g_table[i].name) == 0)
            return g_table[i].fn;
    r = qx_thread_resolve(name);
    if (r)
        return r;
    r = qx_format_resolve(name);
    if (r)
        return r;
    r = qx_stdio_resolve(name);
    if (r)
        return r;
    return qx_scan_resolve(name);
}
