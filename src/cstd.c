/*
 * Quartix - C standard library completion for the Android engine, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * The rest of the pure libc/libm surface the engine imports, below the stdio,
 * string, time and thread slices already in place:
 *
 *   - libm: the float variants (sinf/cosf/powf/...), the hyperbolics
 *     (sinh/cosh/tanh and their floats), and the rounding/decomposition calls
 *     (lround/llround, modf, frexpf, ...). All forward to the host C library.
 *   - the bionic _FORTIFY_SOURCE helpers (__memcpy_chk, __strcpy_chk,
 *     __fread_chk, the fd_set _chk macros, ...): the real operation guarded by
 *     the caller-supplied destination size, aborting on overflow exactly as
 *     bionic does.
 *   - the wide/multibyte and ctype helpers (mbrtowc/wcrtomb done as real UTF-8,
 *     btowc/wctob, the _l ctype classifiers), and stdlib odds and ends
 *     (atoll, strnlen, strcasecmp, rand).
 *   - the C++/runtime glue the engine's initializers touch: __cxa_atexit /
 *     __cxa_finalize (a small LIFO registry), __assert, _Exit/_exit,
 *     arc4random_buf (CryptGenRandom), and the no-op-on-Windows calls
 *     (__register_atfork, getauxval, __system_property_get).
 *
 * Every engine-callable function is sysv_abi. LP64 vs LLP64 matters in two
 * places: the engine's `long` is 64-bit, so lround/llround return a 64-bit
 * value, and its wchar_t is 32-bit (mingw's is 16-bit), so the multibyte
 * conversions read and write 32-bit code points through uint32_t, never the
 * host wchar_t.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <wincrypt.h>

#define SYSV __attribute__((sysv_abi))

/* ---- fatal (fortify overflow / assert) ------------------------------------ */
static void chk_fatal(const char *what)
{
    qx_err("fortify check failed: %s", what);
    ExitProcess(134); /* matches abort()'s 128+SIGABRT */
}

/* ---- libm: forward to the host C library ---------------------------------- */
static SYSV float qx_sinf(float x) { return sinf(x); }
static SYSV float qx_cosf(float x) { return cosf(x); }
static SYSV float qx_tanf(float x) { return tanf(x); }
static SYSV float qx_asinf(float x) { return asinf(x); }
static SYSV float qx_acosf(float x) { return acosf(x); }
static SYSV float qx_atanf(float x) { return atanf(x); }
static SYSV float qx_atan2f(float y, float x) { return atan2f(y, x); }
static SYSV float qx_expf(float x) { return expf(x); }
static SYSV float qx_exp2f(float x) { return exp2f(x); }
static SYSV float qx_logf(float x) { return logf(x); }
static SYSV float qx_log2f(float x) { return log2f(x); }
static SYSV float qx_log10f(float x) { return log10f(x); }
static SYSV double qx_log10(double x) { return log10(x); }
static SYSV float qx_powf(float x, float y) { return powf(x, y); }
static SYSV long double qx_powl(long double x, long double y) { return powl(x, y); }
static SYSV long double qx_fmal(long double a, long double b, long double c) { return fmal(a, b, c); }
static SYSV float qx_fmodf(float x, float y) { return fmodf(x, y); }
static SYSV float qx_floorf(float x) { return floorf(x); }
static SYSV float qx_ceilf(float x) { return ceilf(x); }
static SYSV float qx_cbrtf(float x) { return cbrtf(x); }
static SYSV double qx_cbrt(double x) { return cbrt(x); }
static SYSV float qx_hypotf(float x, float y) { return hypotf(x, y); }
static SYSV double qx_hypot(double x, double y) { return hypot(x, y); }
static SYSV float qx_remainderf(float x, float y) { return remainderf(x, y); }
static SYSV float qx_nextafterf(float x, float y) { return nextafterf(x, y); }
static SYSV double qx_expm1(double x) { return expm1(x); }
static SYSV float qx_erff(float x) { return erff(x); }
static SYSV float qx_erfcf(float x) { return erfcf(x); }
static SYSV double qx_sinh(double x) { return sinh(x); }
static SYSV float qx_sinhf(float x) { return sinhf(x); }
static SYSV double qx_cosh(double x) { return cosh(x); }
static SYSV float qx_coshf(float x) { return coshf(x); }
static SYSV double qx_tanh(double x) { return tanh(x); }
static SYSV float qx_tanhf(float x) { return tanhf(x); }
static SYSV float qx_frexpf(float x, int *e) { return frexpf(x, e); }
static SYSV float qx_ldexpf(float x, int e) { return ldexpf(x, e); }
static SYSV double qx_modf(double x, double *ip) { return modf(x, ip); }
static SYSV float qx_modff(float x, float *ip) { return modff(x, ip); }
static SYSV int qx_ilogb(double x) { return ilogb(x); }
/* LP64: long is 64-bit on the engine side, so lround returns a 64-bit value;
 * compute with the 64-bit host routine so large magnitudes do not truncate. */
