# Quartix plan: a native Roblox runtime for Windows Vista / 7 / 8.1 x64

## Goal

Run the untouched Roblox **Android** client on 64-bit Windows Vista, 7 and 8.1,
the way [Sober](https://sober.vinegarhq.org/) and
[Mocktail](https://github.com/qutad/mocktail) run it on Linux and FreeBSD: as a
user-space runtime that loads `libroblox.so` in-process, rather than a full
Android emulator or virtual machine. Keep it as native, as simple, as robust and
as low-ban-risk as that approach allows, with signed automatic updates that never
touch the Roblox login session.

## Why this is the architecture (and where the hard part is)

The official Windows Roblox client was discontinued for Windows 7/8/8.1 (and
never ran on Vista), so there is no native Windows binary to launch on these
systems. The remaining option is the Android build of Roblox, which ships its
engine as `lib/x86_64/libroblox.so`.

Sober and Mocktail run that `.so` without an emulator because **Android is
Linux**: the Android libraries are ELF, make Linux system calls, and expect a
POSIX-ish C library. On Linux the kernel serves that ABI directly; on FreeBSD
Mocktail runs through the **Linuxulator**, FreeBSD's in-kernel Linux-syscall
layer. So those runtimes only have to shim the parts above the kernel: bionic
libc, the Java/JNI layer, graphics and audio.

Windows serves none of the Linux ABI. Microsoft's equivalent of the Linuxulator
was WSL1, which is Windows 10 only. So on Vista/7/8.1 Quartix has to provide that
missing layer itself. **The ELF loader plus the Linux-syscall and bionic shim is
the core of the project and the part most likely to make or break it.** Graphics
(Vulkan) and SDL are comparatively portable and do run on Windows 7.

This is experimental by nature — Mocktail labels itself experimental even on an
OS that gives it the syscall layer for free. We build and validate each phase on
real hardware before claiming it works.

## Phases

1. **Locate + inspect the imported ELF** — *done.* `quartix run --inspect`
   resolves `apk\current`, parses the ELF64 header, program headers and dynamic
   section, and reports the load segments, needed libraries, soname and TLS. See
   `src/elf.c`.
2. **ELF loader** — *done (`src/loader.c`).* `quartix run` reserves one span,
   copies the `PT_LOAD` segments to a load bias, applies the relocations, and
   sets each segment's final page protection. `libroblox.so` packs its relative
   relocations in **Android's APS2 format** (`DT_ANDROID_RELA`), not a plain
   `RELA` table, so the loader decodes the APS2 sleb128 stream as well as the
   standard `RELA`/PLT tables. Symbolic relocations (`GLOB_DAT`, `JUMP_SLOT`,
   64-bit) are bound through the shim resolver (`src/shim.c`); unresolved names
   bind to a SysV-ABI trap so the image stays complete. Verified against the real
   `libroblox.so`: **628,016** relative relocations applied (matching an
   independent decoder) and 637 symbolic bound. Nothing transfers control to the
   mapped code yet — that is phase 3.

   What the real binary (Roblox 2.744.1081, API 26) told us, now driving the
   remaining phases:
   - It needs 10 Android system libraries — `libc`, `libm`, `libdl`, `liblog`,
     `libandroid`, `libEGL`, `libGLESv2`, `libOpenSLES`, `libOpenMAXAL`,
     `libmediandk` — and imports **585** symbols from them.
   - Graphics is **OpenGL ES 2 + EGL** (~91 entry points), so the Windows side
     needs a GLES/EGL translation rather than raw desktop GL. Quartix vendors
     **ANGLE** for this (phase 5) instead of writing it by hand.
   - It is almost entirely NDK-native: only **1** JNI symbol, so very little of a
     Java VM is required, but it does expect the `libandroid` NativeActivity
     surface (`ALooper`, `AAssetManager`, `ANativeWindow`, `AInputQueue`, …).
   - It is linked `BIND_NOW` with 2968 `INIT_ARRAY` initializers that must run
     after relocation, once the shim can satisfy the symbols they touch.
3. **Linux syscall + bionic shim** — *in progress (`src/shim.c`).* Real,
   **SysV-ABI** implementations of the symbols the engine imports. Every such
   function is `__attribute__((sysv_abi))`, because the Android code uses the
   System V calling convention and Windows uses the Microsoft one — the one
   thing Linux/FreeBSD give Sober/Mocktail for free. As symbols become real,
   `qx_shim_resolve` returns them and the loader's resolved count rises.
   - *Done:* the pure libc (memory/string, `qsort`/`bsearch` with a SysV-ABI
     comparator, `__errno`, `abort`, `exit`, `getenv`), libm math, non-variadic
     liblog, and libdl (`dlsym` routes back through the resolver).
     (Note: Roblox bundles its own allocator, so `malloc`/`free` are not
     imported.)
   - *Done:* pthreads and scheduling (`src/thread.c`) — the 44 `pthread_*`
     symbols plus `sched_*` and `__cxa_thread_atexit_impl`, mapped onto Win32
     primitives that all exist on the Vista floor: `CRITICAL_SECTION` for
     mutexes, `CONDITION_VARIABLE` for condition variables, a writer-preferring
     reader/writer lock built from the two, Win32 threads for `pthread_create`,
     and fibre-local storage (FLS) — whose per-thread exit callback is the one
     Win32 mechanism that runs pthread key destructors and C++ `thread_local`
     teardown. The bionic objects are keyed by address in a side table rather
     than stored in place, so the static initialisers (`PTHREAD_MUTEX_INITIALIZER`
     and friends) work without assuming any bionic struct size.
   - *Done:* the variadic printf/log family (`src/format.c`) — `printf`,
     `snprintf`, `vsnprintf`, the fortified `__vsnprintf_chk`/`__vsprintf_chk`,
     `syslog`, `__android_log_print`, `__android_log_assert`, `vasprintf`, plus
     the non-variadic string-to-number functions (`strtod`/`strtof`/`strtoll`/
     `strtoull`/`atof`/`atoi`/`atol`). The engine passes varargs System V style
     (registers + a register save area) while mingw's default `va_list` walks the
     Microsoft stack convention, so the formatter reads `__builtin_sysv_va_list`
     through the `sysv`-specific builtins; each function is `sysv_abi` **and**
     `noinline`, so the SysV-variadic prologue is never inlined into a
     Microsoft-ABI frame. Because the engine is LP64, `%ld` carries a 64-bit
     argument (long is 32-bit on this Win64 host), so the length modifier is
     parsed here to size each `va_arg`; each conversion's rendering is then
     delegated to the host's mingw ANSI `snprintf`. A `selftest` subcommand runs
     the formatter the way the engine calls it (a SysV→SysV variadic call) and is
     checked in CI under Wine.
   - *Done:* the stdio / file surface (`src/stdio.c`) — the `FILE*` stream
     functions (`fopen`/`fclose`/`fdopen`/`fread`/`fwrite`/`fseek`/`ftell`/
     `fgets`/`fputc`/`fputs`/`getc`/`puts`/`feof`/`ferror`/`fflush`/`setvbuf`),
     the raw descriptors (`open`/`close`/`read`/`write`/`lseek`), the path
     operations (`access`/`mkdir`/`remove`/`rename`/`unlink`), the three
     standard streams as data symbols, and `fprintf`/`vfprintf` (in
     `src/format.c`, which have the variadic machinery). All are backed by the
     host CRT: a `FILE*` handed to the engine is a CRT `FILE*` and a descriptor
     is a CRT descriptor, so follow-on calls forward straight through. Paths are
     converted UTF-8→UTF-16 and the `_w*` entry points used; `open()` flags are
     translated from Linux `O_*` to the host `_O_*` with `_O_BINARY` forced;
     `fseek`/`ftell`/`lseek` use 64-bit offsets (LP64); host `errno` is
     propagated into the engine's `__errno` location on failure; and
     `stdout`/`stderr` text is routed into the Quartix log. A `stdio` self-test
     round-trips a file through the stream and raw paths and is run in CI under
     Wine.
   - *Done:* filesystem metadata and the `scanf` family. `stat`/`fstat`/`lstat`
     (`src/stdio.c`) fill the bionic `struct stat` — whose x86-64 layout is the
     144-byte Linux kernel one, checked at compile time — from the host CRT's
     `_stat64`, and `ungetc` was added. The `scanf` family (`src/scan.c`:
     `sscanf`/`vsscanf`/`fscanf`) is one scanner over a character source that is
     either a string or a `FILE*`; like the printf family it reads the engine's
     register-passed result pointers through `__builtin_sysv_va_list`, and it
     handles `d i u o x f e g s c [set] n %%` with width, length (LP64 `l` →
     64-bit) and assignment suppression. A `scanf` self-test is run in CI.
   - *Done:* the OS services (`src/os.c`) — time (`time`, `gettimeofday`,
     `clock_gettime`/`clock_getres`, `clock`, `difftime`, `nanosleep`), the
     calendar (`gmtime`/`localtime` and their `_r` forms, `mktime`, `strftime`,
     `tzset`, filling a 56-byte bionic `struct tm` checked at compile time),
     virtual memory (`mmap`/`munmap`/`mprotect`/`madvise`/`getpagesize` onto
     `VirtualAlloc`/`VirtualProtect`/`VirtualFree`, anonymous mappings only), and
     the process/system queries (`getpid`, `gettid`, `getuid`, `sysconf` with the
     bionic `_SC_*` names, `getcwd`, and `raise`/`signal`/`sigaction` as accepting
     no-ops, since Win32 has no POSIX signals). `clock_gettime(CLOCK_MONOTONIC)`
     is based on `GetTickCount64`, the same base `pthread_cond_timedwait` uses in
     `src/thread.c`, so a monotonic deadline the engine computes lines up with the
     wait. An `os` self-test is run in CI.
   - *Done:* the rest of the pure libc/libm surface (`src/cstd.c`) — the `float`
     math variants (`sinf`/`powf`/…) and hyperbolics, the rounding/decomposition
     calls (`lround`/`llround` returning the LP64 64-bit value, `modf`,
     `frexpf`), the bionic `_FORTIFY_SOURCE` helpers (`__memcpy_chk`,
     `__strcpy_chk`, `__fread_chk`, the `fd_set` `_chk` forms, …) which perform
     the real operation guarded by the caller-supplied size, real UTF-8
     `mbrtowc`/`wcrtomb` (reading and writing 32-bit code points, since the
     engine's `wchar_t` is 32-bit while the host's is 16-bit), the `_l` ctype
     classifiers, stdlib odds and ends (`atoll`/`strnlen`/`strcasecmp`/`rand`),
     and the C++/runtime glue the `INIT_ARRAY` constructors touch — a LIFO
     `__cxa_atexit`/`__cxa_finalize` registry, `__assert`, `_Exit`/`_exit`,
     `arc4random_buf` (from the Windows CSP), and the no-op-on-Windows calls
     (`__register_atfork`, `getauxval`, `__system_property_get`). A libc
     self-test is run in CI.
   - Against the real `libroblox.so` the shim resolves **336 of 637** symbolic
     relocations; with phase 4 started (below) the pure-libc surface is complete
     and the first Android-framework family is in.
   - *Next:* the remaining pending symbols are the rest of the Android framework
     — EGL/GLES graphics, vendored from **ANGLE** rather than hand-written
     (phase 5), and `libmediandk`/audio (phase 6) — plus the BSD sockets the
     engine uses for networking, which get their own Winsock bridge.
4. **libandroid NativeActivity surface** — *in progress (`src/android.c`).* The
   engine is an NDK NativeActivity app (only **1** JNI symbol, so almost no Java
   VM is needed), so it drives its lifecycle and I/O through the `libandroid` C
   API. This slice supplies that surface: `AAssetManager`/`AAsset` (reading a
   packaged asset into a byte buffer, backed by a filesystem asset root the
   runtime points at the APK's assets), `AConfiguration` (locale, screen size,
   density), `ALooper` (a real per-thread event loop whose `pollOnce` waits up
   to the caller's timeout), `ANativeWindow` (a refcounted handle reporting its
   dimensions; binding it to a real `HWND`/EGL surface is phase 5), and the
   graphics-interop objects `AHardwareBuffer`/`AImageReader`/`AImage` (safe
   placeholders until the graphics phase). These are framework entry points:
   they resolve and are validated structurally (an `android` self-test exercises
   the asset/config/looper/window mechanics in CI), with their full behavior
   exercised once the window and ANGLE land and the client runs on real
   hardware. A tiny synthetic JNI shim for the single JNI symbol and the
   `ANativeActivity` glue lands alongside the window in phase 5.
5. **Graphics + input** — the engine draws with **OpenGL ES 2 + EGL** (~91 entry
   points). Rather than hand-write a GLES→desktop translation, Quartix vendors
   **[ANGLE](https://github.com/google/angle)**, Google's production GLES/EGL
   implementation — the same one Chrome ships — and binds the engine's `libEGL`
   and `libGLESv2` imports straight to ANGLE's own `libEGL.dll`/`libGLESv2.dll`.
   ANGLE already does exactly the job this phase needs (turn GLES2+EGL into a
   native Windows graphics API), it is battle-tested, and reusing it removes the
   single largest hand-written surface from the project. ANGLE's **D3D11** backend
   covers Windows 7 and 8.1; its **D3D9** backend reaches the Vista floor, so the
   backend is chosen at runtime from what the host supports. The remaining
   Quartix-side work is small: create the window, hand ANGLE a native window
   handle for its `EGLNativeWindowType`, drive the `ANativeWindow`/`AInputQueue`
   surface from phase 4, and feed keyboard/mouse events back in. The ANGLE DLLs
   are vendored as a pinned, unmodified third-party build (like Monocypher/miniz/
   BearSSL) and ship beside `Quartix.exe`.

   *Why this is the big accelerator:* graphics was the largest remaining
   hand-written phase. Vendoring ANGLE collapses ~91 GLES/EGL entry points from
   "implement and validate by hand" down to "bind imports to a trusted DLL",
   which is why it moves the timeline more than any other single change.
6. **Audio** — bridge OpenSL ES / OpenMAX AL to a Windows audio backend
   (`dsound.dll`/`winmm.dll` on the Vista floor).

## Kept from the APK-manager groundwork

These already work and are preserved unchanged:

- **APK / bundle import** with the Roblox login session kept in a data directory
  that updates never touch (`%APPDATA%\Quartix\app\com.roblox.client`).
- **Signed automatic updates** over TLS from GitHub Releases (Ed25519 signature,
  SHA-256, downgrade protection), which only ever swap `Quartix.exe`.

## TLS

The bundled BearSSL does TLS 1.0–1.2. The requested TLS 1.1/1.2/1.3 coverage
needs a 1.3-capable stack that still builds for the Vista floor (e.g. mbedTLS or
wolfSSL); swapping it in is tracked as its own change so the update path's
security review stays focused.

## Ban-risk note

Running the untouched client in an unusual environment is inherently more
detectable than an official client. We minimise risk by not modifying the APK or
the engine, presenting a coherent Android-like environment, and avoiding any
injection or tampering. Zero risk is not achievable and is not promised.
