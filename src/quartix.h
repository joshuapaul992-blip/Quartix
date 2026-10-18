/*
 * Quartix - a Roblox APK manager for Windows XP x64.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#ifndef QUARTIX_H
#define QUARTIX_H

/* Target NT 5.2 (Windows XP x64 / Server 2003). Anything newer must not leak in. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0502
#endif
#ifndef WINVER
#define WINVER 0x0502
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "version.h"
#define QX_WIDEN_(x) L##x
#define QX_WIDEN(x) QX_WIDEN_(x)
#define QX_VERSION QX_WIDEN(QX_VERSION_A)
#define QX_PATH_MAX 1024

/* paths.c
 * The install dir holds only what an update may replace (Quartix.exe and friends).
 * The data dir holds everything that must survive updates: the emulated app's
 * private storage (where the Roblox app keeps its login session), imported APKs
 * and logs. Updates never touch the data dir. */
typedef struct qx_paths {
    wchar_t install_dir[QX_PATH_MAX]; /* directory containing Quartix.exe */
    wchar_t exe_path[QX_PATH_MAX];
    wchar_t data_dir[QX_PATH_MAX];    /* %APPDATA%\Quartix */
    wchar_t app_data_dir[QX_PATH_MAX];/* data_dir\app\com.roblox.client (login lives here) */
    wchar_t apk_dir[QX_PATH_MAX];     /* data_dir\apk */
    wchar_t log_dir[QX_PATH_MAX];     /* data_dir\logs */
} qx_paths;

int qx_paths_init(qx_paths *p);
int qx_mkdirs(const wchar_t *path);
int qx_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b);
/* Nonzero if `path` is inside (or equal to) `dir`, compared case-insensitively. */
int qx_path_is_within(const wchar_t *path, const wchar_t *dir);

/* sha256.c */
typedef struct qx_sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} qx_sha256;

void qx_sha256_init(qx_sha256 *s);
void qx_sha256_update(qx_sha256 *s, const void *data, size_t n);
void qx_sha256_final(qx_sha256 *s, uint8_t out[32]);
int qx_sha256_file(const wchar_t *path, uint8_t out[32]);
void qx_hex(const uint8_t *in, size_t n, char *out);

/* apk.c
 * An APK is a zip. We read its central directory to find the native library for
 * our architecture and extract it. Only stored (uncompressed) entries are
 * extracted for now; APKs that load native libraries in place store them that way,
 * and a deflated entry is reported as an error rather than guessed at. */
#define QX_APK_LIB "lib/x86_64/libroblox.so"

typedef struct qx_apk_entry {
    char name[512];
    uint16_t method;          /* 0 = stored, 8 = deflate */
    uint32_t crc32;
    uint64_t comp_size;
    uint64_t size;
    uint64_t local_header_off;
} qx_apk_entry;

typedef int (*qx_apk_visit_fn)(const qx_apk_entry *e, void *ctx);

int qx_apk_list(const wchar_t *apk_path, qx_apk_visit_fn fn, void *ctx);
int qx_apk_find(const wchar_t *apk_path, const char *name, qx_apk_entry *out);
int qx_apk_extract(const wchar_t *apk_path, const qx_apk_entry *e, const wchar_t *dest);
int qx_apk_import(const qx_paths *p, const wchar_t *apk_path, wchar_t *out_dir, size_t cap);

/* update.c */
int qx_update_apply(const qx_paths *p, const wchar_t *staged_exe, const wchar_t *sig_path);
int qx_update_has_key(void);
/* Checks GitHub Releases for a newer signed build and installs it (check_only:
 * report only). Returns one of the codes below, or -1 on error. new_version, if
 * not NULL, receives the release's version when one was found. */
enum { QX_UPDATE_NONE = 0, QX_UPDATE_CURRENT, QX_UPDATE_AVAILABLE, QX_UPDATE_INSTALLED };
int qx_update_online(const qx_paths *p, int check_only, char new_version[32]);

/* net.c */
typedef int (*qx_net_sink)(void *ctx, const void *data, size_t n);
/* HTTPS GET following redirects. 0 on success, 1 on HTTP 404, -1 on error. */
int qx_https_get(const char *url, qx_net_sink sink, void *ctx, uint64_t max_bytes);
void qx_update_cleanup(const qx_paths *p);

/* log.c */
void qx_log(const char *fmt, ...);
void qx_err(const char *fmt, ...);
/* The most recent qx_err message, for showing in the window. */
const char *qx_last_error(void);

/* gui.c */
int qx_gui_run(qx_paths *p);

#endif
