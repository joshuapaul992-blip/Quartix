/*
 * Quartix - a native Roblox client runtime for Windows Vista / 7 / 8.1 x64.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#ifndef QUARTIX_H
#define QUARTIX_H

/* Target NT 6.0 (Windows Vista x64) as the floor; also runs on 7 (6.1) and
 * 8.1 (6.3). Nothing newer than the floor may leak in - see tools/check_win_pe.py. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WINVER
#define WINVER 0x0600
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
 * our architecture and extract it (stored or deflated). APK bundles (.apks, .xapk,
 * .apkm) are zips of a base APK and split APKs; their APKs are unpacked side by
 * side and the library is taken from whichever split carries it. */
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
/* Resolves the active import (apk\current) to its extracted libroblox.so.
 * Returns 0 and writes the path to lib_out when one exists, -1 otherwise. */
int qx_apk_current_lib(const qx_paths *p, wchar_t *lib_out, size_t cap);

/* elf.c - read-only inspection of the Android native library (ELF64). Parses
 * the header, program headers and dynamic section; it maps and runs nothing. */
#define QX_ELF_NEEDED_MAX 16
typedef struct qx_elf_info {
    int is_elf64_x86_64;                     /* little-endian ELF64 for x86-64 */
    int type;                                /* ET_* (3 = shared object / PIE) */
    unsigned machine;                        /* e_machine (62 = x86-64) */
    uint64_t entry;
    unsigned load_segments;
    uint64_t mapped_size;                    /* span of PT_LOAD, memsz */
    unsigned needed_count;                   /* DT_NEEDED entries */
    unsigned needed_listed;
    int has_tls;
    char soname[256];
    char needed[QX_ELF_NEEDED_MAX][256];
} qx_elf_info;

/* 0 when the file was read (inspect is_elf64_x86_64 for whether it is usable);
 * -1 only when the file cannot be read at all. */
int qx_elf_read(const wchar_t *path, qx_elf_info *out);

/* loader.c - map the ELF into memory and apply its relocations. Roblox's
 * libroblox.so packs its relative relocations in Android's APS2 format
 * (DT_ANDROID_RELA), so the loader decodes that as well as the standard RELA
 * and PLT tables. Symbolic relocations are bound through the shim resolver;
 * running the mapped code (ABI bridge, init array) is a later phase. */
typedef struct qx_elf_image {
    uint8_t *base;             /* reserved span (VirtualAlloc), or NULL */
    uint64_t span;             /* bytes reserved */
    uint64_t bias;             /* base - lowest page-aligned p_vaddr */
    uint64_t entry;            /* absolute entry point (bias + e_entry) */
    unsigned load_segments;
    unsigned relative_relocs;  /* R_X86_64_RELATIVE applied (incl. APS2) */
    unsigned symbolic_relocs;  /* GLOB_DAT/JUMP_SLOT/64 total */
    unsigned symbolic_resolved;/* bound to a real shim implementation */
    unsigned other_relocs;     /* unexpected relocation types */
} qx_elf_image;

int qx_elf_load(const wchar_t *path, qx_elf_image *out);
void qx_elf_unload(qx_elf_image *img);

/* shim.c - resolver for the Android/libc symbols libroblox.so imports. Real,
 * SysV-ABI implementations arrive in later phases; until then a name resolves
 * to NULL and the loader binds it to a trap that logs and exits if ever called. */
void *qx_shim_resolve(const char *name); /* real implementation, or NULL */
void *qx_shim_trap_addr(void);           /* the placeholder trap's address */
void *qx_thread_resolve(const char *name); /* pthreads/sched slice, or NULL */
void *qx_format_resolve(const char *name); /* variadic printf/log slice, or NULL */
void *qx_stdio_resolve(const char *name);  /* stdio/file slice, or NULL */
void *qx_scan_resolve(const char *name);   /* scanf slice, or NULL */
void *qx_os_resolve(const char *name);     /* time/memory/process slice, or NULL */
void *qx_cstd_resolve(const char *name);   /* libm floats, fortified _chk, wide/ctype, runtime glue, or NULL */
void *qx_android_resolve(const char *name); /* libandroid NativeActivity slice, or NULL */
void *qx_socket_resolve(const char *name);  /* BSD sockets (Winsock) slice, or NULL */
void *qx_gl_resolve(const char *name);      /* EGL/GLES bound to ANGLE, or NULL when ANGLE is absent */
void *qx_gl_proc(const char *name);         /* raw ANGLE DLL symbol (Microsoft ABI), or NULL */
int qx_gl_available(void);                  /* nonzero once the ANGLE DLLs load */
void qx_gl_set_dir(const wchar_t *dir);     /* directory to load the ANGLE DLLs from */
int qx_gl_smoketest(void);                  /* gltest.c: drive ANGLE->D3D on the GPU; 0 on success */
/* socket fds are not CRT descriptors, so stdio.c routes close/read/write for them here */
int qx_socket_is_sock(int fd);
int qx_socket_close(int fd);
long long qx_socket_read(int fd, void *buf, size_t n);
long long qx_socket_write(int fd, const void *buf, size_t n);
void qx_android_set_asset_dir(const wchar_t *dir); /* point the asset manager at the APK's assets */
void qx_android_set_window_size(int w, int h);     /* set the native window dimensions */
void *qx_android_asset_manager(void);              /* the stand-in AAssetManager handle */
void *qx_android_native_window(void);              /* the stand-in ANativeWindow handle */
int *qx_shim_errno(void);                  /* the engine's errno storage (src/shim.c) */
void qx_stdio_write(void *stream, const char *buf, size_t n); /* stdout/stderr -> log, else CRT */
/* sysv_abi so main() calls it with the same convention it is defined with; these
 * are non-variadic, so the fixed-arg Microsoft->SysV call is a safe thunk. */
int __attribute__((sysv_abi)) qx_format_selftest(int *total); /* formatter checks; returns failures */
int qx_stdio_selftest(int *total);          /* stdio round-trip checks; returns failures */
int __attribute__((sysv_abi)) qx_scan_selftest(int *total); /* scanf checks; returns failures */
int __attribute__((sysv_abi)) qx_os_selftest(int *total);   /* time/memory/process checks; returns failures */
int __attribute__((sysv_abi)) qx_cstd_selftest(int *total); /* libc-completion checks; returns failures */
int __attribute__((sysv_abi)) qx_android_selftest(int *total); /* libandroid surface checks; returns failures */
int __attribute__((sysv_abi)) qx_socket_selftest(int *total);  /* sockets loopback checks; returns failures */
int __attribute__((sysv_abi)) qx_gl_selftest(int *total);      /* EGL loader checks; returns failures */

/* runtime.c - the native Roblox runtime (work in progress). */
enum { QX_RUN_NOT_READY = 2 }; /* launch exit code while the core is unfinished */
int qx_runtime_inspect(const qx_paths *p, qx_elf_info *info, wchar_t *lib_out, size_t cap);
int qx_runtime_launch(const qx_paths *p);

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
