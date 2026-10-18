/*
 * Quartix - self-update. An update replaces Quartix.exe in the install dir and
 * nothing else; the data dir (login session, APKs, logs) is never written here.
 *
 * Every update comes with a signature file signed by the project's Ed25519 key:
 *
 *     quartix-update-v1
 *     version=<major.minor.patch>
 *     sha256=<hex digest of the new Quartix.exe>
 *     sig=<hex Ed25519 signature over the three lines above, newlines included>
 *
 * An update is installed only if the signature verifies against the key built into
 * this executable, the digest matches the file, and the version is newer.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include "../third_party/monocypher/monocypher-ed25519.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef QX_UPDATE_PUBKEY
#define QX_UPDATE_PUBKEY ""
#endif
#ifndef QX_UPDATE_BASE
#define QX_UPDATE_BASE "https://github.com/joshuapaul992-blip/Quartix/releases/latest/download/"
#endif
#define QX_UPDATE_MAX_BYTES (64u * 1024 * 1024)

#define SIG_MAGIC "quartix-update-v1\n"

static int with_suffix(wchar_t *out, const wchar_t *path, const wchar_t *suffix)
{
    if (wcslen(path) + wcslen(suffix) + 1 > QX_PATH_MAX)
        return -1;
    wcscpy(out, path);
    wcscat(out, suffix);
    return 0;
}

static int unhex(const char *in, size_t len, uint8_t *out)
{
    size_t i;
    for (i = 0; i < len * 2; i++) {
        char c = in[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return -1;
        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(v << 4);
        else
            out[i / 2] |= (uint8_t)v;
    }
    return in[len * 2] == 0 || in[len * 2] == '\n' ? 0 : -1;
}

/* Parses "major.minor.patch" into one comparable number. */
static int parse_version(const char *s, uint64_t *out)
{
    unsigned long part[3];
    int i;
    char *end;
    for (i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9')
            return -1;
        part[i] = strtoul(s, &end, 10);
        if (part[i] > 0xffff)
            return -1;
        s = end;
        if (i < 2 && *s++ != '.')
            return -1;
    }
    if (*s != 0 && *s != '\n')
        return -1;
    *out = (uint64_t)part[0] << 32 | (uint64_t)part[1] << 16 | part[2];
    return 0;
}

int qx_update_has_key(void)
{
    return sizeof(QX_UPDATE_PUBKEY) == 65;
}

/* Checks a signature file's contents (NUL-terminated). On success fills the
 * signed digest and version text. */
static int verify_signature(const char *buf, uint8_t digest[32], char version[32])
{
    const char *p, *line_version, *line_sha, *line_sig;
    uint8_t key[32], sig[64];
    size_t signed_len, vlen;

    if (!qx_update_has_key() || unhex(QX_UPDATE_PUBKEY, 32, key)) {
        qx_err("this build has no update signing key; updates are disabled");
        return -1;
    }
    if (strncmp(buf, SIG_MAGIC, sizeof(SIG_MAGIC) - 1) != 0)
        goto bad;
    line_version = buf + sizeof(SIG_MAGIC) - 1;
    if (strncmp(line_version, "version=", 8) != 0 || !(p = strchr(line_version, '\n')))
        goto bad;
    line_sha = p + 1;
    if (strncmp(line_sha, "sha256=", 7) != 0 || !(p = strchr(line_sha, '\n')))
        goto bad;
    line_sig = p + 1;
    if (strncmp(line_sig, "sig=", 4) != 0)
        goto bad;
    signed_len = (size_t)(line_sig - buf);

    if (unhex(line_sig + 4, 64, sig) || unhex(line_sha + 7, 32, digest))
        goto bad;
    if (crypto_ed25519_check(sig, key, (const uint8_t *)buf, signed_len) != 0) {
        qx_err("update signature is not valid");
        return -1;
    }
    vlen = (size_t)(line_sha - line_version) - 9;
    if (vlen >= 32)
        goto bad;
    memcpy(version, line_version + 8, vlen);
    version[vlen] = 0;
    return 0;
bad:
    qx_err("signature file is malformed");
    return -1;
}

/* 1 if `version` is newer than this build, 0 if not, -1 if unreadable. */
static int is_newer(const char *version)
{
    uint64_t new_ver, cur_ver;
    if (parse_version(version, &new_ver) || parse_version(QX_VERSION_A, &cur_ver)) {
        qx_err("update has an unreadable version");
        return -1;
    }
    return new_ver > cur_ver;
}

/* Windows lets a running executable be renamed but not overwritten, so the swap is
 * Quartix.exe -> Quartix.exe.old, staged -> Quartix.exe, rolled back on failure.
 * The .old file is removed on the next start by qx_update_cleanup. */
