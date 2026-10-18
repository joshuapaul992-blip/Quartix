/*
 * Quartix - APK (zip) reading and import.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define SIG_EOCD 0x06054b50u
#define SIG_CDIR 0x02014b50u
#define SIG_LOCAL 0x04034b50u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(HANDLE f, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN))
        return -1;
    if (!ReadFile(f, buf, n, &got, NULL) || got != n)
        return -1;
    return 0;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int ready;
    size_t i;
    if (!ready) {
        uint32_t c, k, j;
        for (k = 0; k < 256; k++) {
            c = k;
            for (j = 0; j < 8; j++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[k] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    for (i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

int qx_apk_list(const wchar_t *apk_path, qx_apk_visit_fn fn, void *ctx)
{
    HANDLE f;
    LARGE_INTEGER size;
    uint8_t *tail = NULL, *cdir = NULL;
    DWORD tail_len;
    uint32_t cd_size, cd_off, i, pos;
    uint16_t count;
    int rc = -1, found = 0;
    long k;

    f = CreateFileW(apk_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 22)
        goto out;

    /* The end-of-central-directory record is in the last 22 + 65535 bytes. */
    tail_len = (DWORD)(size.QuadPart < 22 + 65535 ? size.QuadPart : 22 + 65535);
    tail = malloc(tail_len);
    if (!tail || read_at(f, (uint64_t)size.QuadPart - tail_len, tail, tail_len))
        goto out;
    for (k = (long)tail_len - 22; k >= 0; k--) {
        if (rd32(tail + k) == SIG_EOCD) {
            found = 1;
            break;
        }
    }
    if (!found)
        goto out;
    count = rd16(tail + k + 10);
    cd_size = rd32(tail + k + 12);
    cd_off = rd32(tail + k + 16);
    if (count == 0xffff || cd_size == 0xffffffffu || cd_off == 0xffffffffu)
        goto out; /* zip64: not used by Roblox APKs */
    if ((uint64_t)cd_off + cd_size > (uint64_t)size.QuadPart)
        goto out;

    cdir = malloc(cd_size ? cd_size : 1);
    if (!cdir || read_at(f, cd_off, cdir, cd_size))
        goto out;

    for (i = 0, pos = 0; i < count; i++) {
        qx_apk_entry e;
        uint16_t nlen, xlen, clen;
        if (pos + 46 > cd_size || rd32(cdir + pos) != SIG_CDIR)
            goto out;
        nlen = rd16(cdir + pos + 28);
        xlen = rd16(cdir + pos + 30);
        clen = rd16(cdir + pos + 32);
        if (pos + 46 + (uint32_t)nlen + xlen + clen > cd_size)
            goto out;
        memset(&e, 0, sizeof(e));
        e.method = rd16(cdir + pos + 10);
        e.crc32 = rd32(cdir + pos + 16);
        e.comp_size = rd32(cdir + pos + 20);
        e.size = rd32(cdir + pos + 24);
        e.local_header_off = rd32(cdir + pos + 42);
        if (nlen < sizeof(e.name)) {
            memcpy(e.name, cdir + pos + 46, nlen);
            e.name[nlen] = 0;
            if (fn(&e, ctx))
                break;
        }
        pos += 46 + nlen + xlen + clen;
    }
    rc = 0;
out:
    free(tail);
    free(cdir);
    CloseHandle(f);
    return rc;
}

struct find_ctx {
    const char *name;
    qx_apk_entry *out;
    int hit;
};

static int find_visit(const qx_apk_entry *e, void *ctx)
{
    struct find_ctx *c = ctx;
    if (strcmp(e->name, c->name) == 0) {
        *c->out = *e;
        c->hit = 1;
        return 1;
    }
    return 0;
}

int qx_apk_find(const wchar_t *apk_path, const char *name, qx_apk_entry *out)
{
    struct find_ctx c = {name, out, 0};
    if (qx_apk_list(apk_path, find_visit, &c))
        return -1;
    return c.hit ? 0 : 1;
}

