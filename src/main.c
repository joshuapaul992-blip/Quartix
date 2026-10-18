/*
 * Quartix - entry point and command line.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <stdio.h>
#include <wchar.h>

static void usage(void)
{
    qx_log("Quartix %ls - Roblox APK manager for Windows XP x64", QX_VERSION);
    qx_log("usage:");
    qx_log("  quartix info                         show install and data locations");
    qx_log("  quartix import-apk <file.apk>        import the x86_64 Roblox APK");
    qx_log("  quartix apk-ls <file.apk>            list the entries of an APK");
    qx_log("  quartix update                       download and install the latest signed release");
    qx_log("  quartix update --check               only report whether an update is available");
    qx_log("  quartix apply-update <exe> <exe.sig> install a signed, newer build from local files");
}

static int list_visit(const qx_apk_entry *e, void *ctx)
{
    (void)ctx;
    qx_log("%10llu %s %s", (unsigned long long)e->size, e->method == 0 ? "stored " : "deflate", e->name);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    qx_paths p;

    if (argc < 2) {
        usage();
        return 2;
    }
    if (qx_paths_init(&p)) {
        qx_err("cannot set up install/data directories");
        return 1;
    }
    qx_update_cleanup(&p);

    if (wcscmp(argv[1], L"info") == 0) {
        qx_log("version  %ls", QX_VERSION);
        qx_log("install  %ls", p.install_dir);
        qx_log("data     %ls", p.data_dir);
        qx_log("app data %ls", p.app_data_dir);
        qx_log("apks     %ls", p.apk_dir);
        qx_log("updates  %s", qx_update_has_key() ? "signed updates enabled" : "disabled (no signing key in this build)");
        return 0;
    }
    if (wcscmp(argv[1], L"apk-ls") == 0 && argc == 3) {
        if (qx_apk_list(argv[2], list_visit, NULL)) {
            qx_err("not a readable APK");
            return 1;
        }
        return 0;
    }
    if (wcscmp(argv[1], L"import-apk") == 0 && argc == 3) {
        wchar_t dir[QX_PATH_MAX];
        if (qx_apk_import(&p, argv[2], dir, QX_PATH_MAX))
            return 1;
        qx_log("imported to %ls", dir);
        return 0;
    }
    if (wcscmp(argv[1], L"update") == 0 && (argc == 2 || (argc == 3 && wcscmp(argv[2], L"--check") == 0)))
        return qx_update_online(&p, argc == 3) ? 1 : 0;
    if (wcscmp(argv[1], L"apply-update") == 0 && argc == 4) {
        if (qx_update_apply(&p, argv[2], argv[3]))
            return 1;
        qx_log("data dir untouched");
        return 0;
    }
    usage();
    return 2;
}