static SYSV int64_t qx_lround(double x) { return (int64_t)llround(x); }
static SYSV int64_t qx_lroundf(float x) { return (int64_t)llroundf(x); }
static SYSV int64_t qx_llround(double x) { return (int64_t)llround(x); }
static SYSV int64_t qx_llroundf(float x) { return (int64_t)llroundf(x); }
static SYSV void qx_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* ---- fortified string/memory --------------------------------------------- */
static SYSV void *qx_memcpy_chk(void *d, const void *s, size_t n, size_t dn)
{
    if (n > dn)
        chk_fatal("memcpy");
    return memcpy(d, s, n);
}
static SYSV void *qx_memmove_chk(void *d, const void *s, size_t n, size_t dn)
{
    if (n > dn)
        chk_fatal("memmove");
    return memmove(d, s, n);
}
static SYSV void *qx_memset_chk(void *d, int c, size_t n, size_t dn)
{
    if (n > dn)
        chk_fatal("memset");
    return memset(d, c, n);
}
static SYSV char *qx_strcpy_chk(char *d, const char *s, size_t dn)
{
    if (strlen(s) + 1 > dn)
        chk_fatal("strcpy");
    return strcpy(d, s);
}
static SYSV char *qx_strcat_chk(char *d, const char *s, size_t dn)
{
    if (strlen(d) + strlen(s) + 1 > dn)
        chk_fatal("strcat");
    return strcat(d, s);
}
static SYSV char *qx_strncpy_chk(char *d, const char *s, size_t n, size_t dn)
{
    if (n > dn)
        chk_fatal("strncpy");
    return strncpy(d, s, n);
}
static SYSV char *qx_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn)
{
    (void)sn;
    if (n > dn)
        chk_fatal("strncpy");
    return strncpy(d, s, n);
}
static SYSV size_t qx_strlen_chk(const char *s, size_t sn)
{
    size_t l = strlen(s);
    if (l >= sn)
        chk_fatal("strlen");
    return l;
}
static SYSV char *qx_strchr_chk(const char *s, int ch, size_t sn)
{
    size_t i;
    for (i = 0; i < sn; i++) {
        if (s[i] == (char)ch)
            return (char *)(s + i);
        if (s[i] == '\0')
            return NULL;
    }
    chk_fatal("strchr");
    return NULL;
}
/* read/write/fread/fwrite are owned by src/stdio.c; forward through the
 * resolver so the single implementation there stays the only one. */
typedef SYSV int64_t (*io_fn)(int, void *, size_t);
typedef SYSV size_t (*frw_fn)(void *, size_t, size_t, void *);
static SYSV int64_t qx_read_chk(int fd, void *buf, size_t n, size_t bn)
{
    io_fn rd = (io_fn)qx_stdio_resolve("read");
    if (n > bn)
        chk_fatal("read");
    return rd ? rd(fd, buf, n) : -1;
}
static SYSV int64_t qx_write_chk(int fd, void *buf, size_t n, size_t bn)
{
    io_fn wr = (io_fn)qx_stdio_resolve("write");
    if (n > bn)
        chk_fatal("write");
    return wr ? wr(fd, buf, n) : -1;
}
static SYSV size_t qx_fread_chk(void *buf, size_t size, size_t count, void *stream, size_t bn)
{
    frw_fn fr = (frw_fn)qx_stdio_resolve("fread");
    if (size && count > bn / size)
        chk_fatal("fread");
    return fr ? fr(buf, size, count, stream) : 0;
}
static SYSV size_t qx_fwrite_chk(void *buf, size_t size, size_t count, void *stream, size_t bn)
{
    frw_fn fw = (frw_fn)qx_stdio_resolve("fwrite");
    if (size && count > bn / size)
        chk_fatal("fwrite");
    return fw ? fw(buf, size, count, stream) : 0;
}

/* fd_set is the Linux layout: a bit array of longs. The _chk forms bounds-check
 * the descriptor against the caller's set size before touching the bit. */
