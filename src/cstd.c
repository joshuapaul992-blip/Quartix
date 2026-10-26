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
#include <limits.h>
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

/* Cryptographic random from the Windows CSP (Vista+). Host-ABI helper, shared
 * with the /dev/urandom emulation in stdio.c; callable from our own code. */
static HCRYPTPROV g_prov;
void qx_random_bytes(void *buf, size_t n)
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
static SYSV void qx_arc4random_buf(void *buf, size_t n) { qx_random_bytes(buf, n); }
static SYSV uint32_t qx_arc4random(void)
{
    uint32_t v;
    qx_random_bytes(&v, sizeof(v));
    return v;
}
/* getentropy: fill buf with up to 256 random bytes; 0 on success, -1 on EIO. */
static SYSV int qx_getentropy(void *buf, size_t n)
{
    if (n > 256) {
        *qx_shim_errno() = 5; /* EIO */
        return -1;
    }
    qx_random_bytes(buf, n);
    return 0;
}
/* bionic records a human-readable reason just before aborting; surface it. */
static SYSV void qx_android_set_abort_message(const char *msg)
{
    if (msg)
        qx_err("engine abort: %s", msg);
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

/* ---- locale ----------------------------------------------------------------
 * The engine is built against bionic, whose locale support is C/C.UTF-8 only.
 * A locale_t is an opaque handle; we never consult it, so one non-NULL sentinel
 * stands for every locale and the _l calls behave as the C locale. */
static int g_locale_sentinel;
#define QX_LOCALE ((void *)&g_locale_sentinel)

/* NUL-terminated bounded copy (strlcpy semantics), used by the gaps below. */
static void qx_strlcpy_like(char *d, const char *s, size_t n)
{
    size_t i;
    if (!n)
        return;
    for (i = 0; i + 1 < n && s[i]; i++)
        d[i] = s[i];
    d[i] = '\0';
}

static SYSV void *qx_newlocale(int mask, const char *name, void *base)
{
    (void)mask; (void)name; (void)base;
    return QX_LOCALE;
}
static SYSV void qx_freelocale(void *loc) { (void)loc; }
static SYSV void *qx_uselocale(void *loc) { (void)loc; return QX_LOCALE; }
static SYSV void *qx_duplocale(void *loc) { (void)loc; return QX_LOCALE; }

/* bionic's struct lconv, filled for the C locale (CHAR_MAX = "not specified"). */
struct qx_lconv {
    char *decimal_point, *thousands_sep, *grouping;
    char *int_curr_symbol, *currency_symbol;
    char *mon_decimal_point, *mon_thousands_sep, *mon_grouping;
    char *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space;
    char n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    char int_p_cs_precedes, int_p_sep_by_space, int_n_cs_precedes, int_n_sep_by_space;
    char int_p_sign_posn, int_n_sign_posn;
};
static SYSV void *qx_localeconv(void)
{
    static char empty[] = "";
    static char dot[] = ".";
    static struct qx_lconv lc;
    static int init;
    if (!init) {
        lc.decimal_point = dot;
        lc.thousands_sep = lc.grouping = empty;
        lc.int_curr_symbol = lc.currency_symbol = empty;
        lc.mon_decimal_point = lc.mon_thousands_sep = lc.mon_grouping = empty;
        lc.positive_sign = lc.negative_sign = empty;
        lc.int_frac_digits = lc.frac_digits = CHAR_MAX;
        lc.p_cs_precedes = lc.p_sep_by_space = CHAR_MAX;
        lc.n_cs_precedes = lc.n_sep_by_space = CHAR_MAX;
        lc.p_sign_posn = lc.n_sign_posn = CHAR_MAX;
        lc.int_p_cs_precedes = lc.int_p_sep_by_space = CHAR_MAX;
        lc.int_n_cs_precedes = lc.int_n_sep_by_space = CHAR_MAX;
        lc.int_p_sign_posn = lc.int_n_sign_posn = CHAR_MAX;
        init = 1;
    }
    return &lc;
}

/* _l string/number variants: the locale is ignored (C locale). */
static SYSV int qx_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
static SYSV size_t qx_strxfrm_l(char *d, const char *s, size_t n, void *l)
{
    size_t len = strlen(s);
    (void)l;
    if (d && n)
        qx_strlcpy_like(d, s, n);
    return len;
}
static SYSV long long qx_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return _strtoi64(s, e, b); }
static SYSV unsigned long long qx_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return _strtoui64(s, e, b); }
static SYSV long double qx_strtold_l(const char *s, char **e, void *l) { (void)l; return (long double)strtod(s, e); }