int qx_apk_extract(const wchar_t *apk_path, const qx_apk_entry *e, const wchar_t *dest)
{
    static uint8_t buf[65536];
    wchar_t tmp[QX_PATH_MAX];
    uint8_t lh[30];
    HANDLE in, out = INVALID_HANDLE_VALUE;
    uint64_t off, left;
    uint32_t crc = 0;
    int rc = -1;

    if (e->method != 0) {
        qx_err("%s is compressed (method %u); only stored entries are supported", e->name, e->method);
        return -1;
    }
    if (e->comp_size != e->size)
        return -1;
    if (wcslen(dest) + 5 > QX_PATH_MAX)
        return -1;
    wcscpy(tmp, dest);
    wcscat(tmp, L".tmp");

    in = CreateFileW(apk_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE)
        return -1;
    if (read_at(in, e->local_header_off, lh, sizeof(lh)) || rd32(lh) != SIG_LOCAL)
        goto out;
    off = e->local_header_off + 30 + rd16(lh + 26) + rd16(lh + 28);

    out = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE)
        goto out;
    for (left = e->size; left > 0;) {
        DWORD n = (DWORD)(left < sizeof(buf) ? left : sizeof(buf)), wrote;
        if (read_at(in, off, buf, n))
            goto out;
        crc = crc32_update(crc, buf, n);
        if (!WriteFile(out, buf, n, &wrote, NULL) || wrote != n)
            goto out;
        off += n;
        left -= n;
    }
    if (crc != e->crc32) {
        qx_err("CRC mismatch extracting %s", e->name);
        goto out;
    }
    if (!FlushFileBuffers(out))
        goto out;
    CloseHandle(out);
    out = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(tmp, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        goto out;
    rc = 0;
out:
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (rc)
        DeleteFileW(tmp);
    CloseHandle(in);
    return rc;
}

/* Copies the APK into the data dir under a content-addressed folder, extracts the
 * native library next to it and marks it current. Returns the version folder. */
int qx_apk_import(const qx_paths *p, const wchar_t *apk_path, wchar_t *out_dir, size_t cap)
{
    qx_apk_entry lib;
    uint8_t digest[32];
    char hex[65];
    wchar_t id[17], dir[QX_PATH_MAX], path[QX_PATH_MAX];
    int i, r;
    FILE *cur;

    r = qx_apk_find(apk_path, QX_APK_LIB, &lib);
    if (r < 0) {
        qx_err("not a readable APK");
        return -1;
    }
    if (r > 0) {
        qx_err("APK has no " QX_APK_LIB "; Quartix needs the x86_64 build of the Roblox app");
        return -1;
    }
    if (qx_sha256_file(apk_path, digest))
        return -1;
    qx_hex(digest, 32, hex);
    for (i = 0; i < 16; i++)
        id[i] = (wchar_t)hex[i];
    id[16] = 0;

    if (qx_path_join(dir, QX_PATH_MAX, p->apk_dir, id) || qx_mkdirs(dir))
        return -1;
    if (qx_path_join(path, QX_PATH_MAX, dir, L"base.apk") || !CopyFileW(apk_path, path, FALSE))
        return -1;
    if (qx_path_join(path, QX_PATH_MAX, dir, L"lib\\x86_64") || qx_mkdirs(path))
        return -1;
    if (qx_path_join(path, QX_PATH_MAX, path, L"libroblox.so") || qx_apk_extract(apk_path, &lib, path))
        return -1;

    /* Record the active version; written to a temp file and swapped in. */
    if (qx_path_join(path, QX_PATH_MAX, p->apk_dir, L"current.tmp"))
        return -1;
    cur = _wfopen(path, L"wb");
    if (!cur)
        return -1;
    fprintf(cur, "%.16s\n", hex);
    if (fclose(cur))
        return -1;
    {
        wchar_t final[QX_PATH_MAX];
        if (qx_path_join(final, QX_PATH_MAX, p->apk_dir, L"current") ||
            !MoveFileExW(path, final, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return -1;
    }
    if (out_dir && wcslen(dir) < cap)
        wcscpy(out_dir, dir);
    return 0;
}