#define LX_FD_BITS 1024
static SYSV void qx_fd_set_chk(int fd, uint64_t *set, size_t setsz)
{
    if (fd < 0 || (size_t)fd >= setsz * 8 || fd >= LX_FD_BITS)
        chk_fatal("FD_SET");
    set[fd / 64] |= (uint64_t)1 << (fd % 64);
}
static SYSV void qx_fd_clr_chk(int fd, uint64_t *set, size_t setsz)
{
    if (fd < 0 || (size_t)fd >= setsz * 8 || fd >= LX_FD_BITS)
        chk_fatal("FD_CLR");
    set[fd / 64] &= ~((uint64_t)1 << (fd % 64));
}
static SYSV int qx_fd_isset_chk(int fd, uint64_t *set, size_t setsz)
{
    if (fd < 0 || (size_t)fd >= setsz * 8 || fd >= LX_FD_BITS)
        chk_fatal("FD_ISSET");
    return (set[fd / 64] >> (fd % 64)) & 1;
}

/* ---- wide / multibyte (UTF-8; engine wchar_t is 32-bit) ------------------- */
/* Decode one UTF-8 sequence from s (up to n bytes). Returns bytes consumed, or
 * (size_t)-1 on an invalid sequence, (size_t)-2 if n bytes are incomplete, 0 at
 * a NUL. The code point is written to *out when out is non-NULL. */
static SYSV size_t qx_mbrtowc(uint32_t *out, const char *s, size_t n, void *st)
{
    unsigned char c;
    uint32_t cp;
    int extra, i;
    (void)st;
    if (!s)
        return 0;
    if (n == 0)
        return (size_t)-2;
    c = (unsigned char)s[0];
    if (c < 0x80) {
        if (out)
            *out = c;
        return c ? 1 : 0;
    }
    if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
    else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
    else return (size_t)-1;
    if (n < (size_t)(extra + 1))
        return (size_t)-2;
    for (i = 1; i <= extra; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            return (size_t)-1;
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3f);
    }
    if (out)
        *out = cp;
    return extra + 1;
}
static SYSV size_t qx_wcrtomb(char *s, uint32_t wc, void *st)
{
    (void)st;
    if (!s)
        return 1;
    if (wc < 0x80) {
        s[0] = (char)wc;
        return 1;
    } else if (wc < 0x800) {
        s[0] = (char)(0xc0 | (wc >> 6));
        s[1] = (char)(0x80 | (wc & 0x3f));
        return 2;
    } else if (wc < 0x10000) {
        s[0] = (char)(0xe0 | (wc >> 12));
        s[1] = (char)(0x80 | ((wc >> 6) & 0x3f));
        s[2] = (char)(0x80 | (wc & 0x3f));
        return 3;
    }
    s[0] = (char)(0xf0 | (wc >> 18));
    s[1] = (char)(0x80 | ((wc >> 12) & 0x3f));
    s[2] = (char)(0x80 | ((wc >> 6) & 0x3f));
    s[3] = (char)(0x80 | (wc & 0x3f));
    return 4;
}
static SYSV int qx_btowc(int c) { return (c >= 0 && c < 0x80) ? c : -1; }
static SYSV int qx_wctob(uint32_t c) { return (c < 0x80) ? (int)c : -1; }
static SYSV int qx_ctype_mb_cur_max(void) { return 4; } /* C.UTF-8 */

/* wide ctype, locale variant (the locale_t is ignored). The code point is
 * 32-bit; ASCII is classified exactly, above that best-effort false. */
static SYSV int qx_iswalpha_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? isalpha((int)c) != 0 : 0; }
static SYSV int qx_iswdigit_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? isdigit((int)c) != 0 : 0; }
static SYSV int qx_iswspace_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? isspace((int)c) != 0 : 0; }
static SYSV uint32_t qx_towlower_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? (uint32_t)tolower((int)c) : c; }
static SYSV uint32_t qx_towupper_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? (uint32_t)toupper((int)c) : c; }

/* ---- stdlib odds and ends -------------------------------------------------- */
static SYSV long long qx_atoll(const char *s) { return _atoi64(s); }
static SYSV size_t qx_strnlen(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++)
        ;
    return i;
}
static SYSV int qx_strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
static SYSV int qx_strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
static SYSV int qx_rand(void) { return rand(); }
static SYSV void qx_srand(unsigned s) { srand(s); }

/* ---- runtime glue --------------------------------------------------------- */
static SYSV void qx__Exit(int code) { ExitProcess((UINT)code); }

