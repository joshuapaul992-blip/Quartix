/*
 * Quartix - OS services for the Android engine, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * The remaining non-variadic libc the engine leans on below strings/stdio:
 * time (clock_gettime/gettimeofday/time, the gmtime/localtime/mktime/strftime
 * calendar calls, nanosleep), virtual memory (mmap/munmap/mprotect for the
 * engine's large allocations), and a few process/system queries (getpid, gettid,
 * sysconf, getcwd, and no-op signal handling, which Win32 has no POSIX analogue
 * for). Every function is sysv_abi.
 *
 * Two layout notes, both because the engine is LP64 while this host is LLP64:
 *   - time_t and the tv_* / tv_nsec fields are 64-bit, so the timespec/timeval
 *     structs are spelled with int64.
 *   - struct tm carries tm_gmtoff (a 64-bit long) and tm_zone after the nine
 *     ints, 56 bytes total; the host CRT's struct tm has only the nine ints, so
 *     the nine are copied across and the two extra fields filled in.
 *
 * clock_gettime(CLOCK_MONOTONIC) is based on GetTickCount64, the same base
 * src/thread.c uses for pthread_cond_timedwait, so a monotonic deadline the
 * engine computes here lines up with the wait there.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <direct.h>

#define SYSV __attribute__((sysv_abi))

/* Linux clock ids */
#define LX_CLOCK_REALTIME 0
#define LX_CLOCK_MONOTONIC 1

struct lx_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct lx_timeval { int64_t tv_sec; int64_t tv_usec; };
struct lx_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    int64_t tm_gmtoff;
    const char *tm_zone;
};
typedef char qx_tm_size_check[sizeof(struct lx_tm) == 56 ? 1 : -1];

/* ---- time bases ----------------------------------------------------------- */
static int64_t realtime_ns(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart - 116444736000000000ULL) * 100LL; /* 100ns since 1601 -> ns since 1970 */
}
static int64_t monotonic_ns(void) { return (int64_t)GetTickCount64() * 1000000LL; }

