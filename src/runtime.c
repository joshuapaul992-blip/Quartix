/*
 * Quartix - the native Roblox runtime (work in progress).
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 *
 * This is the piece that makes Quartix a client rather than a manager: it runs
 * the untouched Android libroblox.so in-process, the way Sober and Mocktail do
 * on Linux/FreeBSD, but on Windows Vista/7/8.1 x64. On those Unix systems the
 * kernel already serves the Android (Linux) ABI - directly on Linux, through the
 * Linuxulator on FreeBSD - so their runtimes only shim bionic, JNI and graphics.
 * Windows serves none of it, so Quartix has to provide that layer itself. The
 * phases below are built and switched on one at a time; until the core phases
 * land, launching inspects the library and reports honestly how far it gets.
 *
 * Phase map (see docs/PLAN.md):
 *   1. locate + inspect the imported ELF            [done]
 *   2. ELF loader: map PT_LOAD, apply relocations   [in progress]
 *   3. Linux syscall + bionic libc shim (the core)  [todo]
 *   4. synthetic JNI / Android framework (libjnivm) [todo]
 *   5. graphics (EGL/GLES -> Vulkan via SDL) + input [todo]
 *   6. audio (OpenSL ES / FMOD bridges)             [todo]
 */
#include "quartix.h"
#include <stdio.h>
#include <wchar.h>

int qx_runtime_inspect(const qx_paths *p, qx_elf_info *info, wchar_t *lib_out, size_t cap)
{
    wchar_t lib[QX_PATH_MAX];

    if (qx_apk_current_lib(p, lib, QX_PATH_MAX)) {
        qx_err("no Roblox APK imported yet; run \"import-apk <file>\" first");
        return -1;
    }
    if (qx_elf_read(lib, info)) {
        qx_err("cannot read the native library at %ls", lib);
        return -1;
    }
    if (lib_out && wcslen(lib) < cap)
        wcscpy(lib_out, lib);
    if (!info->is_elf64_x86_64) {
        qx_err("%ls is not a 64-bit x86-64 ELF (machine 0x%x); Quartix needs the x86_64 Roblox library",
               lib, (unsigned)info->machine);
        return -1;
    }
    return 0;
}

static void report(const qx_elf_info *info, const wchar_t *lib)
{
    unsigned i;
    qx_log("native library  %ls", lib);
    qx_log("  ELF64 x86-64  yes (type %d%s)", info->type, info->type == 3 ? ", shared/PIE" : "");
    qx_log("  entry point   0x%llx", (unsigned long long)info->entry);
    qx_log("  load segments %u, mapped span %.1f MB", info->load_segments,
           (double)info->mapped_size / (1024.0 * 1024.0));
    qx_log("  TLS           %s", info->has_tls ? "yes" : "no");
    if (info->soname[0])
        qx_log("  soname        %s", info->soname);
    qx_log("  needed libs   %u", info->needed_count);
    for (i = 0; i < info->needed_listed; i++)
        qx_log("    - %s", info->needed[i]);
}

int qx_runtime_launch(const qx_paths *p)
{
    qx_elf_info info;
    qx_elf_image img;
    wchar_t lib[QX_PATH_MAX];

    if (qx_runtime_inspect(p, &info, lib, QX_PATH_MAX))
        return -1;
    report(&info, lib);

    /* Phase 2: map the library and apply the relocations that need no symbols. */
    if (qx_elf_load(lib, &img)) {
        qx_err("could not map the native library: %s", qx_last_error());
        return -1;
    }
    qx_log("");
    qx_log("mapped %u load segment(s) at base 0x%llx (%.1f MB reserved)", img.load_segments,
           (unsigned long long)(uintptr_t)img.base, (double)img.span / (1024.0 * 1024.0));
    qx_log("  entry 0x%llx", (unsigned long long)img.entry);
    qx_log("  applied %u relative relocation(s)", img.relative_relocs);
    qx_log("  %u symbolic relocation(s) await the bionic/syscall shim",
           img.symbolic_relocs + img.other_relocs);
    qx_elf_unload(&img);

    qx_log("");
    qx_log("The library is mapped, but the runtime core that executes it");
    qx_log("(Linux syscall + bionic shim, synthetic JNI, and the Vulkan/SDL");
    qx_log("graphics bridge) is still being built, so Roblox does not start yet.");
    qx_log("Login data in %ls is preserved across every build.", p->app_data_dir);
    return QX_RUN_NOT_READY;
}
