/*
 * Quartix - entry point and command line.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <stdio.h>
#include <wchar.h>

static void usage(void)
{
    qx_log("Quartix %ls - native Roblox client runtime for Windows Vista/7/8.1 x64", QX_VERSION);
    qx_log("usage:");
    qx_log("  quartix                              open the Quartix window");
    qx_log("  quartix info                         show install and data locations");
    qx_log("  quartix import-apk <file>             import the x86_64 Roblox APK or bundle (.apks, .xapk, .apkm)");
    qx_log("  quartix run                          run the imported Roblox client");
    qx_log("  quartix run --inspect                inspect the imported native library without running");
    qx_log("  quartix gltest                       test the ANGLE graphics path (needs the ANGLE DLLs)");
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

    if (argc == 2 && (wcscmp(argv[1], L"help") == 0 || wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"/?") == 0)) {
        usage();
        return 0;
    }
    if (qx_paths_init(&p)) {
        qx_err("cannot set up install/data directories");
        if (argc < 2)
            MessageBoxW(NULL, L"Quartix cannot set up its data folder.", L"Quartix", MB_ICONERROR);
        return 1;
    }
    qx_update_cleanup(&p);

    /* No arguments: the window, which is what double-clicking Quartix.exe gives. */
    if (argc < 2)
        return qx_gui_run(&p);

    if (wcscmp(argv[1], L"info") == 0) {
        qx_log("version  %ls", QX_VERSION);
        qx_log("install  %ls", p.install_dir);
        qx_log("data     %ls", p.data_dir);
        qx_log("app data %ls", p.app_data_dir);
        qx_log("apks     %ls", p.apk_dir);
        qx_log("updates  %s", qx_update_has_key() ? "signed updates enabled" : "disabled (no signing key in this build)");
        {
            wchar_t lib[QX_PATH_MAX];
            qx_log("client   %s", qx_apk_current_lib(&p, lib, QX_PATH_MAX) == 0 ? "APK imported" : "no APK imported yet");
        }
        return 0;
    }
    if (wcscmp(argv[1], L"run") == 0 && argc == 2) {
        int r = qx_runtime_launch(&p);
        return r == -1 ? 1 : r; /* QX_RUN_NOT_READY (2) while the runtime core is unfinished */
    }
    if (wcscmp(argv[1], L"run") == 0 && argc == 3 && wcscmp(argv[2], L"--inspect") == 0) {
        qx_elf_info info;
        if (qx_runtime_inspect(&p, &info, NULL, 0))
            return 1;
        qx_log("the imported native library is a loadable 64-bit x86-64 ELF");
        qx_log("  type %d, entry 0x%llx, %u load segment(s), %u needed lib(s)%s",
               info.type, (unsigned long long)info.entry, info.load_segments,
               info.needed_count, info.has_tls ? ", TLS" : "");
        if (info.soname[0])
            qx_log("  soname %s", info.soname);
        return 0;
    }
    if (wcscmp(argv[1], L"gltest") == 0 && argc == 2)
        return qx_gl_smoketest();
    if (wcscmp(argv[1], L"selftest") == 0 && argc == 2) {
        int ft = 0, st = 0, ct = 0, ot = 0, xt = 0, at = 0, kt = 0, gt = 0;
        int ff = qx_format_selftest(&ft);
        int sf = qx_stdio_selftest(&st);
        int cf = qx_scan_selftest(&ct);
        int of = qx_os_selftest(&ot);
        int xf = qx_cstd_selftest(&xt);
        int af = qx_android_selftest(&at);
        int kf = qx_socket_selftest(&kt);
        int gf = qx_gl_selftest(&gt);
        qx_log("formatter self-test: %d check(s), %d failure(s)", ft, ff);
        qx_log("stdio self-test:     %d check(s), %d failure(s)", st, sf);
        qx_log("scanf self-test:     %d check(s), %d failure(s)", ct, cf);
        qx_log("os self-test:        %d check(s), %d failure(s)", ot, of);
        qx_log("libc self-test:      %d check(s), %d failure(s)", xt, xf);
        qx_log("android self-test:   %d check(s), %d failure(s)", at, af);
        qx_log("socket self-test:    %d check(s), %d failure(s)", kt, kf);
        qx_log("graphics self-test:  %d check(s), %d failure(s)", gt, gf);
        qx_log("self-test total:     %d check(s), %d failure(s)", ft + st + ct + ot + xt + at + kt + gt, ff + sf + cf + of + xf + af + kf + gf);
        return (ff + sf + cf + of + xf + af + kf + gf) ? 1 : 0;
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
        return qx_update_online(&p, argc == 3, NULL) < 0 ? 1 : 0;
    if (wcscmp(argv[1], L"apply-update") == 0 && argc == 4) {
        if (qx_update_apply(&p, argv[2], argv[3]))
            return 1;
        qx_log("data dir untouched");
        return 0;
    }
    usage();
    return 2;
}
