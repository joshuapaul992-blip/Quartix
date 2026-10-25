/*
 * Quartix - the stdio / file surface for the Android engine, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * libroblox.so opens and reads files through C stdio (fopen/fread/fwrite/...),
 * raw POSIX descriptors (open/read/write/lseek) and a few path operations
 * (access/mkdir/remove/rename/unlink). This file backs all of them with the host
 * CRT: a FILE* handed to the engine is a CRT FILE*, and a descriptor is a CRT
 * descriptor, so every follow-on call forwards straight through. The engine
 * treats FILE* as an opaque token, so this is transparent to it.
 *
 * Two translations are needed at the boundary:
 *   - Paths are UTF-8 (Android); the CRT's wide entry points take UTF-16, so
 *     every path is converted and the _w* function is used, which also handles
 *     non-ASCII paths correctly.
 *   - open() flags are Linux O_* values, which differ from the host _O_* values,
 *     so they are translated; _O_BINARY is always set so no CRLF mangling
 *     happens on these byte streams.
 *
 * The three standard streams are exported as data symbols (stdin/stdout/stderr);
 * text written to stdout/stderr is routed into the Quartix log, where engine
 * diagnostics are useful during bring-up, while real files go to the CRT.
 *
 * errno: on failure the host errno is copied into the shim's errno location, so
 * the engine's __errno (src/shim.c) sees it.
 *
 * Deferred to a later slice: stat/fstat (which need the bionic struct stat byte
 * layout) and the scanf family.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>

#define SYSV __attribute__((sysv_abi))

/* Linux open() flags (x86-64 bionic) */
#define LX_O_RDONLY 0x0000
#define LX_O_WRONLY 0x0001
#define LX_O_RDWR   0x0002
#define LX_O_CREAT  0x0040
#define LX_O_EXCL   0x0080
#define LX_O_TRUNC  0x0200
#define LX_O_APPEND 0x0400

/* Linux access() modes */
#define LX_F_OK 0
#define LX_X_OK 1
#define LX_W_OK 2
#define LX_R_OK 4

/* ---- the standard streams, as exported data symbols ----------------------- */
static FILE *qx_stdin_f;
static FILE *qx_stdout_f;
static FILE *qx_stderr_f;

static void stdio_init(void)
{
    if (!qx_stdout_f) {
        qx_stdin_f = stdin;
        qx_stdout_f = stdout;
        qx_stderr_f = stderr;
    }
}

static int is_std_out_err(void *stream)
{
    return stream == qx_stdout_f || stream == qx_stderr_f ||
           stream == (void *)stdout || stream == (void *)stderr;
}

/* copy the host errno into the shim's errno location so the engine sees it */
static void carry_errno(void)
{
    int *e = qx_shim_errno();
    if (e)
        *e = errno;
}

