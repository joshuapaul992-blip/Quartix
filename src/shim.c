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

/*
 * A named trap: so that when the engine calls a symbol Quartix has not
 * implemented, the log says exactly which one - the single most useful fact for
 * deciding what to build next. Each unresolved symbol gets its own tiny code
 * stub that records the symbol's name and jumps to the handler below. The names
 * point into the mapped image's string table, which outlives startup, so they
 * need no copy. Stubs are generated into one executable page, deduplicated by
 * name. If the table fills or the page cannot be allocated, we fall back to the
 * anonymous trap - the image is still complete.
 */
static const volatile char *g_trap_hit;      /* name of the symbol last trapped on */

static SYSV void qx_shim_trap_report(void)
{
    const char *n = (const char *)g_trap_hit;
    qx_err("libroblox.so called an unimplemented Android symbol: %s", n ? n : "(unknown)");
    ExitProcess(3);
}

#define QX_NAMED_TRAP_CAP 512
#define QX_NAMED_TRAP_STUB 32
static const char *g_trap_names[QX_NAMED_TRAP_CAP];
static unsigned char *g_trap_page;   /* QX_NAMED_TRAP_CAP * QX_NAMED_TRAP_STUB bytes, RX */
static int g_trap_n;

void *qx_shim_named_trap(const char *name)
{
    int i;
    unsigned char *s;
    uint64_t name_ptr, store_ptr, handler;

    if (!name)
        return qx_shim_trap_addr();
    for (i = 0; i < g_trap_n; i++)          /* dedup: one stub per symbol */
        if (strcmp(g_trap_names[i], name) == 0)
            return g_trap_page + (size_t)i * QX_NAMED_TRAP_STUB;
    if (g_trap_n >= QX_NAMED_TRAP_CAP)
        return qx_shim_trap_addr();
    if (!g_trap_page) {
        g_trap_page = (unsigned char *)VirtualAlloc(
            NULL, (SIZE_T)QX_NAMED_TRAP_CAP * QX_NAMED_TRAP_STUB,
            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!g_trap_page)
            return qx_shim_trap_addr();
    }

    /* Emit, for this symbol's stub:
     *   movabs rax, name          48 B8 <imm64>
     *   mov    [&g_trap_hit], rax 48 A3 <abs64>   (store the name)
     *   movabs rax, handler       48 B8 <imm64>
     *   jmp    rax                FF E0
     * The stub never returns, so clobbering rax is harmless. */
    s = g_trap_page + (size_t)g_trap_n * QX_NAMED_TRAP_STUB;
    name_ptr = (uint64_t)(uintptr_t)name;
    store_ptr = (uint64_t)(uintptr_t)&g_trap_hit;
    handler = (uint64_t)(uintptr_t)qx_shim_trap_report;
    s[0] = 0x48; s[1] = 0xB8; memcpy(s + 2, &name_ptr, 8);
    s[10] = 0x48; s[11] = 0xA3; memcpy(s + 12, &store_ptr, 8);
    s[20] = 0x48; s[21] = 0xB8; memcpy(s + 22, &handler, 8);
    s[30] = 0xFF; s[31] = 0xE0;

    g_trap_names[g_trap_n] = name;
    return g_trap_page + (size_t)g_trap_n++ * QX_NAMED_TRAP_STUB;
}

/* List every symbol currently bound to a trap (i.e. imported but not yet
 * implemented). Discovery aid; returns the count. */
int qx_shim_dump_pending(void)
{
    int i;
    for (i = 0; i < g_trap_n; i++)
        qx_log("  pending: %s", g_trap_names[i]);
    return g_trap_n;
}

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
    r = qx_scan_resolve(name);
    if (r)
        return r;
    r = qx_os_resolve(name);
    if (r)
        return r;
    r = qx_cstd_resolve(name);
    if (r)
        return r;
    r = qx_android_resolve(name);
    if (r)
        return r;
    r = qx_socket_resolve(name);
    if (r)
        return r;
    return qx_gl_resolve(name);
}