static int swap_in(const qx_paths *p, const wchar_t *staged_exe)
{
    wchar_t old[QX_PATH_MAX];

    if (qx_path_is_within(p->exe_path, p->data_dir)) {
        qx_err("install dir is inside the data dir; refusing to update");
        return -1;
    }
    if (with_suffix(old, p->exe_path, L".old"))
        return -1;
    DeleteFileW(old);
    if (!MoveFileExW(p->exe_path, old, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        qx_err("cannot move current executable aside (%lu)", GetLastError());
        return -1;
    }
    if (!MoveFileExW(staged_exe, p->exe_path, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        MoveFileExW(old, p->exe_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        qx_err("cannot install update (%lu); rolled back", e);
        return -1;
    }
    return 0;
}

int qx_update_apply(const qx_paths *p, const wchar_t *staged_exe, const wchar_t *sig_path)
{
    uint8_t want[32], got[32];
    char buf[1024], version[32];
    size_t n;
    int newer;
    FILE *f;

    f = _wfopen(sig_path, L"rb");
    if (!f) {
        qx_err("cannot read signature file");
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    if (verify_signature(buf, want, version))
        return -1;
    newer = is_newer(version);
    if (newer < 0)
        return -1;
    if (!newer) {
        qx_err("update %s is not newer than %s", version, QX_VERSION_A);
        return -1;
    }
    if (qx_sha256_file(staged_exe, got)) {
        qx_err("cannot read staged update");
        return -1;
    }
    if (memcmp(want, got, 32) != 0) {
        qx_err("staged update does not match its signature");
        return -1;
    }
    if (swap_in(p, staged_exe))
        return -1;
    qx_log("updated %s -> %s", QX_VERSION_A, version);
    return 0;
}

struct mem_sink {
    char buf[1024];
    size_t n;
};

static int mem_write(void *ctx, const void *data, size_t n)
{
    struct mem_sink *m = ctx;
    if (m->n + n >= sizeof(m->buf))
        return -1;
    memcpy(m->buf + m->n, data, n);
    m->n += n;
    return 0;
}

struct file_sink {
    HANDLE f;
    qx_sha256 sha;
};

static int file_write(void *ctx, const void *data, size_t n)
{
    struct file_sink *fs = ctx;
    DWORD wrote;
    qx_sha256_update(&fs->sha, data, n);
    return WriteFile(fs->f, data, (DWORD)n, &wrote, NULL) && wrote == n ? 0 : -1;
}

int qx_update_online(const qx_paths *p, int check_only, char new_version[32])
{
    struct mem_sink sig;
    struct file_sink fs;
    uint8_t want[32], got[32];
    char version[32], url[512];
    wchar_t staged[QX_PATH_MAX];
    int r, newer;

    if (!qx_update_has_key()) {
        qx_err("this build has no update signing key; updates are disabled");
        return -1;
    }
    memset(&sig, 0, sizeof(sig));
    snprintf(url, sizeof(url), "%sQuartix.exe.sig", QX_UPDATE_BASE);
    r = qx_https_get(url, mem_write, &sig, sizeof(sig.buf) - 1);
    if (r > 0) {
        qx_log("no release published yet");
        return QX_UPDATE_NONE;
    }
    if (r < 0)
        return -1;
    sig.buf[sig.n] = 0;
    if (verify_signature(sig.buf, want, version))
        return -1;
    newer = is_newer(version);
    if (newer < 0)
        return -1;
    if (new_version)
        strcpy(new_version, version);
    if (!newer) {
        qx_log("up to date (%s)", QX_VERSION_A);
        return QX_UPDATE_CURRENT;
    }
    if (check_only) {
        qx_log("update available: %s -> %s", QX_VERSION_A, version);
        return QX_UPDATE_AVAILABLE;
    }

    /* Stage next to the executable so the final move stays on one volume. */
    if (with_suffix(staged, p->exe_path, L".download"))
        return -1;
    fs.f = CreateFileW(staged, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fs.f == INVALID_HANDLE_VALUE) {
        qx_err("cannot write to the install folder (%lu)", GetLastError());
        return -1;
    }
    qx_sha256_init(&fs.sha);
    snprintf(url, sizeof(url), "%sQuartix.exe", QX_UPDATE_BASE);
    qx_log("downloading %s", version);
    r = qx_https_get(url, file_write, &fs, QX_UPDATE_MAX_BYTES);
    if (r == 0 && !FlushFileBuffers(fs.f))
        r = -1;
    CloseHandle(fs.f);
    if (r != 0) {
        DeleteFileW(staged);
        if (r > 0)
            qx_err("release has a signature but no Quartix.exe");
        return -1;
    }
    qx_sha256_final(&fs.sha, got);
    if (memcmp(want, got, 32) != 0) {
        DeleteFileW(staged);
        qx_err("downloaded update does not match its signature");
        return -1;
    }
    if (swap_in(p, staged)) {
        DeleteFileW(staged);
        return -1;
    }
    qx_log("updated %s -> %s; data dir untouched", QX_VERSION_A, version);
    return QX_UPDATE_INSTALLED;
}

void qx_update_cleanup(const qx_paths *p)
{
    wchar_t path[QX_PATH_MAX];
    /* Both fail harmlessly if absent or still in use. */
    if (with_suffix(path, p->exe_path, L".old") == 0)
        DeleteFileW(path);
    if (with_suffix(path, p->exe_path, L".download") == 0)
        DeleteFileW(path);
}
