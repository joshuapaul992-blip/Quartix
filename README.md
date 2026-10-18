# Quartix

A native Win32 tool for **Windows XP x64** that manages Roblox Android APKs: it imports
an APK, keeps app data (including login data) in a folder that updates never touch,
and updates itself safely.

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

## Roadmap

- **Done:** XP x64 executable, install/data split, APK import, verified self-update,
  CI that checks XP compatibility and runs tests under Wine.
- **Next:** fetch updates over the network (XP x64 has no TLS 1.2, so this needs a
  bundled TLS library) and verify them against a signing key before applying.
- **Later:** a small Win32 window for importing APKs and applying updates.

## License

GPLv3, see [LICENSE](LICENSE).
