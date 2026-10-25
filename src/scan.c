/*
 * Quartix - the scanf family for the Android engine, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * sscanf, fscanf and vsscanf, implemented over a small character source so one
 * scanner serves both a string (sscanf/vsscanf) and a FILE* (fscanf). As with
 * the printf family, the engine passes its variadic result pointers System V
 * style, so the variadic entry points read __builtin_sysv_va_list through the
 * sysv-specific builtins and are sysv_abi + noinline; the scanner core takes the
 * SysV va_list and pulls a pointer per conversion.
 *
 * LP64: the engine's "l" length means a 64-bit store (long is 32-bit on this
 * Win64 host), so %ld writes 8 bytes. Supported conversions: d i u o x X,
 * f e g a (with l/L), s, c, [set], n and %%, with optional assignment
 * suppression (*), field width and length modifiers.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi, noinline, noclone))
typedef __builtin_sysv_va_list sysv_va;

/* character source: a string (sp/sp0) or a stream (f) */
struct sc {
    const char *sp;
    const char *sp0;
    FILE *f;
    int nread;
};
static int sc_get(struct sc *c)
{
    int ch;
    if (c->f)
        ch = fgetc(c->f);
    else
        ch = *c->sp ? (unsigned char)*c->sp++ : -1;
    if (ch != -1)
        c->nread++;
    return ch;
}
static void sc_unget(struct sc *c, int ch)
{
    if (ch == -1)
        return;
    c->nread--;
    if (c->f)
        ungetc(ch, c->f);
    else if (c->sp > c->sp0)
        c->sp--;
}

static int is_ws(int ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }

static void skip_ws(struct sc *c)
{
    int ch;
    while ((ch = sc_get(c)) != -1 && is_ws(ch))
        ;
    sc_unget(c, ch);
}

enum sk { SK_NONE, SK_HH, SK_H, SK_L64, SK_LDBL };

/* store an integer value to the suppressible pointer per length */
static void store_int(sysv_va ap, int suppress, enum sk len, long long v)
{
    void *p;
    if (suppress)
        return;
    p = __builtin_va_arg(ap, void *);
    switch (len) {
    case SK_HH: *(signed char *)p = (signed char)v; break;
    case SK_H:  *(short *)p = (short)v; break;
    case SK_L64: *(int64_t *)p = (int64_t)v; break;   /* LP64 long and ll */
    default:    *(int *)p = (int)v; break;
    }
}
static void store_flt(sysv_va ap, int suppress, enum sk len, long double v)
{
    void *p;
    if (suppress)
        return;
    p = __builtin_va_arg(ap, void *);
    if (len == SK_LDBL)
        *(long double *)p = v;
    else if (len == SK_L64)
        *(double *)p = (double)v;
    else
        *(float *)p = (float)v;
}

/* gather a numeric token (bounded by width) into buf; returns length */
static int gather_num(struct sc *c, char *buf, int cap, int width, int base, int allow_sign)
{
    int n = 0, ch, seen_x = 0;
    ch = sc_get(c);
    if (allow_sign && (ch == '+' || ch == '-')) {
        if (n < cap - 1)
            buf[n++] = (char)ch;
        if (width && n >= width)
            goto done;
        ch = sc_get(c);
    }
    for (;;) {
        int ok = 0;
        if (ch == -1)
            break;
        if (width && n >= width)
            break;
        if ((base == 16 || base == 0) && (n == (allow_sign && (buf[0] == '+' || buf[0] == '-') ? 2 : 1)) &&
            (ch == 'x' || ch == 'X') && n >= 1 && buf[n - 1] == '0' && !seen_x) {
            seen_x = 1;
            ok = 1;
        } else if (base == 16 || base == 0) {
            ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            if (base == 0 && !seen_x && n >= 1)
                ok = (ch >= '0' && ch <= '9'); /* decimal unless 0x seen */
        } else if (base == 8) {
            ok = (ch >= '0' && ch <= '7');
        } else {
            ok = (ch >= '0' && ch <= '9');
        }
        if (!ok)
            break;
        if (n < cap - 1)
            buf[n++] = (char)ch;
        ch = sc_get(c);
    }
    sc_unget(c, ch);
done:
    buf[n] = '\0';
    return n;
}

/* gather a floating token (sign, digits, '.', exponent), bounded by width */
static int gather_flt(struct sc *c, char *buf, int cap, int width)
{
    int n = 0, ch;
    ch = sc_get(c);
    if (ch == '+' || ch == '-') {
        if (n < cap - 1)
            buf[n++] = (char)ch;
        ch = sc_get(c);
    }
    while (ch != -1 && (!width || n < width) &&
           ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' ||
            ch == '+' || ch == '-')) {
        if (n < cap - 1)
            buf[n++] = (char)ch;
        ch = sc_get(c);
    }
    sc_unget(c, ch);
    buf[n] = '\0';
    return n;
}