/* _l wide ctype: 32-bit code point, ASCII classified exactly, else false. */
static SYSV int qx_iswblank_l(uint32_t c, void *l) { (void)l; return (c == ' ' || c == '\t'); }
static SYSV int qx_iswcntrl_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? iscntrl((int)c) != 0 : 0; }
static SYSV int qx_iswlower_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? islower((int)c) != 0 : 0; }
static SYSV int qx_iswprint_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? isprint((int)c) != 0 : 0; }
static SYSV int qx_iswpunct_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? ispunct((int)c) != 0 : 0; }
static SYSV int qx_iswupper_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? isupper((int)c) != 0 : 0; }
static SYSV int qx_iswxdigit_l(uint32_t c, void *l) { (void)l; return c < 0x80 ? isxdigit((int)c) != 0 : 0; }

/* ---- string gaps ----------------------------------------------------------- */
static SYSV size_t qx_strcspn(const char *s, const char *set) { return strcspn(s, set); }
static SYSV size_t qx_strspn(const char *s, const char *set) { return strspn(s, set); }
static SYSV char *qx_strpbrk(const char *s, const char *set) { return strpbrk(s, set); }
static SYSV char *qx_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static SYSV void *qx_memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    while (n--)
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    return NULL;
}
static SYSV char *qx_strerror(int e) { return strerror(e); }
static SYSV int qx_strerror_r(int e, char *buf, size_t n)
{
    /* POSIX (glibc returns int); bionic uses the XSI form. */
    const char *m = strerror(e);
    if (!buf || n == 0)
        return 22; /* EINVAL */
    qx_strlcpy_like(buf, m, n);
    return 0;
}
/* The GNU variant returns the (possibly internal) string, not an int. */
static SYSV char *qx_gnu_strerror_r(int e, char *buf, size_t n)
{
    (void)qx_strerror_r(e, buf, n);
    return buf;
}

/* ---- ctype (base) ---------------------------------------------------------- */
static SYSV int qx_isspace(int c) { return isspace(c); }
static SYSV int qx_tolower(int c) { return tolower(c); }

/* ---- wide string (engine wchar_t is 32-bit) -------------------------------- */
static SYSV size_t qx_wcslen32(const uint32_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
static SYSV void *qx_wmemchr32(const uint32_t *s, uint32_t c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] == c)
            return (void *)(s + i);
    return NULL;
}
static SYSV int qx_wmemcmp32(const uint32_t *a, const uint32_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
/* wide _l collation, C locale: code-point order over 32-bit wchar_t. */
static SYSV int qx_wcscoll_l(const uint32_t *a, const uint32_t *b, void *l)
{
    (void)l;
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) return 0;
    return *a < *b ? -1 : 1;
}
static SYSV size_t qx_wcsxfrm_l(uint32_t *d, const uint32_t *s, size_t n, void *l)
{
    size_t len = qx_wcslen32(s), i;
    (void)l;
    if (d && n) {
        for (i = 0; i + 1 < n && s[i]; i++)
            d[i] = s[i];
        d[i] = 0;
    }
    return len;
}

