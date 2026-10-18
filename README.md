# Quartix

A Roblox launcher for **Windows XP x64**, in the spirit of [Sober](https://sober.vinegarhq.org/):
it runs the official Roblox **Android** app (the x86_64 build from the APK) on a native
Win32 host, instead of the Windows client, which has required Windows 10 since 2024.

> **Status: early and experimental.** Quartix can import an APK, keep login data safe
> across updates, and update itself. It cannot run Roblox yet. See the roadmap below for
> what stands between here and a playable game, including one problem that may turn out
> to have no clean solution.

## Using it

```
Quartix.exe info                         show install and data locations
Quartix.exe import-apk roblox.apk        import the x86_64 Roblox APK
Quartix.exe apk-ls roblox.apk            list what is inside an APK
Quartix.exe apply-update new.exe <sha256> replace Quartix.exe with a verified build
```

Quartix does not ship or download the Roblox APK; you provide it.

### Where things live

| Location | What | Touched by updates |
| --- | --- | --- |
| install dir (next to `Quartix.exe`) | the program | yes |
| `%APPDATA%\Quartix\app\com.roblox.client` | the Roblox app's private storage, including its login session | **never** |
| `%APPDATA%\Quartix\apk` | imported APKs and extracted native libraries | never |
| `%APPDATA%\Quartix\logs` | logs | never |

An update only swaps `Quartix.exe` (rename aside, move new one in, roll back on
failure) after checking its SHA-256. The data directory can be moved with the
`QUARTIX_DATA_DIR` environment variable.

## Building

Cross-compiled on Linux with mingw-w64 against `msvcrt.dll`, which XP x64 ships:

```
sudo apt install gcc-mingw-w64-x86-64 wine64
make            # build/Quartix.exe
make check      # verify the image targets NT 5.2 and imports nothing newer
make test WINE=/usr/lib/wine/wine64
```

## Ban risk

Quartix aims to look to Roblox like an ordinary Android device running the
unmodified official app: no anti-cheat bypass, no patching of game code, no
tampering with network traffic, and an honest device profile. That is the same
model Sober uses. It is still an unofficial way to run the client, so the risk is
low but not zero.

## Roadmap

1. **Host skeleton** (done): XP x64 executable, install/data split, APK import,
   verified self-update, CI that checks XP compatibility and runs tests under Wine.
2. **ELF loader spike**: load `libroblox.so` into the Windows process, resolve its
   imports against our own Bionic-compatible C library shim (Android code uses the
   System V calling convention, Windows uses the Microsoft one, so every shim is
   declared `sysv_abi`). The open question this spike answers is **thread-local
   storage**: Android x86_64 code reads its thread pointer and stack canary through
   the `%fs` segment, and Windows gives user programs no supported way to point `%fs`
   at their own data. If no clean route exists, the alternatives involve rewriting
   those instructions in the game library, which would raise ban risk; that call
   will be put to the project before anything is built on it.
3. **Android framework shim**: the JNI environment and the Java-side services the
   engine calls (activity lifecycle, storage paths, input, clipboard, webview for
   sign-in and captcha).
4. **Graphics**: Roblox on Android needs OpenGL ES 3 or Vulkan. XP has no Vulkan;
   the plan is an OpenGL ES 3 context through the `WGL_EXT_create_context_es2_profile`
   extension that some XP-era NVIDIA drivers expose, with a translation layer to
   desktop OpenGL as the fallback.
5. **Audio and input**: OpenSL ES/AAudio onto DirectSound or waveOut; Win32 mouse
   and keyboard as Android input events.
6. **Network self-update**: XP x64 has no TLS 1.2, so updates are fetched with a
   bundled TLS library and verified against a signing key before `apply-update`.
7. **Launcher UI**: a small Win32 window for import, launch and updates.

## License

GPLv3, see [LICENSE](LICENSE).
