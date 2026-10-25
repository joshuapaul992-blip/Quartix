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
2. **ELF loader** — *mapping + relative relocations done (`src/loader.c`).*
   `quartix run` reserves one span, copies the `PT_LOAD` segments to a load bias,
   applies the `R_X86_64_RELATIVE` relocations (verified against `readelf`), and
   sets each segment's final page protection. The symbolic relocations
   (`GLOB_DAT`, `JUMP_SLOT`, 64-bit) are counted and left for phase 3, which
   resolves them; nothing transfers control to the mapped code yet.
3. **Linux syscall + bionic shim** — *the core.* A user-space handler for the
   Linux x86-64 syscalls the engine and bionic use (memory, threads/futex,
   files, sockets, time, `ioctl`), mapped onto Win32/NT, plus bionic libc
   entry points (`pthread_*`, `malloc`, string/stdio). This is the Linuxulator
   equivalent that Windows does not provide.
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