/* ---- multibyte (UTF-8 <-> 32-bit code points) ------------------------------ */
static SYSV int qx_mbtowc(uint32_t *out, const char *s, size_t n)
{
    size_t r;
    if (!s)
        return 0;              /* UTF-8 is stateless */
    r = qx_mbrtowc(out, s, n, NULL);
    if (r == (size_t)-1 || r == (size_t)-2)
        return -1;
    return (int)r;
}
static SYSV size_t qx_mbrlen(const char *s, size_t n, void *st)
{
    return qx_mbrtowc(NULL, s, n, st);
}
/* Convert a whole string. On success returns the count of code points written
 * (or that would be written when dst is NULL), advancing *src as POSIX wants. */
static SYSV size_t qx_mbsnrtowcs(uint32_t *dst, const char **src, size_t nms, size_t len, void *st)
{
    size_t out = 0;
    const char *p = *src;
    (void)st;
    while (nms > 0) {
        uint32_t wc;
        size_t r = qx_mbrtowc(&wc, p, nms, NULL);
        if (r == (size_t)-1) return (size_t)-1;
        if (r == (size_t)-2) break;            /* incomplete tail */
        if (dst) {
            if (out >= len) break;
            dst[out] = wc;
        }
        out++;
        if (wc == 0) {                          /* stop at NUL; clear *src */
            p = NULL;
            if (dst) { *src = NULL; return out - 1; }
            return out - 1;
        }
        p += r;
        nms -= r;
        if (dst && out >= len) break;
    }
    *src = p;
    return out;
}
static SYSV size_t qx_mbsrtowcs(uint32_t *dst, const char **src, size_t len, void *st)
{
    return qx_mbsnrtowcs(dst, src, (size_t)-1, len, st);
}
static SYSV size_t qx_wcsnrtombs(char *dst, const uint32_t **src, size_t nwc, size_t len, void *st)
{
    size_t out = 0;
    const uint32_t *p = *src;
    (void)st;
    while (nwc > 0) {
        char tmp[4];
        size_t m = qx_wcrtomb(tmp, *p, NULL);
        if (*p == 0) {
            if (dst && out < len) dst[out] = '\0';
            *src = NULL;
            return out;
        }
        if (dst) {
            if (out + m > len) break;
            memcpy(dst + out, tmp, m);
        }
        out += m;
        p++;
        nwc--;
    }
    *src = p;
    return out;
}

/* ---- stdlib / math gaps ---------------------------------------------------- */
/* ldiv_t is { long quot; long rem } and the engine's long is 64-bit, so this
 * returns a 16-byte struct by value (rax:rdx under SysV). */
struct qx_ldiv { long long quot, rem; };
static SYSV struct qx_ldiv qx_ldiv(long long num, long long den)
{
    struct qx_ldiv r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}
static SYSV double qx_nan_fn(const char *tag) { (void)tag; return (double)NAN; }
static SYSV int qx_finitef(float x) { return isfinite(x) != 0; }
static SYSV float qx_remquof(float x, float y, int *q) { return remquof(x, y, q); }

/* ---- data globals the engine binds directly -------------------------------- *
 * These are referenced as data (GLOB_DAT), so the resolver returns the address
 * of a real object, not a function. */
