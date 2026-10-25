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
    qx_log("  bound %u symbolic relocation(s): %u resolved by the shim, %u pending",
           img.symbolic_relocs, img.symbolic_resolved, img.symbolic_relocs - img.symbolic_resolved);
    if (img.other_relocs)
        qx_log("  %u relocation(s) of unhandled types", img.other_relocs);
    qx_elf_unload(&img);

    qx_log("");
    qx_log("The library is mapped and relocated, but the runtime core that runs it");
    qx_log("(the SysV-ABI bionic/syscall shim, synthetic JNI, and the EGL/GLES");
    qx_log("graphics bridge) is still being built, so Roblox does not start yet.");
    qx_log("Login data in %ls is preserved across every build.", p->app_data_dir);
    return QX_RUN_NOT_READY;
}

/*
 * Phase-4 discovery: map the engine, run its C++ constructors, and call its
 * JNI_OnLoad against Quartix's synthetic JavaVM. Every JNIEnv call the engine
 * makes logs itself, so this prints the real sequence of Java-layer calls the
 * engine expects at startup - the map we build the rest of the JNI layer from.
 *
 * Each stage is logged before it runs, so if the engine traps on a symbol
 * Quartix does not implement yet (or faults in a constructor), the log still
 * shows exactly how far startup got.
 */
int qx_runtime_jnitest(const qx_paths *p)
{
    qx_elf_info info;
    qx_elf_image img;
    wchar_t lib[QX_PATH_MAX];
    void *jni_onload, *gameactivity;
    int version;

    if (qx_runtime_inspect(p, &info, lib, QX_PATH_MAX))
        return -1;
    if (qx_elf_load(lib, &img)) {
        qx_err("could not map the native library: %s", qx_last_error());
        return -1;
    }
    qx_log("mapped at base 0x%llx; %u relative + %u symbolic reloc(s) (%u pending)",
           (unsigned long long)(uintptr_t)img.base, img.relative_relocs,
           img.symbolic_relocs, img.symbolic_relocs - img.symbolic_resolved);

    /* Locate the entry points before running anything, so we know they exist. */
    jni_onload = qx_elf_sym(&img, "JNI_OnLoad");
    gameactivity = qx_elf_sym(&img,
        "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
    qx_log("JNI_OnLoad           %s", jni_onload ? "found" : "NOT found");
    qx_log("GameActivity init    %s", gameactivity ? "found" : "NOT found");
    if (jni_onload)
        qx_log("  JNI_OnLoad at 0x%llx", (unsigned long long)(uintptr_t)jni_onload);

    /* With QUARTIX_DUMP_PENDING set, list every still-unimplemented import so we
     * can plan what to build next, then stop before running anything. */
    if (GetEnvironmentVariableW(L"QUARTIX_DUMP_PENDING", NULL, 0) > 0) {
        int n;
        qx_log("");
        n = qx_shim_dump_pending();
        qx_log("%d symbol(s) still unimplemented", n);
        qx_elf_unload(&img);
        return 0;
    }

    /* Run the engine's static constructors. This is where global C++ objects
     * are built; it must happen before JNI_OnLoad. */
    qx_log("");
    qx_log("running %llu constructor(s) (DT_INIT%s + DT_INIT_ARRAY)...",
           (unsigned long long)(img.init_array_n + (img.init_func ? 1 : 0)),
           img.init_func ? "" : " absent");
    qx_elf_run_init(&img);
    qx_log("constructors returned");

    if (!jni_onload) {
        qx_log("no JNI_OnLoad to call; stopping after constructors");
        qx_elf_unload(&img);
        return QX_RUN_NOT_READY;
    }

    qx_log("");
    qx_log("calling JNI_OnLoad with the synthetic JavaVM...");
    version = qx_jni_call_onload(jni_onload);
    qx_log("JNI_OnLoad returned 0x%x", (unsigned)version);

    qx_elf_unload(&img);
    return 0;
}