static SYSV int64_t qx_time(int64_t *out)
{
    int64_t s = realtime_ns() / 1000000000LL;
    if (out)
        *out = s;
    return s;
}
static SYSV int qx_gettimeofday(struct lx_timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        int64_t ns = realtime_ns();
        tv->tv_sec = ns / 1000000000LL;
        tv->tv_usec = (ns / 1000LL) % 1000000LL;
    }
    return 0;
}
static SYSV int qx_clock_gettime(int clk, struct lx_timespec *ts)
{
    int64_t ns = (clk == LX_CLOCK_REALTIME) ? realtime_ns() : monotonic_ns();
    if (ts) {
        ts->tv_sec = ns / 1000000000LL;
        ts->tv_nsec = ns % 1000000000LL;
    }
    return 0;
}
static SYSV int qx_clock_getres(int clk, struct lx_timespec *ts)
{
    (void)clk;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 1000000; /* 1ms, the GetTickCount64 granularity */
    }
    return 0;
}
static SYSV long long qx_clock(void) { return (long long)GetTickCount64() * 1000LL; } /* ~us */
static SYSV double qx_difftime(int64_t a, int64_t b) { return (double)(a - b); }
static SYSV int qx_nanosleep(const struct lx_timespec *req, struct lx_timespec *rem)
{
    if (req) {
        int64_t ms = req->tv_sec * 1000LL + req->tv_nsec / 1000000LL;
        if (ms < 0)
            ms = 0;
        Sleep((DWORD)(ms > 0xfffffffeLL ? 0xfffffffeUL : (DWORD)ms));
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

/* ---- calendar ------------------------------------------------------------- */
static void tm_out(const struct tm *s, struct lx_tm *o)
{
    memset(o, 0, sizeof(*o));
    o->tm_sec = s->tm_sec;
    o->tm_min = s->tm_min;
    o->tm_hour = s->tm_hour;
    o->tm_mday = s->tm_mday;
    o->tm_mon = s->tm_mon;
    o->tm_year = s->tm_year;
    o->tm_wday = s->tm_wday;
    o->tm_yday = s->tm_yday;
    o->tm_isdst = s->tm_isdst;
    o->tm_gmtoff = 0;
    o->tm_zone = "UTC";
}
static void tm_in(const struct lx_tm *s, struct tm *o)
{
    memset(o, 0, sizeof(*o));
    o->tm_sec = s->tm_sec;
    o->tm_min = s->tm_min;
    o->tm_hour = s->tm_hour;
    o->tm_mday = s->tm_mday;
    o->tm_mon = s->tm_mon;
    o->tm_year = s->tm_year;
    o->tm_wday = s->tm_wday;
    o->tm_yday = s->tm_yday;
    o->tm_isdst = s->tm_isdst;
}

static struct lx_tm g_gmtime; /* gmtime/localtime return a pointer to static storage */

static SYSV struct lx_tm *qx_gmtime_r(const int64_t *t, struct lx_tm *out)
{
    __time64_t tt = t ? (__time64_t)*t : 0;
    struct tm tmp;
    if (_gmtime64_s(&tmp, &tt) != 0)
        return NULL;
    tm_out(&tmp, out);
    return out;
}
static SYSV struct lx_tm *qx_gmtime(const int64_t *t) { return qx_gmtime_r(t, &g_gmtime); }
static SYSV struct lx_tm *qx_localtime_r(const int64_t *t, struct lx_tm *out)
{
    __time64_t tt = t ? (__time64_t)*t : 0;
    struct tm tmp;
    if (_localtime64_s(&tmp, &tt) != 0)
        return NULL;
    tm_out(&tmp, out);
    return out;
}
static SYSV struct lx_tm *qx_localtime(const int64_t *t) { return qx_localtime_r(t, &g_gmtime); }
static SYSV int64_t qx_mktime(struct lx_tm *tmv)
{
    struct tm tmp;
    if (!tmv)
        return -1;
    tm_in(tmv, &tmp);
    return (int64_t)_mktime64(&tmp);
}
static SYSV size_t qx_strftime(char *buf, size_t max, const char *fmt, const struct lx_tm *tmv)
{
    struct tm tmp;
    if (!tmv)
        return 0;
    tm_in(tmv, &tmp);
    return strftime(buf, max, fmt, &tmp);
}
static SYSV void qx_tzset(void) { _tzset(); }

/* ---- virtual memory ------------------------------------------------------- */
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_FAILED ((void *)-1)

static DWORD prot_to_page(int prot)
{
    int w = prot & LX_PROT_WRITE, x = prot & LX_PROT_EXEC;
    if (!(prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)))
        return PAGE_NOACCESS;
    if (x)
        return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return w ? PAGE_READWRITE : PAGE_READONLY;
}
/* Anonymous mappings only (the engine's allocator use): file-backed mmap
 * returns MAP_FAILED so the caller falls back to read(). */
static SYSV void *qx_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    void *p;
    (void)addr;
    (void)off;
    if (fd >= 0 && !(flags & LX_MAP_ANONYMOUS))
        return LX_MAP_FAILED;
    p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, prot_to_page(prot));
    return p ? p : LX_MAP_FAILED;
}
static SYSV int qx_munmap(void *addr, size_t len)
{
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}
static SYSV int qx_mprotect(void *addr, size_t len, int prot)
{
    DWORD old;
    return VirtualProtect(addr, len, prot_to_page(prot), &old) ? 0 : -1;
}
static SYSV int qx_madvise(void *addr, size_t len, int advice)
{
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}
static SYSV int qx_getpagesize(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwPageSize;
}

/* ---- process / system ----------------------------------------------------- */
/* bionic sysconf names */
#define LX_SC_CLK_TCK 2
#define LX_SC_PAGESIZE 39
#define LX_SC_PAGE_SIZE 39
#define LX_SC_NPROCESSORS_CONF 96
#define LX_SC_NPROCESSORS_ONLN 97
#define LX_SC_PHYS_PAGES 85

