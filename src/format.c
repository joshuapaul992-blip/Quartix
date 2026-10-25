/*
 * Quartix - the variadic printf/log family for the Android engine, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * libroblox.so calls a handful of variadic C functions: the printf family
 * (printf, snprintf, vsnprintf, the _chk fortified forms), syslog, and Android's
 * __android_log_print / __android_log_assert. These are the hard case of the
 * shim, for two reasons:
 *
 *  1. ABI. The engine passes its variadic arguments by the System V AMD64
 *     convention (integers in registers, floats in SSE registers, spilled to a
 *     register save area), but mingw's default va_list walks the Microsoft
 *     convention (everything on the stack). Reading a SysV call with an MS
 *     va_list reads the wrong memory. GCC exposes __builtin_sysv_va_list and the
 *     matching __builtin_sysv_va_* builtins for exactly this; every function here
 *     is sysv_abi and must also be noinline, so the SysV-variadic register-save
 *     prologue is never inlined into a Microsoft-ABI frame (which ICEs the
 *     compiler and would corrupt the frame regardless).
 *
 *  2. LP64 vs LLP64. The engine is LP64, so "%ld" passes a 64-bit long; on Win64
 *     long is 32-bit. The length modifier is therefore parsed here to decide how
 *     many bytes to pull from the va_list (l/ll/z/j/t/q -> 64-bit), independent
 *     of the host's long.
 *
 * Each conversion's actual rendering (every float format, width, precision and
 * flag) is delegated to the host's mingw ANSI snprintf by reconstructing a
 * single-conversion format string and calling it with the one extracted
 * argument - correct and locale-aware without reimplementing dtoa. The build
 * defines __USE_MINGW_ANSI_STDIO=1, so the host snprintf is the C99 one.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* noinline/noclone: keep the SysV-variadic prologue out of any MS-ABI frame. */
#define SYSV __attribute__((sysv_abi, noinline, noclone))
typedef __builtin_sysv_va_list sysv_va;

/* argument width class, after the length modifier is normalised */
enum len_kind { L_INT, L_I64, L_DBL, L_LDBL };

/* A growing output sink with snprintf truncation semantics: it reports how many
 * bytes the whole result would need, but writes at most cap-1 + a NUL. */
struct sink {
    char *buf;
    size_t cap;
    size_t len;
};

static void sink_putc(struct sink *s, char c)
{
    if (s->buf && s->len + 1 < s->cap)
        s->buf[s->len] = c;
    s->len++;
}
static void sink_puts(struct sink *s, const char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        sink_putc(s, p[i]);
}

/* Render one conversion with the host snprintf. The argument is read once into a
 * local, so a result that overflows the stack temp can be re-rendered into a
 * heap buffer from the saved value. spec is the reconstructed single-conversion
 * format in the host's length spelling. */
#define RENDER_WITH(TYPE)                                                      \
    do {                                                                       \
        TYPE a = __builtin_va_arg(ap, TYPE);                                   \
        int need = snprintf(tmp, sizeof(tmp), spec, a);                        \
        if (need < 0)                                                          \
            return;                                                            \
        if ((size_t)need >= sizeof(tmp)) {                                     \
            char *big = (char *)malloc((size_t)need + 1);                      \
            if (big) {                                                         \
                snprintf(big, (size_t)need + 1, spec, a);                      \
                sink_puts(s, big, (size_t)need);                              \
                free(big);                                                     \
            } else {                                                           \
                sink_puts(s, tmp, strlen(tmp));                                \
            }                                                                  \
            return;                                                            \
        }                                                                      \
        sink_puts(s, tmp, (size_t)need);                                       \
        return;                                                                \
    } while (0)

static void render_one(struct sink *s, const char *spec, enum len_kind cls, char conv, sysv_va ap)
{
    char tmp[256];
    switch (conv) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
        if (cls == L_I64)
            RENDER_WITH(long long);
        else
            RENDER_WITH(int);
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        if (cls == L_LDBL)
            RENDER_WITH(long double);
        else
            RENDER_WITH(double);
    case 'c':
        RENDER_WITH(int);
    case 'p':
        RENDER_WITH(void *);
    case 's': {
        const char *a = __builtin_va_arg(ap, const char *);
        int need;
        if (!a)
            a = "(null)";
        need = snprintf(tmp, sizeof(tmp), spec, a);
        if (need < 0)
            return;
        if ((size_t)need >= sizeof(tmp)) {
            char *big = (char *)malloc((size_t)need + 1);
            if (big) {
                snprintf(big, (size_t)need + 1, spec, a);
                sink_puts(s, big, (size_t)need);
                free(big);
            } else {
                sink_puts(s, tmp, strlen(tmp));
            }
            return;
        }
        sink_puts(s, tmp, (size_t)need);
        return;
    }
    default:
        /* unknown conversion: emit the reconstructed spec verbatim, dropping
         * nothing (the matching argument, if any, is then misaligned, but a
         * format Quartix does not understand is already a bug on the engine's
         * side and this keeps the rest of the string intact) */
        sink_puts(s, spec, strlen(spec));
        return;
    }
}