/* UTF-8 -> UTF-16, heap-allocated (caller frees), or NULL on failure */
static wchar_t *to_wide(const char *s)
{
    int n;
    wchar_t *w;
    if (!s)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* ---- the routed text writer, shared with fprintf/vfprintf in format.c ------ */
void qx_stdio_write(void *stream, const char *buf, size_t n)
{
    stdio_init();
    if (is_std_out_err(stream)) {
        /* route engine stdout/stderr text into the log, in bounded chunks */
        char tmp[1024];
        size_t off = 0;
        while (off < n) {
            size_t k = n - off;
            if (k > sizeof(tmp) - 1)
                k = sizeof(tmp) - 1;
            memcpy(tmp, buf + off, k);
            tmp[k] = '\0';
            qx_log("[roblox] %s", tmp);
            off += k;
        }
    } else if (stream) {
        fwrite(buf, 1, n, (FILE *)stream);
    }
}

/* ---- FILE* stream functions ----------------------------------------------- */
static SYSV void *qx_fopen(const char *path, const char *mode)
{
    wchar_t *wp = to_wide(path);
    wchar_t wm[16];
    FILE *f;
    size_t i;
    stdio_init();
    if (!wp) {
        carry_errno();
        return NULL;
    }
    for (i = 0; mode && mode[i] && i < sizeof(wm) / sizeof(wm[0]) - 1; i++)
        wm[i] = (wchar_t)mode[i];
    wm[i] = L'\0';
    f = _wfopen(wp, wm);
    free(wp);
    if (!f)
        carry_errno();
    return f;
}
static SYSV void *qx_fdopen(int fd, const char *mode)
{
    wchar_t wm[16];
    size_t i;
    FILE *f;
    for (i = 0; mode && mode[i] && i < sizeof(wm) / sizeof(wm[0]) - 1; i++)
        wm[i] = (wchar_t)mode[i];
    wm[i] = L'\0';
    f = _wfdopen(fd, wm);
    if (!f)
        carry_errno();
    return f;
}
static SYSV int qx_fclose(void *f) { return fclose((FILE *)f); }
static SYSV int qx_feof(void *f) { return feof((FILE *)f); }
static SYSV int qx_ferror(void *f) { return ferror((FILE *)f); }
static SYSV int qx_fflush(void *f) { return fflush((FILE *)f); }
static SYSV size_t qx_fread(void *p, size_t sz, size_t nm, void *f)
{
    size_t r = fread(p, sz, nm, (FILE *)f);
    if (r < nm && ferror((FILE *)f))
        carry_errno();
    return r;
}
static SYSV size_t qx_fwrite(const void *p, size_t sz, size_t nm, void *f)
{
    if (is_std_out_err(f)) {
        qx_stdio_write(f, (const char *)p, sz * nm);
        return nm;
    }
    return fwrite(p, sz, nm, (FILE *)f);
}
static SYSV char *qx_fgets(char *buf, int n, void *f) { return fgets(buf, n, (FILE *)f); }
static SYSV int qx_fputc(int c, void *f)
{
    if (is_std_out_err(f)) {
        char ch = (char)c;
        qx_stdio_write(f, &ch, 1);
        return c;
    }
    return fputc(c, (FILE *)f);
}
static SYSV int qx_fputs(const char *s, void *f)
{
    if (is_std_out_err(f)) {
        qx_stdio_write(f, s, s ? strlen(s) : 0);
        return 0;
    }
    return fputs(s, (FILE *)f);
}
static SYSV int qx_getc(void *f) { return fgetc((FILE *)f); }
static SYSV int qx_puts(const char *s)
{
    stdio_init();
    qx_log("[roblox] %s", s ? s : "");
    return 0;
}
/* bionic fseek/ftell use a 64-bit long (LP64), so take/return 64-bit offsets */
static SYSV int qx_fseek(void *f, long long off, int whence) { return _fseeki64((FILE *)f, off, whence); }
static SYSV long long qx_ftell(void *f) { return _ftelli64((FILE *)f); }
static SYSV int qx_setvbuf(void *f, char *buf, int mode, size_t size)
{
    return setvbuf((FILE *)f, buf, mode, size);
}

/* ---- raw descriptors ------------------------------------------------------ */
static int translate_oflags(int lx)
{
    int ms = _O_BINARY;
    int acc = lx & 0x3;
    if (acc == LX_O_WRONLY)
        ms |= _O_WRONLY;
    else if (acc == LX_O_RDWR)
        ms |= _O_RDWR;
    else
        ms |= _O_RDONLY;
    if (lx & LX_O_CREAT)
        ms |= _O_CREAT;
    if (lx & LX_O_EXCL)
        ms |= _O_EXCL;
    if (lx & LX_O_TRUNC)
        ms |= _O_TRUNC;
    if (lx & LX_O_APPEND)
        ms |= _O_APPEND;
    return ms;
}
static SYSV int qx_open(const char *path, int flags, int mode)
{
    wchar_t *wp = to_wide(path);
    int fd;
    if (!wp) {
        carry_errno();
        return -1;
    }
    (void)mode;   /* Windows has no POSIX permission bits; grant read+write */
    fd = _wopen(wp, translate_oflags(flags), _S_IREAD | _S_IWRITE);
    free(wp);
    if (fd < 0)
        carry_errno();
    return fd;
}
/* bionic's _FORTIFY_SOURCE open(): the two-arg form, no mode. */
static SYSV int qx_open_2(const char *path, int flags) { return qx_open(path, flags, 0); }
static SYSV int qx_close(int fd)
{
    int r;
    if (qx_socket_is_sock(fd)) /* sockets are not CRT fds; route to closesocket */
        return qx_socket_close(fd);
    r = _close(fd);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV long long qx_read(int fd, void *buf, size_t n)
{
    int r;
    unsigned cap = n > 0x7fffffffu ? 0x7fffffffu : (unsigned)n;
    if (qx_socket_is_sock(fd)) /* a socket fd uses recv, not _read */
        return qx_socket_read(fd, buf, n);
    r = _read(fd, buf, cap);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV long long qx_write(int fd, const void *buf, size_t n)
{
    int r;
    unsigned cap = n > 0x7fffffffu ? 0x7fffffffu : (unsigned)n;
    if (qx_socket_is_sock(fd)) /* a socket fd uses send, not _write */
        return qx_socket_write(fd, buf, n);
    r = _write(fd, buf, cap);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV long long qx_lseek(int fd, long long off, int whence)
{
    long long r = _lseeki64(fd, off, whence);
    if (r < 0)
        carry_errno();
    return r;
}

/* ---- path operations ------------------------------------------------------ */
static SYSV int qx_access(const char *path, int mode)
{
    wchar_t *wp = to_wide(path);
    int msmode = 0, r;
    if (!wp) {
        carry_errno();
        return -1;
    }
    /* _waccess has no execute check; map X_OK to a plain existence test */
    if (mode & LX_R_OK)
        msmode |= 4;
    if (mode & LX_W_OK)
        msmode |= 2;
    (void)LX_F_OK;
    (void)LX_X_OK;
    r = _waccess(wp, msmode);
    free(wp);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV int qx_mkdir(const char *path, int mode)
{
    wchar_t *wp = to_wide(path);
    int r;
    (void)mode;
    if (!wp) {
        carry_errno();
        return -1;
    }
    r = _wmkdir(wp);
    free(wp);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV int qx_remove(const char *path)
{
    wchar_t *wp = to_wide(path);
    int r;
    if (!wp) {
        carry_errno();
        return -1;
    }
    r = _wremove(wp);
    free(wp);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV int qx_rename(const char *a, const char *b)
{
    wchar_t *wa = to_wide(a), *wb = to_wide(b);
    int r = -1;
    if (wa && wb)
        r = _wrename(wa, wb);
    free(wa);
    free(wb);
    if (r < 0)
        carry_errno();
    return r;
}
static SYSV int qx_unlink(const char *path)
{
    wchar_t *wp = to_wide(path);
    int r;
    if (!wp) {
        carry_errno();
        return -1;
    }
    r = _wunlink(wp);
    free(wp);
    if (r < 0)
        carry_errno();
    return r;
}

static SYSV int qx_ungetc(int ch, void *f) { return ungetc(ch, (FILE *)f); }

/* ---- stat / fstat / lstat --------------------------------------------------
 * The engine reads a bionic struct stat, whose x86-64 layout is the Linux
 * kernel one (144 bytes). We fill it from the host CRT's _stat64 (whose S_IF*
 * bits already match POSIX). */
struct lx_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    int64_t __reserved[3];
};
typedef char qx_stat_size_check[sizeof(struct lx_stat) == 144 ? 1 : -1];

static void fill_stat(const struct _stat64 *ms, struct lx_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = (uint64_t)ms->st_dev;
    out->st_ino = (uint64_t)ms->st_ino;
    out->st_nlink = ms->st_nlink ? ms->st_nlink : 1;
    out->st_mode = ms->st_mode;      /* S_IFDIR/S_IFREG bits match POSIX */
    out->st_rdev = (uint64_t)ms->st_rdev;
    out->st_size = ms->st_size;
    out->st_blksize = 512;
    out->st_blocks = (ms->st_size + 511) / 512;
    out->st_atime_sec = ms->st_atime;
    out->st_mtime_sec = ms->st_mtime;
    out->st_ctime_sec = ms->st_ctime;
}

static SYSV int qx_stat(const char *path, void *buf)
{
    wchar_t *wp = to_wide(path);
    struct _stat64 ms;
    int r;
    if (!wp) {
        carry_errno();
        return -1;
    }
    r = _wstat64(wp, &ms);
    free(wp);
    if (r < 0) {
        carry_errno();
        return -1;
    }
    fill_stat(&ms, (struct lx_stat *)buf);
    return 0;
}
static SYSV int qx_fstat(int fd, void *buf)
{
    struct _stat64 ms;
    if (_fstat64(fd, &ms) < 0) {
        carry_errno();
        return -1;
    }
    fill_stat(&ms, (struct lx_stat *)buf);
    return 0;
}

/* ---- resolver -------------------------------------------------------------- */
struct io_sym {
    const char *name;
    void *fn;
};
static const struct io_sym g_io_table[] = {
    {"fopen", (void *)qx_fopen}, {"fdopen", (void *)qx_fdopen}, {"fclose", (void *)qx_fclose},
    {"feof", (void *)qx_feof}, {"ferror", (void *)qx_ferror}, {"fflush", (void *)qx_fflush},
    {"fread", (void *)qx_fread}, {"fwrite", (void *)qx_fwrite}, {"fgets", (void *)qx_fgets},
    {"fputc", (void *)qx_fputc}, {"fputs", (void *)qx_fputs}, {"getc", (void *)qx_getc},
    {"puts", (void *)qx_puts}, {"fseek", (void *)qx_fseek}, {"ftell", (void *)qx_ftell},
    {"setvbuf", (void *)qx_setvbuf},
    {"open", (void *)qx_open}, {"__open_2", (void *)qx_open_2},
    {"close", (void *)qx_close}, {"read", (void *)qx_read},
    {"write", (void *)qx_write}, {"lseek", (void *)qx_lseek},
    {"access", (void *)qx_access}, {"mkdir", (void *)qx_mkdir}, {"remove", (void *)qx_remove},
    {"rename", (void *)qx_rename}, {"unlink", (void *)qx_unlink}, {"ungetc", (void *)qx_ungetc},
    {"stat", (void *)qx_stat}, {"fstat", (void *)qx_fstat}, {"lstat", (void *)qx_stat},
};

void *qx_stdio_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    stdio_init();
    if (strcmp(name, "stdin") == 0)
        return &qx_stdin_f;
    if (strcmp(name, "stdout") == 0)
        return &qx_stdout_f;
    if (strcmp(name, "stderr") == 0)
        return &qx_stderr_f;
    for (i = 0; i < sizeof(g_io_table) / sizeof(g_io_table[0]); i++)
        if (strcmp(name, g_io_table[i].name) == 0)
            return g_io_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------
 * Round-trips a temp file through fopen/fwrite/fseek/ftell/fread/fgets and the
 * raw open/write/lseek/read path, then exercises access/rename/unlink. Returns
 * the failure count and, via *total, the number of checks. */
int qx_stdio_selftest(int *total)
{
    int fails = 0, n = 0;
    wchar_t dir[QX_PATH_MAX];
    char path8[QX_PATH_MAX * 2];
    DWORD dl;
    FILE *f;
    char rd[64];
    long pos;
    int fd;
    stdio_init();

    /* a scratch path in the temp directory, as UTF-8 */
    dl = GetTempPathW(QX_PATH_MAX, dir);
    if (dl == 0 || dl >= QX_PATH_MAX) {
        if (total)
            *total = 0;
        return 1;
    }
    {
        wchar_t full[QX_PATH_MAX];
        _snwprintf(full, QX_PATH_MAX, L"%squartix_io_%lu.tmp", dir, (unsigned long)GetCurrentProcessId());
        WideCharToMultiByte(CP_UTF8, 0, full, -1, path8, sizeof(path8), NULL, NULL);
    }

    /* stdio write/read round-trip */
    n++;
    f = (FILE *)qx_fopen(path8, "wb");
    if (!f || qx_fwrite("hello\nworld", 1, 11, f) != 11 || qx_fclose(f) != 0)
        fails++;

    n++;
    f = (FILE *)qx_fopen(path8, "rb");
    if (!f) {
        fails++;
    } else {
        size_t got = qx_fread(rd, 1, sizeof(rd), f);
        if (got != 11 || memcmp(rd, "hello\nworld", 11) != 0)
            fails++;
        qx_fclose(f);
    }

    /* fseek / ftell */
    n++;
    f = (FILE *)qx_fopen(path8, "rb");
    if (!f) {
        fails++;
    } else {
        qx_fseek(f, 6, SEEK_SET);
        pos = qx_ftell(f);
        if (pos != 6 || !qx_fgets(rd, sizeof(rd), f) || strcmp(rd, "world") != 0)
            fails++;
        qx_fclose(f);
    }

    /* raw descriptor path: open/write/lseek/read */
    n++;
    fd = qx_open(path8, LX_O_WRONLY | LX_O_TRUNC, 0);
    if (fd < 0 || qx_write(fd, "0123456789", 10) != 10 || qx_close(fd) != 0)
        fails++;
    n++;
    fd = qx_open(path8, LX_O_RDONLY, 0);
    if (fd < 0) {
        fails++;
    } else {
        char b[16];
        long long off = qx_lseek(fd, 4, SEEK_SET);
        long long got = qx_read(fd, b, 4);
        if (off != 4 || got != 4 || memcmp(b, "4567", 4) != 0)
            fails++;
        qx_close(fd);
    }

    /* stat: a regular file (S_IFREG) of 10 bytes, from the raw write above */
    n++;
    {
        struct lx_stat st;
        if (qx_stat(path8, &st) != 0 || st.st_size != 10 || (st.st_mode & 0xF000u) != 0x8000u)
            fails++;
    }

    /* access / rename / unlink */
    n++;
    if (qx_access(path8, LX_F_OK) != 0 || qx_access(path8, LX_R_OK) != 0)
        fails++;
    n++;
    {
        char path8b[QX_PATH_MAX * 2];
        snprintf(path8b, sizeof(path8b), "%s.renamed", path8);
        /* rename moves it, the new name exists, and unlink removes it */
        if (qx_rename(path8, path8b) != 0 ||
            qx_access(path8b, LX_F_OK) != 0 ||
            qx_unlink(path8b) != 0 ||
            qx_access(path8b, LX_F_OK) == 0)
            fails++;
    }

    if (total)
        *total = n;
    return fails;
}