/* __cxa_atexit: a small LIFO registry run by __cxa_finalize. The engine's
 * BIND_NOW initializers register C++ static destructors here. */
struct atexit_ent {
    void (SYSV *fn)(void *);
    void *arg;
    void *dso;
    int live;
};
static struct atexit_ent g_atexit[2048];
static int g_atexit_n;
static CRITICAL_SECTION g_atexit_lock;
static int g_atexit_init;
static void atexit_lock(void)
{
    if (!g_atexit_init) { /* racey only before any thread starts; good enough at init */
        InitializeCriticalSection(&g_atexit_lock);
        g_atexit_init = 1;
    }
    EnterCriticalSection(&g_atexit_lock);
}
static SYSV int qx_cxa_atexit(void (SYSV *fn)(void *), void *arg, void *dso)
{
    int r = 0;
    atexit_lock();
    if (g_atexit_n < (int)(sizeof(g_atexit) / sizeof(g_atexit[0]))) {
        g_atexit[g_atexit_n].fn = fn;
        g_atexit[g_atexit_n].arg = arg;
        g_atexit[g_atexit_n].dso = dso;
        g_atexit[g_atexit_n].live = 1;
        g_atexit_n++;
    } else {
        r = -1; /* registry full: drop (teardown is best-effort at exit) */
    }
    LeaveCriticalSection(&g_atexit_lock);
    return r;
}
static SYSV void qx_cxa_finalize(void *dso)
{
    int i;
    atexit_lock();
    for (i = g_atexit_n - 1; i >= 0; i--) {
        if (g_atexit[i].live && (dso == NULL || g_atexit[i].dso == dso)) {
            void (SYSV *fn)(void *) = g_atexit[i].fn;
            void *arg = g_atexit[i].arg;
            g_atexit[i].live = 0;
            LeaveCriticalSection(&g_atexit_lock);
            if (fn)
                fn(arg);
            atexit_lock();
        }
    }
    LeaveCriticalSection(&g_atexit_lock);
}
static SYSV void qx_assert(const char *file, int line, const char *msg)
{
    qx_err("assert: %s:%d: %s", file ? file : "?", line, msg ? msg : "");
    ExitProcess(134);
}
static SYSV void qx_assert2(const char *file, int line, const char *func, const char *msg)
{
    qx_err("assert: %s:%d: %s: %s", file ? file : "?", line, func ? func : "?", msg ? msg : "");
    ExitProcess(134);
}

/* arc4random_buf: cryptographic random from the Windows CSP (Vista+). */
static HCRYPTPROV g_prov;
static SYSV void qx_arc4random_buf(void *buf, size_t n)
{
    if (!g_prov)
        CryptAcquireContextW(&g_prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT);
    if (g_prov && CryptGenRandom(g_prov, (DWORD)n, (BYTE *)buf))
        return;
    { /* fallback: never leave the buffer unwritten */
        size_t i;
        for (i = 0; i < n; i++)
            ((unsigned char *)buf)[i] = (unsigned char)rand();
    }
}
static SYSV uint32_t qx_arc4random(void)
{
    uint32_t v;
    qx_arc4random_buf(&v, sizeof(v));
    return v;
}

static SYSV int qx_register_atfork(void *a, void *b, void *c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
    return 0; /* Windows has no fork; nothing to register */
}
static SYSV unsigned long qx_getauxval(unsigned long type)
{
    (void)type;
    return 0; /* no auxiliary vector; callers treat 0 as "feature absent" */
}
static SYSV int qx_system_property_get(const char *name, char *value)
{
    (void)name;
    if (value)
        value[0] = '\0';
    return 0; /* property not set */
}

