# Quartix

A native Win32 runtime that aims to run the untouched Roblox **Android** client
on **Windows Vista, 7 and 8.1 (x64)** — the way [Sober][sober] and
[Mocktail][mocktail] run it on Linux and FreeBSD, but on legacy Windows. It loads
the Android `libroblox.so` in-process rather than shipping an Android emulator or
virtual machine, keeps the Roblox login session in a folder that updates never
touch, and updates itself safely.

> **Status: in progress.** The official Windows Roblox client was dropped for
> Windows 7/8/8.1 and never ran on Vista, so Quartix targets the Android build
> instead. The groundwork is done — APK import, the login-preserving data split,
> signed automatic updates, and a runtime that locates and inspects the native
> library. The runtime **core that actually executes Roblox** (a Linux-syscall +
> bionic shim, a synthetic JNI layer, and a Vulkan/SDL graphics bridge) is still
> being built, so Roblox does not launch yet. See [docs/PLAN.md](docs/PLAN.md)
> for the architecture and the phase-by-phase roadmap, including the honest risks.

[sober]: https://sober.vinegarhq.org/
[mocktail]: https://github.com/qutad/mocktail

## Why the Android client, and why a runtime instead of an emulator

Roblox no longer ships a Windows client that runs on Vista/7/8.1, but it still
ships an Android build whose engine is a normal ELF shared object,
`lib/x86_64/libroblox.so`. Sober and Mocktail run that `.so` directly, without an
emulator, because Android is Linux underneath — same ELF binaries, same system
calls. On FreeBSD, Mocktail leans on the **Linuxulator** (the OS's built-in
Linux-syscall layer) to do it.

Windows has no such layer below Windows 10 (that was WSL1), so Quartix has to
build it. That ELF loader plus Linux-syscall/bionic shim is the heart of the
project; graphics (Vulkan) and SDL are the comparatively portable parts and do
run on Windows 7. This is experimental work — [docs/PLAN.md](docs/PLAN.md) is
candid about what is and isn't feasible.

## Using it

Double-click `Quartix.exe` to open its window. It shows the imported APK, imports
a new one, and checks for updates every time it starts.

Everything is also available from a command prompt:

```
Quartix.exe help                         list the commands
Quartix.exe info                         show install and data locations
Quartix.exe import-apk roblox.apk        import the x86_64 Roblox APK (or .apks/.xapk/.apkm bundle)
Quartix.exe run                          run the imported Roblox client
Quartix.exe run --inspect                inspect the imported native library without running
Quartix.exe apk-ls roblox.apk            list what is inside an APK or bundle
Quartix.exe update                       download and install the latest signed release
Quartix.exe update --check               only say whether an update is available
Quartix.exe apply-update new.exe new.exe.sig  install a signed, newer build from local files
```

Quartix does not ship or download the Roblox APK; you provide it. A plain APK or
an APK bundle (`.apks`, `.xapk`, or an unencrypted `.apkm`) both work, as long as
it includes the x86_64 build of the native library. For a bundle, every APK
inside is kept side by side, the base one is saved as `base.apk`, and the library
is taken from the x86_64 split.

### Where things live

| Location | What | Touched by updates |
| --- | --- | --- |
| install dir (next to `Quartix.exe`) | the program | yes |
| `%APPDATA%\Quartix\app\com.roblox.client` | the Roblox app's private storage, including its login session | **never** |
| `%APPDATA%\Quartix\apk` | imported APKs and extracted native libraries | never |
| `%APPDATA%\Quartix\logs` | logs | never |

An update only swaps `Quartix.exe` (rename aside, move new one in, roll back on
failure). The data directory can be moved with the `QUARTIX_DATA_DIR` environment
variable.

### Updates

`Quartix.exe update` fetches `Quartix.exe.sig` from the latest GitHub release
over TLS 1.2 (bundled BearSSL, since these Windows versions lack a modern stack;
certificates are checked against the bundled Mozilla CA list and the host name).
It installs the release only if the signature is made by the project's Ed25519
key, the download matches the signed SHA-256, and the version is newer than the
running one, so downgrades are refused. A build made without a key refuses all
updates. (Extending the bundled TLS to cover 1.1/1.2/1.3 is tracked in the plan.)

### Releasing

One-time setup: the private signing key goes in the repository secret
`QUARTIX_SIGNING_KEY` (Settings > Secrets and variables > Actions; paste the whole
`.pem` file). Its public half is `keys/update_pubkey.txt`.

To release: bump the version in `src/version.h`, merge, then run the **release**
workflow from the Actions tab. It builds, runs the tests, signs, checks the
signature against `keys/update_pubkey.txt` and publishes release `v<version>`.

To change keys: commit the new public key, then sign that one release with the
old key. Installed copies trust it because the old key signed it, and trust only
the new key from then on.

Manual signing still works: `python3 tools/sign_update.py sign <key.pem>
build/Quartix.exe <version>`.

## Building

Cross-compiled on Linux with mingw-w64 against `msvcrt.dll`, which these Windows
versions ship. The image is marked for NT 6.0 (Vista), so it loads on Vista, 7
and 8.1, and `make check` guards against importing any API newer than that floor:

```
sudo apt install gcc-mingw-w64-x86-64 wine64 python3-cryptography xvfb
make            # build/Quartix.exe
make check      # verify the image targets NT 6.0 and imports nothing newer
xvfb-run -a make test WINE=/usr/lib/wine/wine64   # window tests need a display
```

## Roadmap

- **Done:** x64 executable for Vista/7/8.1, install/data split, APK import, signed
  automatic updates over HTTPS with downgrade protection, a native window, release
  workflow, CI that checks compatibility and runs tests under Wine, a runtime that
  locates and inspects the Android native library, and an ELF loader that maps it
  into memory and applies its relative relocations.
- **Next:** the runtime core — the Linux-syscall/bionic shim that resolves the
  remaining relocations and serves the Android ABI, then synthetic JNI, Vulkan/SDL
  graphics and input, and audio. See [docs/PLAN.md](docs/PLAN.md).

## Third-party code

[Monocypher](https://monocypher.org/) 4.0.2 (BSD-2-Clause / CC0) for Ed25519,
vendored unmodified in `third_party/monocypher`.
[miniz](https://github.com/richgel999/miniz) 3.0.2 (MIT), only its inflate, for
compressed APK and bundle entries, vendored unmodified in `third_party/miniz`.
[BearSSL](https://bearssl.org/) 0.6 (MIT) for TLS, vendored unmodified in
`third_party/bearssl`. CA list generated from the Mozilla bundle by
`tools/gen_trust_anchors.sh`.

## License

GPLv3, see [LICENSE](LICENSE).