/* append a decimal integer to rebuilt[] (bounded) */
static size_t put_int(char *rebuilt, size_t ri, size_t cap, int v)
{
    char d[16];
    int n = 0;
    unsigned uv;
    if (v < 0) {
        if (ri < cap - 1)
            rebuilt[ri++] = '-';
        uv = (unsigned)(-(v + 1)) + 1u;
    } else {
        uv = (unsigned)v;
    }
    do {
        d[n++] = (char)('0' + uv % 10u);
        uv /= 10u;
    } while (uv);
    while (n-- > 0)
        if (ri < cap - 1)
            rebuilt[ri++] = d[n];
    return ri;
}

/* Core formatter. Parses fmt, pulls each argument per the SysV va_list in order,
 * and delegates each conversion's rendering to the host snprintf. Returns the
 * number of bytes the whole result needs (snprintf semantics). */
static size_t qx_vformat(char *out, size_t n, const char *fmt, sysv_va ap)
{
    struct sink s;
    const char *p = fmt;
    s.buf = out;
    s.cap = n;
    s.len = 0;
    if (!fmt) {
        if (out && n)
            out[0] = '\0';
        return 0;
    }
    while (*p) {
        char rebuilt[80];
        size_t ri = 0;
        enum len_kind cls = L_INT;
        char conv;
        const char *start;

        if (*p != '%') {
            sink_putc(&s, *p++);
            continue;
        }
        start = p;
        p++;
        if (*p == '%') {
            sink_putc(&s, '%');
            p++;
            continue;
        }
        rebuilt[ri++] = '%';
        /* flags */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            if (ri < sizeof(rebuilt) - 1)
                rebuilt[ri++] = *p;
            p++;
        }
        /* width */
        if (*p == '*') {
            int w = __builtin_va_arg(ap, int);
            p++;
            if (w < 0) {
                if (ri < sizeof(rebuilt) - 1)
                    rebuilt[ri++] = '-';   /* negative field width = left-justify */
                w = -w;
            }
            ri = put_int(rebuilt, ri, sizeof(rebuilt), w);
        } else {
            while (*p >= '0' && *p <= '9') {
                if (ri < sizeof(rebuilt) - 1)
                    rebuilt[ri++] = *p;
                p++;
            }
        }
        /* precision */
        if (*p == '.') {
            p++;
            if (*p == '*') {
                int pr = __builtin_va_arg(ap, int);
                p++;
                if (pr >= 0) {         /* a negative '*' precision is as if omitted */
                    if (ri < sizeof(rebuilt) - 1)
                        rebuilt[ri++] = '.';
                    ri = put_int(rebuilt, ri, sizeof(rebuilt), pr);
                }
            } else {
                if (ri < sizeof(rebuilt) - 1)
                    rebuilt[ri++] = '.';
                while (*p >= '0' && *p <= '9') {
                    if (ri < sizeof(rebuilt) - 1)
                        rebuilt[ri++] = *p;
                    p++;
                }
            }
        }
        /* length modifier: parsed to size the va_arg, not copied verbatim */
        if (p[0] == 'h' && p[1] == 'h')
            p += 2;
        else if (p[0] == 'h')
            p += 1;
        else if (p[0] == 'l' && p[1] == 'l') {
            cls = L_I64;
            p += 2;
        } else if (p[0] == 'l') {
            cls = L_I64;               /* LP64: the engine's long is 64-bit */
            p += 1;
        } else if (p[0] == 'L') {
            cls = L_LDBL;
            p += 1;
        } else if (p[0] == 'z' || p[0] == 'j' || p[0] == 't' || p[0] == 'q') {
            cls = L_I64;
            p += 1;
        }
        conv = *p;
        if (!conv) {
            sink_puts(&s, start, (size_t)(p - start));
            break;
        }
        p++;

        /* float conversions take a double unless modified by L */
        if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
            conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            if (cls != L_LDBL)
                cls = L_DBL;
        }
        /* host length spelling for the reconstructed spec */
        if (cls == L_I64) {
            if (ri < sizeof(rebuilt) - 3) {
                rebuilt[ri++] = 'l';
                rebuilt[ri++] = 'l';
            }
        } else if (cls == L_LDBL) {
            if (ri < sizeof(rebuilt) - 2)
                rebuilt[ri++] = 'L';
        }
        if (ri < sizeof(rebuilt) - 1)
            rebuilt[ri++] = conv;
        rebuilt[ri] = '\0';
        render_one(&s, rebuilt, cls, conv, ap);
    }
    if (s.buf && s.cap)
        s.buf[s.len < s.cap ? s.len : s.cap - 1] = '\0';
    return s.len;
}