static int qx_vscan(struct sc *c, const char *fmt, sysv_va ap)
{
    const char *p = fmt;
    int assigned = 0;
    int any_input = 0;

    while (*p) {
        if (is_ws(*p)) {
            skip_ws(c);
            p++;
            continue;
        }
        if (*p != '%') {
            int ch = sc_get(c);
            if (ch == -1)
                return assigned ? assigned : -1;
            any_input = 1;
            if (ch != (unsigned char)*p) {
                sc_unget(c, ch);
                return assigned;
            }
            p++;
            continue;
        }
        /* a conversion */
        p++;
        if (*p == '%') {
            int ch;
            skip_ws(c);
            ch = sc_get(c);
            if (ch != '%') {
                sc_unget(c, ch);
                return assigned;
            }
            p++;
            continue;
        }
        {
            int suppress = 0, width = 0;
            enum sk len = SK_NONE;
            char buf[128];
            char conv;

            if (*p == '*') {
                suppress = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
            if (p[0] == 'h' && p[1] == 'h') { len = SK_HH; p += 2; }
            else if (p[0] == 'h') { len = SK_H; p += 1; }
            else if (p[0] == 'l' && p[1] == 'l') { len = SK_L64; p += 2; }
            else if (p[0] == 'l') { len = SK_L64; p += 1; }
            else if (p[0] == 'L') { len = SK_LDBL; p += 1; }
            else if (p[0] == 'z' || p[0] == 'j' || p[0] == 't' || p[0] == 'q') { len = SK_L64; p += 1; }
            conv = *p;
            if (!conv)
                break;
            p++;

            switch (conv) {
            case 'd': case 'u': case 'i': case 'o': case 'x': case 'X': {
                int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o') ? 8 : (conv == 'i') ? 0 : 10;
                int got;
                skip_ws(c);
                got = gather_num(c, buf, (int)sizeof(buf), width, base, 1);
                if (got == 0)
                    return assigned ? assigned : (any_input ? assigned : -1);
                any_input = 1;
                if (conv == 'u')
                    store_int(ap, suppress, len, (long long)strtoull(buf, NULL, 10));
                else
                    store_int(ap, suppress, len, strtoll(buf, NULL, base));
                if (!suppress)
                    assigned++;
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                int got;
                skip_ws(c);
                got = gather_flt(c, buf, (int)sizeof(buf), width);
                if (got == 0)
                    return assigned ? assigned : (any_input ? assigned : -1);
                any_input = 1;
                store_flt(ap, suppress, len, (long double)strtod(buf, NULL));
                if (!suppress)
                    assigned++;
                break;
            }
            case 's': {
                char *dst = suppress ? NULL : __builtin_va_arg(ap, char *);
                int ch, w = width ? width : 0x7fffffff, n = 0;
                skip_ws(c);
                ch = sc_get(c);
                if (ch == -1)
                    return assigned ? assigned : -1;
                while (ch != -1 && !is_ws(ch) && n < w) {
                    if (dst)
                        dst[n] = (char)ch;
                    n++;
                    ch = sc_get(c);
                }
                sc_unget(c, ch);
                any_input = 1;
                if (dst)
                    dst[n] = '\0';
                if (!suppress)
                    assigned++;
                break;
            }
            case 'c': {
                char *dst = suppress ? NULL : __builtin_va_arg(ap, char *);
                int w = width ? width : 1, n = 0, ch;
                while (n < w && (ch = sc_get(c)) != -1) {
                    if (dst)
                        dst[n] = (char)ch;
                    n++;
                }
                if (n == 0)
                    return assigned ? assigned : -1;
                any_input = 1;
                if (!suppress)
                    assigned++;
                break;
            }
            case '[': {
                unsigned char set[256];
                int negate = 0, ch, w = width ? width : 0x7fffffff, n = 0;
                char *dst = suppress ? NULL : __builtin_va_arg(ap, char *);
                memset(set, 0, sizeof(set));
                if (*p == '^') { negate = 1; p++; }
                if (*p == ']') { set[(unsigned char)']'] = 1; p++; }
                while (*p && *p != ']') {
                    if (p[1] == '-' && p[2] && p[2] != ']') {
                        int lo = (unsigned char)p[0], hi = (unsigned char)p[2], k;
                        if (lo <= hi)
                            for (k = lo; k <= hi; k++)
                                set[k] = 1;
                        else { set[lo] = 1; set[(unsigned char)'-'] = 1; set[hi] = 1; }
                        p += 3;
                    } else {
                        set[(unsigned char)*p] = 1;
                        p++;
                    }
                }
                if (*p == ']')
                    p++;
                ch = sc_get(c);
                while (ch != -1 && n < w && (negate ? !set[ch] : set[ch])) {
                    if (dst)
                        dst[n] = (char)ch;
                    n++;
                    ch = sc_get(c);
                }
                sc_unget(c, ch);
                if (n == 0)
                    return assigned ? assigned : (any_input ? assigned : -1);
                any_input = 1;
                if (dst)
                    dst[n] = '\0';
                if (!suppress)
                    assigned++;
                break;
            }
            case 'n': {
                if (!suppress)
                    store_int(ap, 0, len, (long long)c->nread);
                break;
            }
            default:
                return assigned;
            }
        }
    }
    return assigned;
}

/* ---- entry points --------------------------------------------------------- */
static SYSV int qx_sscanf(const char *str, const char *fmt, ...)
{
    struct sc c;
    sysv_va ap;
    int r;
    c.sp = c.sp0 = str ? str : "";
    c.f = NULL;
    c.nread = 0;
    __builtin_sysv_va_start(ap, fmt);
    r = qx_vscan(&c, fmt, ap);
    __builtin_sysv_va_end(ap);
    return r;
}
static SYSV int qx_vsscanf(const char *str, const char *fmt, sysv_va ap)
{
    struct sc c;
    c.sp = c.sp0 = str ? str : "";
    c.f = NULL;
    c.nread = 0;
    return qx_vscan(&c, fmt, ap);
}
static SYSV int qx_fscanf(void *stream, const char *fmt, ...)
{
    struct sc c;
    sysv_va ap;
    int r;
    c.sp = c.sp0 = NULL;
    c.f = (FILE *)stream;
    c.nread = 0;
    __builtin_sysv_va_start(ap, fmt);
    r = qx_vscan(&c, fmt, ap);
    __builtin_sysv_va_end(ap);
    return r;
}

/* ---- resolver -------------------------------------------------------------- */
struct scan_sym {
    const char *name;
    void *fn;
};
static const struct scan_sym g_scan_table[] = {
    {"sscanf", (void *)qx_sscanf},
    {"vsscanf", (void *)qx_vsscanf},
    {"fscanf", (void *)qx_fscanf},
};

void *qx_scan_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_scan_table) / sizeof(g_scan_table[0]); i++)
        if (strcmp(name, g_scan_table[i].name) == 0)
            return g_scan_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------
 * Exercises sscanf the way the engine calls it (SysV->SysV variadic). */
