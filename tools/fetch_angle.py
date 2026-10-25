#!/usr/bin/env python3
"""Fetch the ANGLE graphics DLLs Quartix binds to, verified by SHA-256.

The engine draws with OpenGL ES 2 through EGL; Quartix binds those imports to
ANGLE's libEGL.dll / libGLESv2.dll (see src/gl.c). Rather than commit ~13 MB of
binaries, this script downloads them from a pinned, trusted release and checks
each one against a known hash, so the provenance is reproducible.

Source: Electron v22.3.27 (win32-x64). Electron 22 is the last Electron line to
support Windows 7/8.1 (23+ require Windows 10), so its ANGLE build is both
Win7-compatible and current, and d3dcompiler_47.dll is Microsoft's shader
compiler as redistributed there. All three are x86-64 PE DLLs.

Usage:
    python3 tools/fetch_angle.py [dest_dir]     # default: build/

Place the result beside Quartix.exe, or point the client at it with the
QUARTIX_ANGLE_DIR environment variable.
"""
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

ELECTRON_VERSION = "v22.3.27"
URL = ("https://github.com/electron/electron/releases/download/"
       f"{ELECTRON_VERSION}/electron-{ELECTRON_VERSION}-win32-x64.zip")

# sha256 of each DLL as shipped in the release above.
DLLS = {
    "libEGL.dll": "85f238fb7ace3cbdf7c29c72b01307c440f13491b07a509cbc5b9f257a637164",
    "libGLESv2.dll": "25d49934fc220b169cadeb21fc99dc2a8fb1dd5a4f244265799392f0f5f2f8f8",
    "d3dcompiler_47.dll": "5653bc7b0e2701561464ef36602ff6171c96bffe96e4c3597359cd7addcba88a",
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def already_present(dest):
    """True when every DLL is already in dest with the expected hash."""
    for name, want in DLLS.items():
        p = os.path.join(dest, name)
        try:
            with open(p, "rb") as f:
                if sha256(f.read()) != want:
                    return False
        except OSError:
            return False
    return True


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else "build"
    os.makedirs(dest, exist_ok=True)

    if already_present(dest):
        print("ANGLE DLLs already present and verified in %s" % dest)
        return 0

    print("Downloading ANGLE DLLs from electron %s ..." % ELECTRON_VERSION)
    # urllib honours http(s)_proxy / HTTPS_PROXY from the environment.
    with urllib.request.urlopen(URL) as r:  # noqa: S310 (pinned https URL)
        blob = r.read()
    print("  got %.1f MB; extracting and verifying" % (len(blob) / 1e6))

    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        names = {os.path.basename(n): n for n in z.namelist()}
        for name, want in DLLS.items():
            if name not in names:
                print("ERROR: %s not found in the archive" % name, file=sys.stderr)
                return 1
            data = z.read(names[name])
            got = sha256(data)
            if got != want:
                print("ERROR: %s hash mismatch\n  expected %s\n  got      %s"
                      % (name, want, got), file=sys.stderr)
                return 1
            with open(os.path.join(dest, name), "wb") as f:
                f.write(data)
            print("  %-20s ok (%d bytes)" % (name, len(data)))

    print("ANGLE DLLs written to %s" % dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