static char **g_environ;                 /* empty environment */
static uintptr_t g_stack_chk_guard = (uintptr_t)0x2b7e151628aed2a6ull;

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
    {"getentropy", (void *)qx_getentropy}, {"android_set_abort_message", (void *)qx_android_set_abort_message},
    {"__register_atfork", (void *)qx_register_atfork}, {"getauxval", (void *)qx_getauxval},
    {"__system_property_get", (void *)qx_system_property_get},
    /* locale */
    {"newlocale", (void *)qx_newlocale}, {"freelocale", (void *)qx_freelocale},
    {"uselocale", (void *)qx_uselocale}, {"duplocale", (void *)qx_duplocale},
    {"localeconv", (void *)qx_localeconv},
    {"strcoll_l", (void *)qx_strcoll_l}, {"strxfrm_l", (void *)qx_strxfrm_l},
    {"wcscoll_l", (void *)qx_wcscoll_l}, {"wcsxfrm_l", (void *)qx_wcsxfrm_l},
    {"strtoll_l", (void *)qx_strtoll_l}, {"strtoull_l", (void *)qx_strtoull_l},
    {"strtold_l", (void *)qx_strtold_l},
    {"iswblank_l", (void *)qx_iswblank_l}, {"iswcntrl_l", (void *)qx_iswcntrl_l},
    {"iswlower_l", (void *)qx_iswlower_l}, {"iswprint_l", (void *)qx_iswprint_l},
    {"iswpunct_l", (void *)qx_iswpunct_l}, {"iswupper_l", (void *)qx_iswupper_l},
    {"iswxdigit_l", (void *)qx_iswxdigit_l},
    /* string gaps */
    {"strcspn", (void *)qx_strcspn}, {"strspn", (void *)qx_strspn},
    {"strpbrk", (void *)qx_strpbrk}, {"strncat", (void *)qx_strncat},
    {"memrchr", (void *)qx_memrchr}, {"strerror", (void *)qx_strerror},
    {"strerror_r", (void *)qx_strerror_r}, {"__gnu_strerror_r", (void *)qx_gnu_strerror_r},
    /* base ctype */
    {"isspace", (void *)qx_isspace}, {"tolower", (void *)qx_tolower},
    /* wide string (32-bit wchar) */
    {"wcslen", (void *)qx_wcslen32}, {"wmemchr", (void *)qx_wmemchr32},
    {"wmemcmp", (void *)qx_wmemcmp32},
    /* multibyte */
    {"mbtowc", (void *)qx_mbtowc}, {"mbrlen", (void *)qx_mbrlen},
    {"mbsrtowcs", (void *)qx_mbsrtowcs}, {"mbsnrtowcs", (void *)qx_mbsnrtowcs},
    {"wcsnrtombs", (void *)qx_wcsnrtombs},
    /* stdlib / math gaps */
    {"ldiv", (void *)qx_ldiv}, {"nan", (void *)qx_nan_fn},
    {"finitef", (void *)qx_finitef}, {"remquof", (void *)qx_remquof},
    /* data globals (bound as data, not called) */
    {"environ", (void *)&g_environ}, {"__stack_chk_guard", (void *)&g_stack_chk_guard},
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
    /* locale/_l variants behave as the C locale */
    n++;
    if (qx_newlocale(0, "C", NULL) == NULL || qx_strcoll_l("a", "b", NULL) >= 0 ||
        qx_iswupper_l('A', NULL) == 0 || qx_iswlower_l('A', NULL) != 0)
        fails++;
    /* 32-bit wide string helpers (engine wchar_t is 32-bit) */
    n++;
    {
        static const uint32_t w[] = { 'h', 'e', 'l', 'l', 'o', 0 };
        static const uint32_t w2[] = { 'h', 'e', 'l', 'p', 0 };
        if (qx_wcslen32(w) != 5 || qx_wmemchr32(w, 'l', 5) != (void *)(w + 2) ||
            qx_wmemcmp32(w, w2, 4) >= 0)
            fails++;
    }
    /* mbtowc decodes one UTF-8 code point; memrchr finds the last match */
    n++;
    {
        uint32_t cp = 0;
        if (qx_mbtowc(&cp, "A", 1) != 1 || cp != 'A' ||
            qx_memrchr("abcabc", 'b', 6) != (const void *)("abcabc" + 4))
            fails++;
    }
    /* ldiv returns a 64-bit quotient/remainder by value */
    n++;
    {
        struct qx_ldiv d = qx_ldiv(5000000001LL, 1000000000LL);
        if (d.quot != 5 || d.rem != 1)
            fails++;
    }
    /* string gaps */
    n++;
    if (qx_strcspn("hello", "l") != 2 || qx_strspn("aaab", "a") != 3 ||
        qx_strpbrk("hello", "l") != (char *)("hello" + 2))
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