SYSV int qx_scan_selftest(int *total)
{
    int fails = 0, n = 0;

    n++;
    { int a = 0, b = 0, r = qx_sscanf("12 34", "%d %d", &a, &b);
      if (r != 2 || a != 12 || b != 34) fails++; }
    n++;
    { int64_t v = 0; int r = qx_sscanf("4294967296", "%ld", &v);     /* LP64 %ld */
      if (r != 1 || v != 4294967296LL) fails++; }
    n++;
    { unsigned u = 0; int r = qx_sscanf("0xff", "%x", &u);
      if (r != 1 || u != 255) fails++; }
    n++;
    { double d = 0; int r = qx_sscanf("3.14159", "%lf", &d);
      if (r != 1 || d < 3.1415 || d > 3.1416) fails++; }
    n++;
    { char s[32] = {0}; int x = 0; int r = qx_sscanf("hello 42", "%s %d", s, &x);
      if (r != 2 || strcmp(s, "hello") != 0 || x != 42) fails++; }
    n++;
    { char w[8] = {0}; int r = qx_sscanf("abcdef", "%3c", w);        /* %Nc, no NUL */
      if (r != 1 || memcmp(w, "abc", 3) != 0) fails++; }
    n++;
    { int a = 0; char rest[16] = {0}; int r = qx_sscanf("7:tail", "%d:%s", &a, rest);
      if (r != 2 || a != 7 || strcmp(rest, "tail") != 0) fails++; }
    n++;
    { char set[16] = {0}; int r = qx_sscanf("abc123", "%[a-c]", set); /* literal set a,b,c,- */
      if (r != 1 || strcmp(set, "abc") != 0) fails++; }
    n++;
    { int a = 0, b = 0, cnt = 0; int r = qx_sscanf("5 6", "%d %d%n", &a, &b, &cnt);
      if (r != 2 || a != 5 || b != 6 || cnt != 3) fails++; }
    n++;
    { int a = 0; int r = qx_sscanf("", "%d", &a);                    /* no input -> EOF */
      if (r != -1) fails++; }

    if (total)
        *total = n;
    return fails;
}