/* ---- entry points --------------------------------------------------------- */
static SYSV int qx_snprintf(char *b, size_t n, const char *fmt, ...)
{
    sysv_va ap;
    size_t r;
    __builtin_sysv_va_start(ap, fmt);
    r = qx_vformat(b, n, fmt, ap);
    __builtin_sysv_va_end(ap);
    return (int)r;
}
static SYSV int qx_vsnprintf(char *b, size_t n, const char *fmt, sysv_va ap)
{
    return (int)qx_vformat(b, n, fmt, ap);
}
/* __vsnprintf_chk(dest, supplied_size, flags, dest_object_size, fmt, ap) */
static SYSV int qx_vsnprintf_chk(char *b, size_t n, int flag, size_t slen, const char *fmt, sysv_va ap)
{
    (void)flag;
    if (slen < n)
        n = slen;
    return (int)qx_vformat(b, n, fmt, ap);
}
/* __vsprintf_chk(dest, flags, dest_object_size, fmt, ap) */
static SYSV int qx_vsprintf_chk(char *b, int flag, size_t slen, const char *fmt, sysv_va ap)
{
    (void)flag;
    return (int)qx_vformat(b, slen, fmt, ap);
}
static SYSV int qx_printf(const char *fmt, ...)
{
    char buf[2048];
    sysv_va ap;
    size_t r;
    __builtin_sysv_va_start(ap, fmt);
    r = qx_vformat(buf, sizeof(buf), fmt, ap);
    __builtin_sysv_va_end(ap);
    qx_log("%s", buf);
    return (int)r;
}
static SYSV void qx_syslog(int prio, const char *fmt, ...)
{
    char buf[2048];
    sysv_va ap;
    (void)prio;
    __builtin_sysv_va_start(ap, fmt);
    qx_vformat(buf, sizeof(buf), fmt, ap);
    __builtin_sysv_va_end(ap);
    qx_log("[syslog] %s", buf);
}
static SYSV int qx_android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    char buf[2048];
    sysv_va ap;
    (void)prio;
    __builtin_sysv_va_start(ap, fmt);
    qx_vformat(buf, sizeof(buf), fmt, ap);
    __builtin_sysv_va_end(ap);
    qx_log("[roblox] %s: %s", tag ? tag : "", buf);
    return 0;
}
static SYSV void qx_android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char buf[2048];
    sysv_va ap;
    if (fmt) {
        __builtin_sysv_va_start(ap, fmt);
        qx_vformat(buf, sizeof(buf), fmt, ap);
        __builtin_sysv_va_end(ap);
    } else {
        snprintf(buf, sizeof(buf), "assertion failed: %s", cond ? cond : "?");
    }
    qx_err("[roblox] %s: %s", tag ? tag : "", buf);
    ExitProcess(134);
}
/* vasprintf(strp, fmt, ap): measure with a copy, allocate, fill. */
static SYSV int qx_vasprintf(char **out, const char *fmt, sysv_va ap)
{
    sysv_va cp;
    size_t need;
    char *buf;
    /* measure with a copy, then fill from the original. The SysV va_list is a
     * one-element __va_list_tag array, so a struct copy duplicates the cursor;
     * SysV va_end is a no-op, so the copy needs no explicit end. */
    cp[0] = ap[0];
    need = qx_vformat(NULL, 0, fmt, cp);
    buf = (char *)malloc(need + 1);
    if (!buf) {
        if (out)
            *out = NULL;
        return -1;
    }
    qx_vformat(buf, need + 1, fmt, ap);
    if (out)
        *out = buf;
    return (int)need;
}

/* ---- resolver -------------------------------------------------------------- */
struct fmt_sym {
    const char *name;
    void *fn;
};
static const struct fmt_sym g_fmt_table[] = {
    {"snprintf", (void *)qx_snprintf},
    {"vsnprintf", (void *)qx_vsnprintf},
    {"__vsnprintf_chk", (void *)qx_vsnprintf_chk},
    {"__vsprintf_chk", (void *)qx_vsprintf_chk},
    {"printf", (void *)qx_printf},
    {"syslog", (void *)qx_syslog},
    {"__android_log_print", (void *)qx_android_log_print},
    {"__android_log_assert", (void *)qx_android_log_assert},
    {"vasprintf", (void *)qx_vasprintf},
};

