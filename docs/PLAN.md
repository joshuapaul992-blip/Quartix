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
     needs a GLES/EGL translation (e.g. ANGLE) rather than raw desktop GL.
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
   - Against the real `libroblox.so` the shim now resolves **131 of 637**
     symbolic relocations.
   - *Next:* the stdio FILE surface (`fopen`/`fread`/`fwrite`/`fputs`/`stderr`/
     `stdout` and the raw `open`/`read`/`write`, ~25 symbols, which `fprintf`/
     `fscanf`/`sscanf` join), the remaining non-variadic libc (time/
     `clock_gettime`, which must share its monotonic base with
     `pthread_cond_timedwait`), and a handler for the Linux x86-64 syscalls
     bionic makes underneath (memory, futex, files, time).
4. **Synthetic JNI / Android framework** — a minimal JNI environment (in the
   spirit of `libjnivm`) so the engine's calls into "Java" for device info,
   networking, windowing and lifecycle resolve to host implementations.
5. **Graphics + input** — translate the engine's `ANativeWindow` / EGL / GLES
   calls to a Vulkan surface presented through SDL, and feed keyboard/mouse back
   in. Vulkan and SDL are available on Windows 7.
6. **Audio** — bridge OpenSL ES / FMOD to a Windows audio backend
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
