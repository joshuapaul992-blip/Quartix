/*
 * Quartix - self-update. An update replaces Quartix.exe in the install dir and
 * nothing else; the data dir (login session, APKs, logs) is never written here.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <wchar.h>

static int with_suffix(wchar_t *out, const wchar_t *path, const wchar_t *suffix)
{
    if (wcslen(path) + wcslen(suffix) + 1 > QX_PATH_MAX)
        return -1;
    wcscpy(out, path);
    wcscat(out, suffix);
    return 0;
}

/* Windows lets a running executable be renamed but not overwritten, so the swap is
 * Quartix.exe -> Quartix.exe.old, staged -> Quartix.exe, rolled back on failure.
 * The .old file is removed on the next start by qx_update_cleanup. */
int qx_update_apply(const qx_paths *p, const wchar_t *staged_exe, const char *sha256_hex)
{
    uint8_t digest[32];
    char hex[65];
    wchar_t old[QX_PATH_MAX];

    if (qx_path_is_within(p->exe_path, p->data_dir)) {
        qx_err("install dir is inside the data dir; refusing to update");
        return -1;
    }
    if (qx_sha256_file(staged_exe, digest)) {
        qx_err("cannot read staged update");
        return -1;
    }
    qx_hex(digest, 32, hex);
    if (_stricmp(hex, sha256_hex) != 0) {
        qx_err("staged update hash mismatch (got %s)", hex);
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

void qx_update_cleanup(const qx_paths *p)
{
    wchar_t old[QX_PATH_MAX];
    if (with_suffix(old, p->exe_path, L".old") == 0)
        DeleteFileW(old); /* fails harmlessly if absent or still in use */
}