/* ---- resolver -------------------------------------------------------------- */
struct cstd_sym {
    const char *name;
    void *fn;
};
static const struct cstd_sym g_cstd_table[] = {
    /* libm */
    {"sinf", (void *)qx_sinf}, {"cosf", (void *)qx_cosf}, {"tanf", (void *)qx_tanf},
    {"asinf", (void *)qx_asinf}, {"acosf", (void *)qx_acosf}, {"atanf", (void *)qx_atanf},
    {"atan2f", (void *)qx_atan2f}, {"expf", (void *)qx_expf}, {"exp2f", (void *)qx_exp2f},
    {"logf", (void *)qx_logf}, {"log2f", (void *)qx_log2f}, {"log10f", (void *)qx_log10f},
    {"log10", (void *)qx_log10}, {"powf", (void *)qx_powf}, {"powl", (void *)qx_powl},
    {"fmal", (void *)qx_fmal}, {"fmodf", (void *)qx_fmodf}, {"floorf", (void *)qx_floorf},
    {"ceilf", (void *)qx_ceilf}, {"cbrtf", (void *)qx_cbrtf}, {"cbrt", (void *)qx_cbrt},
    {"hypotf", (void *)qx_hypotf}, {"hypot", (void *)qx_hypot}, {"remainderf", (void *)qx_remainderf},
    {"nextafterf", (void *)qx_nextafterf}, {"expm1", (void *)qx_expm1},
    {"erff", (void *)qx_erff}, {"erfcf", (void *)qx_erfcf},
    {"sinh", (void *)qx_sinh}, {"sinhf", (void *)qx_sinhf}, {"cosh", (void *)qx_cosh},
    {"coshf", (void *)qx_coshf}, {"tanh", (void *)qx_tanh}, {"tanhf", (void *)qx_tanhf},
    {"frexpf", (void *)qx_frexpf}, {"ldexpf", (void *)qx_ldexpf}, {"modf", (void *)qx_modf},
    {"modff", (void *)qx_modff}, {"ilogb", (void *)qx_ilogb},
    {"lround", (void *)qx_lround}, {"lroundf", (void *)qx_lroundf},
    {"llround", (void *)qx_llround}, {"llroundf", (void *)qx_llroundf},
    {"sincosf", (void *)qx_sincosf},
    /* fortified */
    {"__memcpy_chk", (void *)qx_memcpy_chk}, {"__memmove_chk", (void *)qx_memmove_chk},
    {"__memset_chk", (void *)qx_memset_chk}, {"__strcpy_chk", (void *)qx_strcpy_chk},
    {"__strcat_chk", (void *)qx_strcat_chk}, {"__strncpy_chk", (void *)qx_strncpy_chk},
    {"__strncpy_chk2", (void *)qx_strncpy_chk2}, {"__strlen_chk", (void *)qx_strlen_chk},
    {"__strchr_chk", (void *)qx_strchr_chk}, {"__read_chk", (void *)qx_read_chk},
    {"__write_chk", (void *)qx_write_chk}, {"__fread_chk", (void *)qx_fread_chk},
    {"__fwrite_chk", (void *)qx_fwrite_chk}, {"__FD_SET_chk", (void *)qx_fd_set_chk},
    {"__FD_CLR_chk", (void *)qx_fd_clr_chk}, {"__FD_ISSET_chk", (void *)qx_fd_isset_chk},
    /* wide / multibyte / ctype */
    {"mbrtowc", (void *)qx_mbrtowc}, {"wcrtomb", (void *)qx_wcrtomb},
    {"btowc", (void *)qx_btowc}, {"wctob", (void *)qx_wctob},
    {"__ctype_get_mb_cur_max", (void *)qx_ctype_mb_cur_max},
    {"iswalpha_l", (void *)qx_iswalpha_l}, {"iswdigit_l", (void *)qx_iswdigit_l},
    {"iswspace_l", (void *)qx_iswspace_l}, {"towlower_l", (void *)qx_towlower_l},
    {"towupper_l", (void *)qx_towupper_l},
    /* stdlib */
    {"atoll", (void *)qx_atoll}, {"strnlen", (void *)qx_strnlen},
    {"strcasecmp", (void *)qx_strcasecmp}, {"strncasecmp", (void *)qx_strncasecmp},
    {"rand", (void *)qx_rand}, {"srand", (void *)qx_srand},
    /* runtime glue */
    {"_Exit", (void *)qx__Exit}, {"_exit", (void *)qx__Exit},
    {"__cxa_atexit", (void *)qx_cxa_atexit}, {"__cxa_finalize", (void *)qx_cxa_finalize},
    {"__assert", (void *)qx_assert}, {"__assert2", (void *)qx_assert2},
    {"arc4random_buf", (void *)qx_arc4random_buf}, {"arc4random", (void *)qx_arc4random},
    {"__register_atfork", (void *)qx_register_atfork}, {"getauxval", (void *)qx_getauxval},
    {"__system_property_get", (void *)qx_system_property_get},
};

void *qx_cstd_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_cstd_table) / sizeof(g_cstd_table[0]); i++)
        if (strcmp(name, g_cstd_table[i].name) == 0)
            return g_cstd_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------ */
