/*
 * Quartix - APK and APK bundle (zip) reading and import.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.h"

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

/* Streams one entry's data (stored or deflated) to `out`, checking size and CRC. */
static int copy_entry(HANDLE in, const qx_apk_entry *e, HANDLE out)
{
    static uint8_t inbuf[65536], dict[TINFL_LZ_DICT_SIZE];
    static tinfl_decompressor inf;
    uint8_t lh[30];
    uint64_t off, comp_left, total = 0;
    uint32_t crc = 0;
    size_t in_pos = 0, in_avail = 0, dict_ofs = 0;
    DWORD wrote;

    if (e->method != 0 && e->method != 8) {
        qx_err("%s uses unsupported compression method %u", e->name, e->method);
        return -1;
    }
    if (read_at(in, e->local_header_off, lh, sizeof(lh)) || rd32(lh) != SIG_LOCAL)
        return -1;
    off = e->local_header_off + 30 + rd16(lh + 26) + rd16(lh + 28);
    comp_left = e->comp_size;

    if (e->method == 0) {
        if (e->comp_size != e->size)
            return -1;
        while (comp_left > 0) {
            DWORD n = (DWORD)(comp_left < sizeof(inbuf) ? comp_left : sizeof(inbuf));
            if (read_at(in, off, inbuf, n))
                return -1;
            crc = crc32_update(crc, inbuf, n);
            if (!WriteFile(out, inbuf, n, &wrote, NULL) || wrote != n)
                return -1;
            off += n;
            comp_left -= n;
        }
        total = e->size;
    } else {
        tinfl_init(&inf);
        for (;;) {
            size_t in_bytes, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
            tinfl_status st;
            if (in_avail == 0 && comp_left > 0) {
                DWORD n = (DWORD)(comp_left < sizeof(inbuf) ? comp_left : sizeof(inbuf));
                if (read_at(in, off, inbuf, n))
                    return -1;
                off += n;
                comp_left -= n;
                in_pos = 0;
                in_avail = n;
            }
            in_bytes = in_avail;
            st = tinfl_decompress(&inf, inbuf + in_pos, &in_bytes, dict, dict + dict_ofs, &out_bytes,
                                  comp_left > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            in_pos += in_bytes;
            in_avail -= in_bytes;
            if (out_bytes) {
                total += out_bytes;
                if (total > e->size)
                    break;
                crc = crc32_update(crc, dict + dict_ofs, out_bytes);
                if (!WriteFile(out, dict + dict_ofs, (DWORD)out_bytes, &wrote, NULL) || wrote != out_bytes)
                    return -1;
                dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (st == TINFL_STATUS_DONE)
                break;
            if (st < TINFL_STATUS_DONE || (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_avail == 0 && comp_left == 0)) {
                qx_err("%s is corrupt", e->name);
                return -1;
            }
        }
    }
    if (total != e->size || crc != e->crc32) {
        qx_err("%s is corrupt (size or CRC mismatch)", e->name);
        return -1;
    }
    return 0;
}

int qx_apk_extract(const wchar_t *apk_path, const qx_apk_entry *e, const wchar_t *dest)
{
    wchar_t tmp[QX_PATH_MAX];
    HANDLE in, out;
    int rc = -1;

    if (wcslen(dest) + 5 > QX_PATH_MAX)
        return -1;
    wcscpy(tmp, dest);
    wcscat(tmp, L".tmp");

    in = CreateFileW(apk_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE)
        return -1;
    out = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        rc = copy_entry(in, e, out) || !FlushFileBuffers(out) ? -1 : 0;
        CloseHandle(out);
        if (rc == 0 && !MoveFileExW(tmp, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            rc = -1;
        if (rc)
            DeleteFileW(tmp);
    }
    CloseHandle(in);
    return rc;
}

/* Bundles (.apks, .xapk, .apkm) are zips of APKs: a base APK plus split APKs, one
 * of which carries the native library for our architecture. */
#define MAX_SPLITS 64

struct bundle {
    qx_apk_entry apks[MAX_SPLITS];
    wchar_t names[MAX_SPLITS][128];
    int count;
};

static int ends_with_apk(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && _stricmp(name + n - 4, ".apk") == 0;
}

/* The file name an entry is saved under: its base name, restricted to a safe
 * character set so nothing can be written outside the version folder. */
static int safe_name(const char *entry, wchar_t *out, size_t cap)
{
    const char *base = strrchr(entry, '/');
    size_t i, n;
    base = base ? base + 1 : entry;
    n = strlen(base);
    if (n < 5 || n >= cap || base[0] == '.')
        return -1;
    for (i = 0; i < n; i++) {
        char c = base[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return -1;
        out[i] = (wchar_t)c;
    }
    out[n] = 0;
    return 0;
}

static int bundle_visit(const qx_apk_entry *e, void *ctx)
{
    struct bundle *b = ctx;
    wchar_t name[128];
    int i;
    if (!ends_with_apk(e->name) || strncmp(e->name, "standalones/", 12) == 0 || safe_name(e->name, name, 128))
        return 0; /* .apks standalones/ hold full per-device copies we don't need */
    for (i = 0; i < b->count; i++)
        if (_wcsicmp(b->names[i], name) == 0)
            return 0; /* first one wins */
    if (b->count == MAX_SPLITS)
        return 1;
    b->apks[b->count] = *e;
    wcscpy(b->names[b->count], name);
    b->count++;
    return 0;
}

static void remove_version_dir(const wchar_t *dir)
{
    wchar_t pattern[QX_PATH_MAX], path[QX_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (qx_path_join(path, QX_PATH_MAX, dir, L"lib\\x86_64\\libroblox.so") == 0)
        DeleteFileW(path);
    if (qx_path_join(path, QX_PATH_MAX, dir, L"lib\\x86_64") == 0)
        RemoveDirectoryW(path);
    if (qx_path_join(path, QX_PATH_MAX, dir, L"lib") == 0)
        RemoveDirectoryW(path);
    if (qx_path_join(pattern, QX_PATH_MAX, dir, L"*") == 0 && (h = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && qx_path_join(path, QX_PATH_MAX, dir, fd.cFileName) == 0)
                DeleteFileW(path);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* Extracts every APK in the bundle into `dir`, names the base one base.apk and
 * extracts the native library from whichever APK carries it. */
static int import_bundle(const wchar_t *bundle_path, const wchar_t *dir)
{
    static struct bundle b;
    static const wchar_t *base_names[] = {L"base.apk", L"base-master.apk", L"com.roblox.client.apk"};
    wchar_t path[QX_PATH_MAX], lib[QX_PATH_MAX];
    qx_apk_entry e;
    int i, k, base = -1, found = 0;

    memset(&b, 0, sizeof(b));
    if (qx_apk_list(bundle_path, bundle_visit, &b))
        return -1;
    if (b.count == 0) {
        qx_err("not an APK bundle and has no " QX_APK_LIB "; Quartix needs the x86_64 build of the Roblox app");
        return -1;
    }
    for (k = 0; k < 3 && base < 0; k++)
        for (i = 0; i < b.count; i++)
            if (_wcsicmp(b.names[i], base_names[k]) == 0)
                base = i;
    for (i = 0; i < b.count && base < 0; i++)
        if (!wcsstr(b.names[i], L"config") && !wcsstr(b.names[i], L"split"))
            base = i;
    if (base < 0) {
        qx_err("the bundle has no base APK");
        return -1;
    }
    if (base > 0 || _wcsicmp(b.names[0], L"base.apk") != 0) {
        /* Avoid a clash if some other split is literally named base.apk. */
        for (i = 0; i < b.count; i++)
            if (i != base && _wcsicmp(b.names[i], L"base.apk") == 0)
                wcscpy(b.names[i], L"base-other.apk");
        wcscpy(b.names[base], L"base.apk");
    }

    for (i = 0; i < b.count; i++) {
        if (qx_path_join(path, QX_PATH_MAX, dir, b.names[i]) || qx_apk_extract(bundle_path, &b.apks[i], path))
            return -1;
    }
    if (qx_path_join(lib, QX_PATH_MAX, dir, L"lib\\x86_64") || qx_mkdirs(lib) ||
        qx_path_join(lib, QX_PATH_MAX, lib, L"libroblox.so"))
        return -1;
    for (i = 0; i < b.count && !found; i++) {
        if (qx_path_join(path, QX_PATH_MAX, dir, b.names[i]))
            return -1;
        if (qx_apk_find(path, QX_APK_LIB, &e) == 0) {
            if (qx_apk_extract(path, &e, lib))
                return -1;
            found = 1;
        }
    }
    if (!found) {
        qx_err("the bundle has no " QX_APK_LIB "; it needs the x86_64 split of the Roblox app");
        return -1;
    }
    return 0;
}

/* Copies an APK, or the APKs inside a bundle, into the data dir under a
 * content-addressed folder, extracts the native library and marks it current.
 * Returns the version folder. */
int qx_apk_import(const qx_paths *p, const wchar_t *apk_path, wchar_t *out_dir, size_t cap)
{
    qx_apk_entry lib;
    uint8_t digest[32];
    char hex[65];
    wchar_t id[17], dir[QX_PATH_MAX], path[QX_PATH_MAX];
    int i, r, existed;
    FILE *cur;

    r = qx_apk_find(apk_path, QX_APK_LIB, &lib);
    if (r < 0) {
        qx_err("not a readable APK or APK bundle");
        return -1;
    }
    if (qx_sha256_file(apk_path, digest))
        return -1;
    qx_hex(digest, 32, hex);
    for (i = 0; i < 16; i++)
        id[i] = (wchar_t)hex[i];
    id[16] = 0;

    if (qx_path_join(dir, QX_PATH_MAX, p->apk_dir, id))
        return -1;
    /* Re-importing a file that is already here must never delete it on failure. */
    existed = GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
    if (qx_mkdirs(dir))
        return -1;
    if (r > 0)
        r = import_bundle(apk_path, dir);
    else
        r = qx_path_join(path, QX_PATH_MAX, dir, L"base.apk") || !CopyFileW(apk_path, path, FALSE) ||
                    qx_path_join(path, QX_PATH_MAX, dir, L"lib\\x86_64") || qx_mkdirs(path) ||
                    qx_path_join(path, QX_PATH_MAX, path, L"libroblox.so") || qx_apk_extract(apk_path, &lib, path)
                ? -1
                : 0;
    if (r) {
        if (!existed)
            remove_version_dir(dir);
        return -1;
    }

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
