#!/usr/bin/env python3
"""End-to-end tests for Quartix.exe, run under Wine (or natively on Windows)."""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

EXE = os.path.abspath(sys.argv[1])
WINE = os.environ.get("WINE", "wine")
failures = 0


def winpath(p):
    if os.name == "nt":
        return p
    return "Z:" + p.replace("/", "\\")


def run(exe, *args, data_dir):
    cmd = ([] if os.name == "nt" else [WINE]) + [exe] + list(args)
    env = dict(os.environ, QUARTIX_DATA_DIR=winpath(data_dir), WINEDEBUG="-all")
    return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)


def check(name, cond, detail=""):
    global failures
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + detail))
    if not cond:
        failures += 1


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def make_apk(path, lib_bytes, compress=zipfile.ZIP_STORED):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", b"\0" * 64, zipfile.ZIP_DEFLATED)
        z.writestr("classes.dex", b"dex\n035\0" + b"x" * 1000, zipfile.ZIP_DEFLATED)
        z.writestr("lib/x86_64/libroblox.so", lib_bytes, compress)


def test_apk(tmp):
    data = os.path.join(tmp, "data")
    lib = b"\x7fELF" + os.urandom(300000)
    apk = os.path.join(tmp, "roblox.apk")
    make_apk(apk, lib)

    r = run(EXE, "apk-ls", winpath(apk), data_dir=data)
    check("apk-ls lists the native library", r.returncode == 0 and "lib/x86_64/libroblox.so" in r.stdout, r.stderr)

    r = run(EXE, "import-apk", winpath(apk), data_dir=data)
    check("import-apk succeeds", r.returncode == 0, r.stdout + r.stderr)
    cur = open(os.path.join(data, "apk", "current")).read().strip()
    check("current points at the APK's hash", cur == sha256(apk)[:16], cur)
    out = os.path.join(data, "apk", cur, "lib", "x86_64", "libroblox.so")
    check("extracted library is byte-identical", os.path.exists(out) and open(out, "rb").read() == lib)
    check("APK copied into the data dir", sha256(os.path.join(data, "apk", cur, "base.apk")) == sha256(apk))

    deflated = os.path.join(tmp, "deflated.apk")
    make_apk(deflated, lib, zipfile.ZIP_DEFLATED)
    r = run(EXE, "import-apk", winpath(deflated), data_dir=data)
    check("deflated library is rejected, not guessed", r.returncode != 0 and "compressed" in r.stderr, r.stderr)

    noarch = os.path.join(tmp, "arm.apk")
    with zipfile.ZipFile(noarch, "w") as z:
        z.writestr("lib/arm64-v8a/libroblox.so", b"\x7fELF")
    r = run(EXE, "import-apk", winpath(noarch), data_dir=data)
    check("APK without x86_64 library is rejected", r.returncode != 0 and "x86_64" in r.stderr, r.stderr)

    junk = os.path.join(tmp, "junk.apk")
    open(junk, "wb").write(os.urandom(5000))
    r = run(EXE, "import-apk", winpath(junk), data_dir=data)
    check("non-zip file is rejected", r.returncode != 0, r.stdout)


def test_update(tmp):
    install = os.path.join(tmp, "install")
    data = os.path.join(tmp, "data")
    os.makedirs(install)
    exe = os.path.join(install, "Quartix.exe")
    shutil.copy(EXE, exe)

    login = os.path.join(data, "app", "com.roblox.client", "shared_prefs", "session.xml")
    os.makedirs(os.path.dirname(login))
    open(login, "w").write("<session>keep-me</session>")

    staged = os.path.join(tmp, "Quartix.new.exe")
    shutil.copy(EXE, staged)
    with open(staged, "ab") as f:
        f.write(b"\0" * 512)  # a different, still valid image
    before = sha256(exe)

    r = run(exe, "apply-update", winpath(staged), "0" * 64, data_dir=data)
    check("update with wrong hash is refused", r.returncode != 0 and "mismatch" in r.stderr, r.stderr)
    check("refused update leaves the executable alone", sha256(exe) == before)

    want = sha256(staged)
    r = run(exe, "apply-update", winpath(staged), want, data_dir=data)
    check("verified update applies", r.returncode == 0, r.stdout + r.stderr)
    check("executable was replaced", sha256(exe) == want)
    check("login data survived the update", open(login).read() == "<session>keep-me</session>")

    r = run(exe, "info", data_dir=data)
    check("updated build runs", r.returncode == 0 and winpath(data).lower() in r.stdout.lower(), r.stdout + r.stderr)
    check("old executable cleaned up on next start", not os.path.exists(exe + ".old"))
    check("login data survived the restart", open(login).read() == "<session>keep-me</session>")


def main():
    for t in (test_apk, test_update):
        tmp = tempfile.mkdtemp(prefix="quartix-")
        try:
            t(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