static int cstd_test_order[4];
static int cstd_test_idx;
static SYSV void cstd_test_handler_a(void *arg) { (void)arg; cstd_test_order[cstd_test_idx++] = 1; }
static SYSV void cstd_test_handler_b(void *arg) { (void)arg; cstd_test_order[cstd_test_idx++] = 2; }

SYSV int qx_cstd_selftest(int *total)
{
    int fails = 0, n = 0;

    /* float math matches double math to float precision */
    n++;
    if (fabsf(qx_sinf(1.0f) - (float)sin(1.0)) > 1e-6f)
        fails++;
    n++;
    if (fabsf(qx_powf(2.0f, 10.0f) - 1024.0f) > 1e-3f)
        fails++;
    /* hyperbolic */
    n++;
    if (fabs(qx_cosh(0.0) - 1.0) > 1e-9)
        fails++;
    /* LP64 rounding returns a full 64-bit value */
    n++;
    if (qx_llround(2.6) != 3 || qx_lround(-2.6) != -3)
        fails++;
    n++;
    {
        int64_t big = qx_llround(5000000000.4); /* > 2^32 */
        if (big != 5000000000LL)
            fails++;
    }
    /* modf splits integer and fraction */
    n++;
    {
        double ip = 0;
        double fp = qx_modf(3.25, &ip);
        if (ip != 3.0 || fabs(fp - 0.25) > 1e-12)
            fails++;
    }
    /* fortified copy: in-bounds succeeds and copies */
    n++;
    {
        char buf[8];
        qx_memcpy_chk(buf, "hello", 6, sizeof(buf));
        if (strcmp(buf, "hello") != 0)
            fails++;
    }
    n++;
    {
        char buf[8];
        qx_strcpy_chk(buf, "abc", sizeof(buf));
        if (strcmp(buf, "abc") != 0 || qx_strlen_chk(buf, sizeof(buf)) != 3)
            fails++;
    }
    /* fd_set round-trip through the _chk helpers */
    n++;
    {
        uint64_t set[16];
        memset(set, 0, sizeof(set));
        qx_fd_set_chk(5, set, sizeof(set));
        qx_fd_set_chk(130, set, sizeof(set));
        if (!qx_fd_isset_chk(5, set, sizeof(set)) || !qx_fd_isset_chk(130, set, sizeof(set)) ||
            qx_fd_isset_chk(6, set, sizeof(set)))
            fails++;
        qx_fd_clr_chk(5, set, sizeof(set));
        if (qx_fd_isset_chk(5, set, sizeof(set)))
            fails++;
    }
    /* UTF-8 round-trip of a 3-byte code point (€ = U+20AC) */
    n++;
    {
        uint32_t cp = 0;
        char enc[4];
        size_t used = qx_mbrtowc(&cp, "\xe2\x82\xac", 3, NULL);
        size_t m = qx_wcrtomb(enc, cp, NULL);
        if (used != 3 || cp != 0x20AC || m != 3 ||
            (unsigned char)enc[0] != 0xe2 || (unsigned char)enc[1] != 0x82 ||
            (unsigned char)enc[2] != 0xac)
            fails++;
    }
    n++;
    if (qx_btowc('A') != 'A' || qx_btowc(0x80) != -1 || qx_ctype_mb_cur_max() != 4)
        fails++;
    /* stdlib */
    n++;
    if (qx_atoll("-9000000000") != -9000000000LL)
        fails++;
    n++;
    if (qx_strnlen("hello", 3) != 3 || qx_strnlen("hi", 8) != 2 ||
        qx_strcasecmp("ABC", "abc") != 0)
        fails++;
    /* __cxa_atexit / __cxa_finalize run registered handlers LIFO, scoped by dso */
    n++;
    {
        void *dso = (void *)&n;
        cstd_test_idx = 0;
        qx_cxa_atexit(cstd_test_handler_a, NULL, dso);
        qx_cxa_atexit(cstd_test_handler_b, NULL, dso);
        qx_cxa_finalize(dso);
        /* b registered last, so it runs first: order is {2, 1} */
        if (cstd_test_idx != 2 || cstd_test_order[0] != 2 || cstd_test_order[1] != 1)
            fails++;
    }
    /* arc4random_buf fills the buffer (not left all-zero with overwhelming odds) */
    n++;
    {
        unsigned char b[32];
        int i, nonzero = 0;
        memset(b, 0, sizeof(b));
        qx_arc4random_buf(b, sizeof(b));
        for (i = 0; i < 32; i++)
            if (b[i])
                nonzero = 1;
        if (!nonzero)
            fails++;
    }

    if (total)
        *total = n;
    return fails;
}