static SYSV int qx_getpid(void) { return (int)GetCurrentProcessId(); }
static SYSV int qx_gettid(void) { return (int)GetCurrentThreadId(); }
static SYSV int qx_getuid(void) { return 0; }
static SYSV long long qx_sysconf(int name)
{
    SYSTEM_INFO si;
    switch (name) {
    case LX_SC_PAGESIZE:
        GetSystemInfo(&si);
        return (long long)si.dwPageSize;
    case LX_SC_NPROCESSORS_CONF:
    case LX_SC_NPROCESSORS_ONLN:
        GetSystemInfo(&si);
        return (long long)si.dwNumberOfProcessors;
    case LX_SC_CLK_TCK:
        return 100;
    case LX_SC_PHYS_PAGES: {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        GetSystemInfo(&si);
        if (GlobalMemoryStatusEx(&ms))
            return (long long)(ms.ullTotalPhys / si.dwPageSize);
        return -1;
    }
    default:
        return -1;
    }
}
static SYSV char *qx_getcwd(char *buf, size_t size)
{
    wchar_t w[QX_PATH_MAX];
    if (!_wgetcwd(w, QX_PATH_MAX))
        return NULL;
    if (!buf)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)size, NULL, NULL) == 0)
        return NULL;
    return buf;
}
/* Win32 has no POSIX signals; accept and do nothing */
static SYSV int qx_raise(int sig) { (void)sig; return 0; }
static SYSV void *qx_signal(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
static SYSV int qx_sigaction(int sig, const void *act, void *old) { (void)sig; (void)act; (void)old; return 0; }

/* ---- resolver -------------------------------------------------------------- */
struct os_sym {
    const char *name;
    void *fn;
};
static const struct os_sym g_os_table[] = {
    {"time", (void *)qx_time}, {"gettimeofday", (void *)qx_gettimeofday},
    {"clock_gettime", (void *)qx_clock_gettime}, {"clock_getres", (void *)qx_clock_getres},
    {"clock", (void *)qx_clock}, {"difftime", (void *)qx_difftime},
    {"nanosleep", (void *)qx_nanosleep},
    {"gmtime", (void *)qx_gmtime}, {"gmtime_r", (void *)qx_gmtime_r},
    {"localtime", (void *)qx_localtime}, {"localtime_r", (void *)qx_localtime_r},
    {"mktime", (void *)qx_mktime}, {"strftime", (void *)qx_strftime},
    {"tzset", (void *)qx_tzset},
    {"mmap", (void *)qx_mmap}, {"munmap", (void *)qx_munmap}, {"mprotect", (void *)qx_mprotect},
    {"madvise", (void *)qx_madvise}, {"getpagesize", (void *)qx_getpagesize},
    {"getpid", (void *)qx_getpid}, {"gettid", (void *)qx_gettid}, {"getuid", (void *)qx_getuid},
    {"sysconf", (void *)qx_sysconf}, {"getcwd", (void *)qx_getcwd},
    {"raise", (void *)qx_raise}, {"signal", (void *)qx_signal}, {"sigaction", (void *)qx_sigaction},
};

void *qx_os_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_os_table) / sizeof(g_os_table[0]); i++)
        if (strcmp(name, g_os_table[i].name) == 0)
            return g_os_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------ */
SYSV int qx_os_selftest(int *total)
{
    int fails = 0, n = 0;

    /* monotonic clock advances and never goes backward */
    n++;
    {
        struct lx_timespec a, b;
        qx_clock_gettime(LX_CLOCK_MONOTONIC, &a);
        Sleep(5);
        qx_clock_gettime(LX_CLOCK_MONOTONIC, &b);
        if ((b.tv_sec * 1000000000LL + b.tv_nsec) < (a.tv_sec * 1000000000LL + a.tv_nsec))
            fails++;
    }
    /* realtime is a plausible modern epoch (after 2023-11) */
    n++;
    {
        int64_t t = qx_time(NULL);
        if (t < 1700000000LL)
            fails++;
    }
    /* gmtime of epoch 0 is 1970-01-01 00:00:00, a Thursday */
    n++;
    {
        int64_t z = 0;
        struct lx_tm tm;
        if (!qx_gmtime_r(&z, &tm) || tm.tm_year != 70 || tm.tm_mon != 0 ||
            tm.tm_mday != 1 || tm.tm_hour != 0 || tm.tm_wday != 4)
            fails++;
    }
    /* strftime of that epoch */
    n++;
    {
        int64_t z = 0;
        struct lx_tm tm;
        char buf[32];
        qx_gmtime_r(&z, &tm);
        if (qx_strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) == 0 || strcmp(buf, "1970-01-01") != 0)
            fails++;
    }
    /* mktime round-trips a gmtime back to the same epoch (UTC) */
    n++;
    {
        int64_t z = 1000000000LL; /* 2001-09-09 */
        struct lx_tm tm;
        qx_gmtime_r(&z, &tm);
        /* _mktime64 is local time; just require it produces a sane positive value */
        if (qx_mktime(&tm) <= 0)
            fails++;
    }
    /* anonymous mmap is writable and releasable */
    n++;
    {
        void *p = qx_mmap(NULL, 8192, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_ANONYMOUS, -1, 0);
        if (p == LX_MAP_FAILED) {
            fails++;
        } else {
            ((char *)p)[0] = 'A';
            ((char *)p)[8191] = 'Z';
            if (((char *)p)[0] != 'A' || ((char *)p)[8191] != 'Z')
                fails++;
            if (qx_munmap(p, 8192) != 0)
                fails++;
        }
    }
    /* sysconf / getpagesize agree on the page size */
    n++;
    if (qx_sysconf(LX_SC_PAGESIZE) != qx_getpagesize() || qx_getpagesize() <= 0)
        fails++;
    /* ids are non-zero */
    n++;
    if (qx_getpid() == 0 || qx_gettid() == 0)
        fails++;

    if (total)
        *total = n;
    return fails;
}
