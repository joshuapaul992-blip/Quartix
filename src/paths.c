/*
 * Quartix - install/data directory layout.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <shlobj.h>
#include <wchar.h>

int qx_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    int sep = la > 0 && a[la - 1] != L'\\' && a[la - 1] != L'/';
    if (la + sep + lb + 1 > cap)
        return -1;
    if (out != a)
        memmove(out, a, la * sizeof(wchar_t));
    if (sep)
        out[la++] = L'\\';
    memcpy(out + la, b, lb * sizeof(wchar_t));
    out[la + lb] = 0;
    return 0;
}

int qx_mkdirs(const wchar_t *path)
{
    wchar_t buf[QX_PATH_MAX];
    size_t n = wcslen(path), i;
    if (n == 0 || n >= QX_PATH_MAX)
        return -1;
    wcscpy(buf, path);
    for (i = 0; i < n; i++)
        if (buf[i] == L'/')
            buf[i] = L'\\';
    /* Skip the drive or UNC prefix so we never try to create "C:" itself. */
    i = (n >= 2 && buf[1] == L':') ? 3 : 0;
    if (n >= 2 && buf[0] == L'\\' && buf[1] == L'\\')
        i = 2;
    for (; i <= n; i++) {
        if (buf[i] == L'\\' || buf[i] == 0) {
            wchar_t c = buf[i];
            buf[i] = 0;
            if (buf[0] && !CreateDirectoryW(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                DWORD a = GetFileAttributesW(buf);
                if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
                    return -1;
            }
            buf[i] = c;
        }
    }
    return 0;
}

int qx_path_is_within(const wchar_t *path, const wchar_t *dir)
{
    wchar_t fp[QX_PATH_MAX], fd[QX_PATH_MAX];
    size_t ld;
    if (!GetFullPathNameW(path, QX_PATH_MAX, fp, NULL) || !GetFullPathNameW(dir, QX_PATH_MAX, fd, NULL))
        return 0;
    ld = wcslen(fd);
    while (ld > 0 && (fd[ld - 1] == L'\\' || fd[ld - 1] == L'/'))
        fd[--ld] = 0;
    if (_wcsnicmp(fp, fd, ld) != 0)
        return 0;
    return fp[ld] == 0 || fp[ld] == L'\\' || fp[ld] == L'/';
}

int qx_paths_init(qx_paths *p)
{
    wchar_t *slash;
    DWORD n;

    memset(p, 0, sizeof(*p));
    n = GetModuleFileNameW(NULL, p->exe_path, QX_PATH_MAX);
    if (n == 0 || n >= QX_PATH_MAX)
        return -1;
    wcscpy(p->install_dir, p->exe_path);
    slash = wcsrchr(p->install_dir, L'\\');
    if (slash)
        *slash = 0;

    /* QUARTIX_DATA_DIR overrides the location (tests, portable installs). */
    n = GetEnvironmentVariableW(L"QUARTIX_DATA_DIR", p->data_dir, QX_PATH_MAX);
    if (n == 0 || n >= QX_PATH_MAX) {
        wchar_t appdata[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appdata)))
            return -1;
        if (qx_path_join(p->data_dir, QX_PATH_MAX, appdata, L"Quartix"))
            return -1;
    }

    /* The data dir may live under the install dir (portable), but never be it:
     * updates write into the install dir. */
    if (qx_path_is_within(p->data_dir, p->install_dir) && qx_path_is_within(p->install_dir, p->data_dir))
        return -1;

    if (qx_path_join(p->app_data_dir, QX_PATH_MAX, p->data_dir, L"app\\com.roblox.client") ||
        qx_path_join(p->apk_dir, QX_PATH_MAX, p->data_dir, L"apk") ||
        qx_path_join(p->log_dir, QX_PATH_MAX, p->data_dir, L"logs"))
        return -1;

    if (qx_mkdirs(p->app_data_dir) || qx_mkdirs(p->apk_dir) || qx_mkdirs(p->log_dir))
        return -1;
    return 0;
}