void *qx_format_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_fmt_table) / sizeof(g_fmt_table[0]); i++)
        if (strcmp(name, g_fmt_table[i].name) == 0)
            return g_fmt_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------
 * Exercises the SysV-variadic formatter the way the engine calls it: the checks
 * run inside this SysV function, so the arguments are passed System V style (the
 * sysv->sysv call the engine makes), while this entry itself is non-variadic and
 * safe to call from the Microsoft-ABI main(). Returns the failure count and, via
 * *total, the number of checks. Covers the LP64 "%ld" width, '*' width/precision,
 * truncation with the would-be return length, and the oversized (heap) render. */
/* test helper: a variadic front to the va_list-taking qx_vasprintf */
static SYSV int selftest_asprintf(char **out, const char *fmt, ...)
{
    sysv_va ap;
    int r;
    __builtin_sysv_va_start(ap, fmt);
    r = qx_vasprintf(out, fmt, ap);
    __builtin_sysv_va_end(ap);
    return r;
}

SYSV int qx_format_selftest(int *total)
{
    char b[4096];
    int fails = 0, n = 0;
    char *heap;
    int r;

#define CK(fmtcall, want)                                                      \
    do {                                                                       \
        n++;                                                                   \
        fmtcall;                                                               \
        if (strcmp(b, (want)) != 0)                                            \
            fails++;                                                           \
    } while (0)

    CK(qx_snprintf(b, sizeof(b), "%d", 42), "42");
    CK(qx_snprintf(b, sizeof(b), "%i %u", -7, 7u), "-7 7");
    CK(qx_snprintf(b, sizeof(b), "%x %X %o", 255, 255, 8), "ff FF 10");
    CK(qx_snprintf(b, sizeof(b), "%5d|%-5d|%05d|%+d", 42, 42, 42, 42), "   42|42   |00042|+42");
    CK(qx_snprintf(b, sizeof(b), "%ld", (long long)4294967296LL), "4294967296");
    CK(qx_snprintf(b, sizeof(b), "%lld", (long long)-5000000000LL), "-5000000000");
    CK(qx_snprintf(b, sizeof(b), "%zu", (long long)123456789012ULL), "123456789012");
    CK(qx_snprintf(b, sizeof(b), "%c%c%c", 'R', 'B', 'X'), "RBX");
    CK(qx_snprintf(b, sizeof(b), "[%s]", "hello"), "[hello]");
    CK(qx_snprintf(b, sizeof(b), "%.10s", "truncatedstring"), "truncateds");
    CK(qx_snprintf(b, sizeof(b), "%8.2f", 3.14159), "    3.14");
    CK(qx_snprintf(b, sizeof(b), "%e", 12345.678), "1.234568e+04");
    CK(qx_snprintf(b, sizeof(b), "%g", 0.0001), "0.0001");
    CK(qx_snprintf(b, sizeof(b), "%*d", 6, 42), "    42");
    CK(qx_snprintf(b, sizeof(b), "%.*f", 3, 2.5), "2.500");
    CK(qx_snprintf(b, sizeof(b), "100%%done"), "100%done");
    CK(qx_snprintf(b, sizeof(b), "%s=%d, %s=%.1f", "x", 10, "y", 2.5), "x=10, y=2.5");

    /* truncation: writes cap-1 + NUL, returns the would-be length */
    n++;
    r = qx_snprintf(b, 8, "%s", "abcdefghij");
    if (strcmp(b, "abcdefg") != 0 || r != 10)
        fails++;

    /* oversized field: 500 wide, past the 256-byte stack temp -> heap render */
    n++;
    heap = (char *)malloc(600);
    if (heap) {
        qx_snprintf(heap, 600, "%500s", "Z");
        if (strlen(heap) != 500 || heap[0] != ' ' || heap[499] != 'Z')
            fails++;
        free(heap);
    } else {
        fails++;
    }

    /* vasprintf allocates to fit */
    n++;
    heap = NULL;
    r = selftest_asprintf(&heap, "val=%d/%s", 99, "ok");
    if (!heap || strcmp(heap, "val=99/ok") != 0 || r != 9)
        fails++;
    free(heap);

#undef CK
    if (total)
        *total = n;
    return fails;
}
