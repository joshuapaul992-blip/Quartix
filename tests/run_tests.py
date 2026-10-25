#!/usr/bin/env python3
"""End-to-end tests for Quartix.exe, run under Wine (or natively on Windows)."""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

EXE = os.path.abspath(sys.argv[1])
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGN = os.path.join(ROOT, "tools", "sign_update.py")
WINE = os.environ.get("WINE", "wine")
failures = 0


def winpath(p):
    if os.name == "nt":
        return p
    return "Z:" + p.replace("/", "\\")


def run(exe, *args, data_dir, env=None):
    cmd = ([] if os.name == "nt" else [WINE]) + [exe] + list(args)
    env = dict(os.environ, QUARTIX_DATA_DIR=winpath(data_dir), WINEDEBUG="-all", **(env or {}))
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
    cur = open(os.path.join(data, "apk", "current")).read().strip()
    out = os.path.join(data, "apk", cur, "lib", "x86_64", "libroblox.so")
    check("deflated library is inflated", r.returncode == 0 and open(out, "rb").read() == lib, r.stderr)

    corrupt = os.path.join(tmp, "corrupt.apk")
    raw = bytearray(open(deflated, "rb").read())
    at = raw.index(b"lib/x86_64/libroblox.so") + len("lib/x86_64/libroblox.so") + 2000
    raw[at] ^= 0xFF
    open(corrupt, "wb").write(raw)
    r = run(EXE, "import-apk", winpath(corrupt), data_dir=data)
    check("corrupt deflated library is rejected", r.returncode != 0 and "corrupt" in r.stderr, r.stderr)
    check("failed import leaves current alone", open(os.path.join(data, "apk", "current")).read().strip() == cur)
    check("failed import removes its folder", not os.path.exists(os.path.join(data, "apk", sha256(corrupt)[:16])))

    noarch = os.path.join(tmp, "arm.apk")
    with zipfile.ZipFile(noarch, "w") as z:
        z.writestr("lib/arm64-v8a/libroblox.so", b"\x7fELF")
    r = run(EXE, "import-apk", winpath(noarch), data_dir=data)
    check("APK without x86_64 library is rejected", r.returncode != 0 and "x86_64" in r.stderr, r.stderr)

    test_bundles(tmp, data, lib)

    junk = os.path.join(tmp, "junk.apk")
    open(junk, "wb").write(os.urandom(5000))
    r = run(EXE, "import-apk", winpath(junk), data_dir=data)
    check("non-zip file is rejected", r.returncode != 0, r.stdout)


def zip_bytes(entries, compress=zipfile.ZIP_DEFLATED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in entries:
            z.writestr(name, data, compress)
    return buf.getvalue()


def test_bundles(tmp, data, lib):
    base = zip_bytes([("AndroidManifest.xml", b"\0" * 64), ("classes.dex", b"dex\n035\0" + b"x" * 1000)])
    split = zip_bytes([("AndroidManifest.xml", b"\1" * 64), ("lib/x86_64/libroblox.so", lib)], zipfile.ZIP_STORED)
    arm = zip_bytes([("lib/arm64-v8a/libroblox.so", b"\x7fELF")])

    def imported(path):
        r = run(EXE, "import-apk", winpath(path), data_dir=data)
        cur = open(os.path.join(data, "apk", "current")).read().strip()
        return r, os.path.join(data, "apk", cur)

    # .apks (SAI / bundletool): splits/ folder, outer zip deflated.
    apks = os.path.join(tmp, "roblox.apks")
    open(apks, "wb").write(zip_bytes([("toc.pb", b"x"), ("splits/base-master.apk", base),
                                      ("splits/base-x86_64.apk", split), ("splits/base-arm64_v8a.apk", arm),
                                      ("standalones/standalone-x86_64.apk", b"ignored")]))
    r, d = imported(apks)
    check(".apks bundle imports", r.returncode == 0, r.stdout + r.stderr)
    check(".apks base becomes base.apk", open(os.path.join(d, "base.apk"), "rb").read() == base)
    check(".apks splits are kept", open(os.path.join(d, "base-x86_64.apk"), "rb").read() == split)
    check(".apks library comes from the x86_64 split",
          open(os.path.join(d, "lib", "x86_64", "libroblox.so"), "rb").read() == lib)
    check(".apks standalones are skipped", not os.path.exists(os.path.join(d, "standalone-x86_64.apk")))
    check(".apks current points at the bundle's hash", os.path.basename(d) == sha256(apks)[:16])

    # .xapk (APKPure): APKs at the top level next to a manifest.
    xapk = os.path.join(tmp, "roblox.xapk")
    open(xapk, "wb").write(zip_bytes([("manifest.json", b"{}"), ("icon.png", b"png"),
                                      ("com.roblox.client.apk", base), ("config.x86_64.apk", split)],
                                     zipfile.ZIP_STORED))
    r, d = imported(xapk)
    check(".xapk bundle imports", r.returncode == 0, r.stdout + r.stderr)
    check(".xapk base becomes base.apk", open(os.path.join(d, "base.apk"), "rb").read() == base)
    check(".xapk library extracted", open(os.path.join(d, "lib", "x86_64", "libroblox.so"), "rb").read() == lib)
    check(".xapk non-APK files are not copied", not os.path.exists(os.path.join(d, "manifest.json")))

    # Names that try to climb out of the version folder are skipped.
    evil = os.path.join(tmp, "evil.apks")
    open(evil, "wb").write(zip_bytes([("base.apk", base), ("../../escape.apk", split), ("..\\up.apk", split),
                                      ("x/config.x86_64.apk", split)]))
    r, d = imported(evil)
    check("bundle with hostile names still imports", r.returncode == 0, r.stdout + r.stderr)
    check("hostile names are not written outside", not os.path.exists(os.path.join(data, "escape.apk")) and
          not os.path.exists(os.path.join(data, "apk", "escape.apk")) and
          not os.path.exists(os.path.join(data, "apk", "up.apk")))

    before = open(os.path.join(data, "apk", "current")).read()
    noarch = os.path.join(tmp, "arm.apks")
    open(noarch, "wb").write(zip_bytes([("splits/base-master.apk", base), ("splits/base-arm64_v8a.apk", arm)]))
    r = run(EXE, "import-apk", winpath(noarch), data_dir=data)
    check("bundle without x86_64 split is rejected", r.returncode != 0 and "x86_64 split" in r.stderr, r.stderr)
    check("rejected bundle leaves current alone", open(os.path.join(data, "apk", "current")).read() == before)
    check("rejected bundle leaves no folder", not os.path.exists(os.path.join(data, "apk", sha256(noarch)[:16])))


def make_elf64_x86_64():
    """A minimal but valid little-endian ELF64 x86-64 shared object header with
    one PT_LOAD segment - enough for the runtime inspector to recognise it."""
    import struct
    ehdr = (b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8 +
            struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0))
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, 120, 120, 0x1000)  # PT_LOAD
    return ehdr + phdr


def _sleb(v):
    """Signed LEB128, matching what the loader's APS2 decoder reads."""
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if (v == 0 and not (b & 0x40)) or (v == -1 and (b & 0x40)):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def make_elf_with_aps2():
    """A one-PT_LOAD ELF64 x86-64 whose DT_ANDROID_RELA section carries two
    R_X86_64_RELATIVE relocations in Android's APS2 packed format. Exercises the
    loader's APS2 decoder without the real 117 MB library. vaddr == file offset."""
    import struct
    TOTAL, DYN, BLOB, R1, R2 = 0x1000, 0x100, 0x200, 0x300, 0x308
    img = bytearray(TOTAL)
    img[0:64] = (b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8 +
                 struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0))
    img[64:120] = struct.pack("<IIQQQQQQ", 1, 7, 0, 0, 0, TOTAL, TOTAL, 0x1000)   # PT_LOAD rwx
    img[120:176] = struct.pack("<IIQQQQQQ", 2, 6, DYN, DYN, DYN, 48, 48, 8)       # PT_DYNAMIC
    # APS2: count=2, initial offset=0; one group of 2, flags=HAS_ADDEND(8).
    blob = b"APS2" + _sleb(2) + _sleb(0) + _sleb(2) + _sleb(8)
    blob += _sleb(R1) + _sleb(8) + _sleb(0x1111)   # +0x300 -> R1, R_X86_64_RELATIVE, addend
    blob += _sleb(R2 - R1) + _sleb(8) + _sleb(0x2222)  # +8 -> R2
    img[BLOB:BLOB + len(blob)] = blob
    dyn = struct.pack("<QQQQQQ", 0x60000011, BLOB, 0x60000012, len(blob), 0, 0)  # ANDROID_RELA, RELASZ, NULL
    img[DYN:DYN + len(dyn)] = dyn
    return bytes(img)


def make_elf_with_symbol(symname=b"memcpy"):
    """A one-PT_LOAD ELF64 x86-64 with one R_X86_64_JUMP_SLOT relocation against
    the named symbol, so the loader must resolve it through the shim. Exercises
    symbol resolution (DT_SYMTAB/DT_STRTAB + DT_JMPREL)."""
    import struct
    TOTAL, DYN, SYMTAB, DYNSTR, JMPREL, GOT = 0x1000, 0x100, 0x200, 0x240, 0x2c0, 0x340
    strtab = b"\x00" + symname + b"\x00"
    img = bytearray(TOTAL)
    img[0:64] = (b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8 +
                 struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0))
    img[64:120] = struct.pack("<IIQQQQQQ", 1, 7, 0, 0, 0, TOTAL, TOTAL, 0x1000)   # PT_LOAD rwx
    img[120:176] = struct.pack("<IIQQQQQQ", 2, 6, DYN, DYN, DYN, 96, 96, 8)       # PT_DYNAMIC
    # dynsym: index 0 null, index 1 -> symname; Elf64_Sym is 24 bytes.
    img[SYMTAB:SYMTAB + 24] = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    img[SYMTAB + 24:SYMTAB + 48] = struct.pack("<IBBHQQ", 1, 0x12, 0, 0, 0, 0)    # st_name=1
    img[DYNSTR:DYNSTR + len(strtab)] = strtab
    img[JMPREL:JMPREL + 24] = struct.pack("<QQq", GOT, (1 << 32) | 7, 0)          # JUMP_SLOT, sym 1
    dyn = struct.pack("<QQQQQQQQQQQQ",
                      6, SYMTAB,          # DT_SYMTAB
                      5, DYNSTR,          # DT_STRTAB
                      10, len(strtab),    # DT_STRSZ
                      23, JMPREL,         # DT_JMPREL
                      2, 24,              # DT_PLTRELSZ
                      0, 0)               # DT_NULL
    img[DYN:DYN + len(dyn)] = dyn
    return bytes(img)


def test_runtime(tmp):
    data = os.path.join(tmp, "data")

    r = run(EXE, "run", "--inspect", data_dir=data)
    check("run --inspect with no APK is rejected", r.returncode != 0 and "import-apk" in r.stderr, r.stderr)

    apk = os.path.join(tmp, "roblox.apk")
    make_apk(apk, make_elf64_x86_64())
    r = run(EXE, "import-apk", winpath(apk), data_dir=data)
    check("runtime test APK imports", r.returncode == 0, r.stdout + r.stderr)

    r = run(EXE, "run", "--inspect", data_dir=data)
    check("run --inspect recognises the x86-64 ELF",
          r.returncode == 0 and "64-bit x86-64 ELF" in r.stdout, r.stdout + r.stderr)

    r = run(EXE, "run", data_dir=data)
    check("run reports the core is unfinished (exit 2)",
          r.returncode == 2 and "does not start yet" in r.stdout, str(r.returncode) + r.stdout + r.stderr)
    check("run maps the library's load segment", "mapped 1 load segment" in r.stdout, r.stdout)
    check("run promises login data is preserved", "preserved" in r.stdout, r.stdout)

    aps2 = os.path.join(tmp, "aps2.apk")
    make_apk(aps2, make_elf_with_aps2())
    run(EXE, "import-apk", winpath(aps2), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run applies APS2 packed relative relocations",
          "applied 2 relative relocation(s)" in r.stdout, r.stdout + r.stderr)

    sym = os.path.join(tmp, "sym.apk")
    make_apk(sym, make_elf_with_symbol())
    run(EXE, "import-apk", winpath(sym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a known symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    pth = os.path.join(tmp, "pthread.apk")
    make_apk(pth, make_elf_with_symbol(b"pthread_mutex_lock"))
    run(EXE, "import-apk", winpath(pth), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a pthread symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    fmtsym = os.path.join(tmp, "snprintf.apk")
    make_apk(fmtsym, make_elf_with_symbol(b"snprintf"))
    run(EXE, "import-apk", winpath(fmtsym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a variadic symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    iosym = os.path.join(tmp, "fopen.apk")
    make_apk(iosym, make_elf_with_symbol(b"fopen"))
    run(EXE, "import-apk", winpath(iosym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a stdio symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    scsym = os.path.join(tmp, "sscanf.apk")
    make_apk(scsym, make_elf_with_symbol(b"sscanf"))
    run(EXE, "import-apk", winpath(scsym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a scanf symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    ossym = os.path.join(tmp, "clock_gettime.apk")
    make_apk(ossym, make_elf_with_symbol(b"clock_gettime"))
    run(EXE, "import-apk", winpath(ossym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves an os/time symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    mmsym = os.path.join(tmp, "mmap.apk")
    make_apk(mmsym, make_elf_with_symbol(b"mmap"))
    run(EXE, "import-apk", winpath(mmsym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a memory (mmap) symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    fsym = os.path.join(tmp, "powf.apk")
    make_apk(fsym, make_elf_with_symbol(b"powf"))
    run(EXE, "import-apk", winpath(fsym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a libm float symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    cksym = os.path.join(tmp, "memcpy_chk.apk")
    make_apk(cksym, make_elf_with_symbol(b"__memcpy_chk"))
    run(EXE, "import-apk", winpath(cksym), data_dir=data)
    r = run(EXE, "run", data_dir=data)
    check("run resolves a fortified (_chk) symbol through the shim",
          "1 resolved by the shim" in r.stdout, r.stdout + r.stderr)

    # The SysV-ABI formatter, the stdio/file round-trip, the scanf parser, the
    # time/memory/process services, and the libc completion (math, fortified
    # _chk, UTF-8, runtime glue), each exercised the way the engine calls them.
    r = run(EXE, "selftest", data_dir=data)
    check("formatter, stdio, scanf, os and libc self-tests pass",
          r.returncode == 0 and "self-test total:" in r.stdout and "0 failure(s)" in r.stdout
          and "1 failure" not in r.stdout, str(r.returncode) + r.stdout + r.stderr)

    bad = os.path.join(tmp, "notelf.apk")
    make_apk(bad, b"\x7fELF" + b"\xff" * 2000)  # ELF magic, but not a usable ELF
    run(EXE, "import-apk", winpath(bad), data_dir=data)
    r = run(EXE, "run", "--inspect", data_dir=data)
    check("run --inspect rejects a non-x86-64 library",
          r.returncode != 0 and "x86-64" in r.stderr, r.stderr)


def build(out, pubkey, *extra):
    subprocess.run([os.environ.get("MAKE", "make"), "-s", "OUT=" + out, "UPDATE_PUBKEY=" + pubkey] + list(extra),
                   cwd=ROOT, check=True, capture_output=True)
    return out


def build_keyed(tmp):
    """Builds Quartix.exe with a throwaway signing key; returns (exe, private key file)."""
    key = os.path.join(tmp, "test.pem")
    pub = subprocess.run([sys.executable, SIGN, "keygen", key], capture_output=True, text=True, check=True).stdout.strip()
    return build(os.path.join(tmp, "keyed", "Quartix.exe"), pub), key


def sign(key, exe, version):
    subprocess.run([sys.executable, SIGN, "sign", key, exe, version], check=True, capture_output=True)
    return exe + ".sig"


def test_update(tmp):
    data = os.path.join(tmp, "data")
    login = os.path.join(data, "app", "com.roblox.client", "shared_prefs", "session.xml")
    os.makedirs(os.path.dirname(login))
    open(login, "w").write("<session>keep-me</session>")

    unkeyed = build(os.path.join(tmp, "unkeyed", "Quartix.exe"), "")
    r = run(unkeyed, "info", data_dir=data)
    check("unkeyed build reports updates disabled", "disabled" in r.stdout, r.stdout)
    staged = os.path.join(tmp, "unkeyed-new.exe")
    shutil.copy(unkeyed, staged)
    open(staged + ".sig", "w").write("quartix-update-v1\n")
    r = run(unkeyed, "apply-update", winpath(staged), winpath(staged + ".sig"), data_dir=data)
    check("unkeyed build refuses updates", r.returncode != 0 and "no update signing key" in r.stderr, r.stderr)

    keyed, key = build_keyed(tmp)
    install = os.path.join(tmp, "install")
    os.makedirs(install)
    exe = os.path.join(install, "Quartix.exe")
    shutil.copy(keyed, exe)
    before = sha256(exe)

    staged = os.path.join(tmp, "new", "Quartix.exe")
    os.makedirs(os.path.dirname(staged))
    shutil.copy(keyed, staged)
    with open(staged, "ab") as f:
        f.write(b"\0" * 512)  # a different, still valid image

    def apply(sig):
        return run(exe, "apply-update", winpath(staged), winpath(sig), data_dir=data)

    other_key = os.path.join(tmp, "other.pem")
    subprocess.run([sys.executable, SIGN, "keygen", other_key], check=True, capture_output=True)
    r = apply(sign(other_key, staged, "9.0.0"))
    check("update signed by another key is refused", r.returncode != 0 and "not valid" in r.stderr, r.stderr)

    sig = sign(key, staged, "9.0.0")
    body = open(sig).read()
    open(sig, "w").write(body.replace("version=9.0.0", "version=9.0.1"))
    r = apply(sig)
    check("tampered signature file is refused", r.returncode != 0 and "not valid" in r.stderr, r.stderr)

    r = apply(sign(key, staged, "0.1.0"))
    check("older version is refused", r.returncode != 0 and "not newer" in r.stderr, r.stderr)

    good = sign(key, staged, "9.0.0")
    with open(staged, "ab") as f:
        f.write(b"x")
    r = apply(good)
    check("file changed after signing is refused", r.returncode != 0 and "does not match" in r.stderr, r.stderr)
    check("refused updates leave the executable alone", sha256(exe) == before)

    good = sign(key, staged, "9.0.0")
    want = sha256(staged)
    r = apply(good)
    check("signed, newer update applies", r.returncode == 0, r.stdout + r.stderr)
    check("executable was replaced", sha256(exe) == want)
    check("login data survived the update", open(login).read() == "<session>keep-me</session>")

    r = run(exe, "info", data_dir=data)
    check("updated build runs", r.returncode == 0 and winpath(data).lower() in r.stdout.lower(), r.stdout + r.stderr)
    check("old executable cleaned up on next start", not os.path.exists(exe + ".old"))
    check("login data survived the restart", open(login).read() == "<session>keep-me</session>")


def make_tls_material(tmp):
    """Writes a test CA, a server cert for localhost signed by it, and a second,
    untrusted CA's localhost cert. Returns their paths."""
    import datetime
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    now = datetime.datetime.now(datetime.timezone.utc)

    def name(cn):
        return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])

    def make_ca(cn):
        key = ec.generate_private_key(ec.SECP256R1())
        cert = (x509.CertificateBuilder().subject_name(name(cn)).issuer_name(name(cn))
                .public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=30))
                .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
                .sign(key, hashes.SHA256()))
        return key, cert

    def make_leaf(ca_key, ca_cert, host, prefix):
        key = ec.generate_private_key(ec.SECP256R1())
        cert = (x509.CertificateBuilder().subject_name(name(host)).issuer_name(ca_cert.subject)
                .public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=30))
                .add_extension(x509.SubjectAlternativeName([x509.DNSName(host)]), critical=False)
                .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
                .sign(ca_key, hashes.SHA256()))
        cert_path, key_path = os.path.join(tmp, prefix + ".crt"), os.path.join(tmp, prefix + ".key")
        open(cert_path, "wb").write(cert.public_bytes(serialization.Encoding.PEM) +
                                    ca_cert.public_bytes(serialization.Encoding.PEM))
        open(key_path, "wb").write(key.private_bytes(serialization.Encoding.PEM,
                                                     serialization.PrivateFormat.PKCS8,
                                                     serialization.NoEncryption()))
        return cert_path, key_path

    ca_key, ca_cert = make_ca("Quartix Test CA")
    ca_path = os.path.join(tmp, "ca.pem")
    open(ca_path, "wb").write(ca_cert.public_bytes(serialization.Encoding.PEM))
    rogue_key, rogue_cert = make_ca("Untrusted CA")
    return {
        "ca": ca_path,
        "good": make_leaf(ca_key, ca_cert, "localhost", "good"),
        "wrong_host": make_leaf(ca_key, ca_cert, "example.invalid", "wrong"),
        "untrusted": make_leaf(rogue_key, rogue_cert, "localhost", "rogue"),
    }


class Server:
    """Local HTTPS server mimicking GitHub's latest-release download redirects."""

    def __init__(self, tls):
        import http.server
        import socket
        import ssl
        import threading

        self.files = {}
        self.hits = {}
        self.chunked = True
        files = self.files
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path.startswith("/latest/download/"):
                    name = self.path.rsplit("/", 1)[1]
                    self.send_response(302)
                    self.send_header("Location", "/assets/" + name)  # relative, like a CDN hop
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                name = self.path.rsplit("/", 1)[1]
                if not self.path.startswith("/assets/") or name not in files:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                body = files[name]
                server.hits[name] = server.hits.get(name, 0) + 1
                self.send_response(200)
                if server.chunked and name.endswith(".exe"):
                    self.send_header("Transfer-Encoding", "chunked")
                    self.end_headers()
                    for i in range(0, len(body), 7000):
                        part = body[i:i + 7000]
                        self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        self.ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        self.use(tls["good"])
        self.httpd.socket = self.ctx.wrap_socket(self.httpd.socket, server_side=True)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def use(self, cert_key):
        self.ctx.load_cert_chain(*cert_key)

    def stop(self):
        self.httpd.shutdown()


def test_online(tmp):
    tls = make_tls_material(tmp)
    srv = Server(tls)
    try:
        key = os.path.join(tmp, "k.pem")
        pub = subprocess.run([sys.executable, SIGN, "keygen", key], capture_output=True, text=True, check=True).stdout.strip()
        base = "UPDATE_BASE=https://localhost:%d/latest/download/" % srv.port
        exe_src = build(os.path.join(tmp, "b", "Quartix.exe"), pub, base, "TESTING=1")
        install = os.path.join(tmp, "install")
        os.makedirs(install)
        exe = os.path.join(install, "Quartix.exe")
        shutil.copy(exe_src, exe)
        before = sha256(exe)
        data = os.path.join(tmp, "data")
        login = os.path.join(data, "app", "com.roblox.client", "shared_prefs", "session.xml")
        os.makedirs(os.path.dirname(login))
        open(login, "w").write("<session>keep-me</session>")
        env = {"QUARTIX_TEST_CA": winpath(tls["ca"])}

        def update(*args, e=env):
            return run(exe, "update", *args, data_dir=data, env=e)

        r = update()
        check("no published release is reported, not an error", r.returncode == 0 and "no release" in r.stdout, r.stdout + r.stderr)

        new = os.path.join(tmp, "new", "Quartix.exe")
        os.makedirs(os.path.dirname(new))
        shutil.copy(exe_src, new)
        with open(new, "ab") as f:
            f.write(os.urandom(300000))  # different image, big enough for many chunks

        def publish(version, body=None, signer=key):
            sig = sign(signer, new, version)
            srv.files["Quartix.exe.sig"] = open(sig, "rb").read()
            srv.files["Quartix.exe"] = body if body is not None else open(new, "rb").read()

        publish("0.3.0")
        r = update()
        check("same version reports up to date", r.returncode == 0 and "up to date" in r.stdout, r.stdout + r.stderr)

        publish("9.0.0")
        r = update("--check")
        check("--check reports the newer version", r.returncode == 0 and "9.0.0" in r.stdout, r.stdout + r.stderr)
        check("--check does not install", sha256(exe) == before)

        r = update(e={})
        check("server outside the trusted CAs is refused", r.returncode != 0 and "certificate" in r.stderr, r.stderr)

        srv.use(tls["untrusted"])
        r = update()
        check("certificate from an untrusted CA is refused", r.returncode != 0 and "certificate" in r.stderr, r.stderr)
        srv.use(tls["wrong_host"])
        r = update()
        check("certificate for another host name is refused", r.returncode != 0 and "certificate" in r.stderr, r.stderr)
        srv.use(tls["good"])

        other = os.path.join(tmp, "other.pem")
        subprocess.run([sys.executable, SIGN, "keygen", other], check=True, capture_output=True)
        publish("9.0.0", signer=other)
        r = update()
        check("release signed by another key is refused", r.returncode != 0 and "not valid" in r.stderr, r.stderr)

        publish("9.0.0", body=open(new, "rb").read() + b"tampered")
        r = update()
        check("tampered download is refused", r.returncode != 0 and "does not match" in r.stderr, r.stderr)
        check("refused updates leave the executable alone", sha256(exe) == before)
        check("refused download is cleaned up", not os.path.exists(exe + ".download"))

        publish("9.0.0")
        r = update()
        check("signed release downloads (redirect, chunked) and installs", r.returncode == 0, r.stdout + r.stderr)
        check("executable now matches the release", sha256(exe) == sha256(new))
        check("login data survived the online update", open(login).read() == "<session>keep-me</session>")

        srv.chunked = False
        shutil.copy(exe_src, exe)
        r = update()
        check("Content-Length download also installs", r.returncode == 0 and sha256(exe) == sha256(new), r.stdout + r.stderr)
    finally:
        srv.stop()


def test_gui(tmp):
    """Starts the window (needs a display, e.g. xvfb-run) and checks that it installs
    an available update on its own, restarts, and does not install again."""
    import time
    if os.name != "nt" and not os.environ.get("DISPLAY"):
        print("skip window tests: no DISPLAY (run under xvfb-run)")
        return
    tls = make_tls_material(tmp)
    srv = Server(tls)
    wineserver = os.path.join(os.path.dirname(WINE), "wineserver")
    try:
        key = os.path.join(tmp, "k.pem")
        pub = subprocess.run([sys.executable, SIGN, "keygen", key], capture_output=True, text=True, check=True).stdout.strip()
        exe_src = build(os.path.join(tmp, "b", "Quartix.exe"), pub,
                        "UPDATE_BASE=https://localhost:%d/latest/download/" % srv.port, "TESTING=1")
        install = os.path.join(tmp, "install")
        os.makedirs(install)
        exe = os.path.join(install, "Quartix.exe")
        shutil.copy(exe_src, exe)
        data = os.path.join(tmp, "data")
        login = os.path.join(data, "app", "com.roblox.client", "shared_prefs", "session.xml")
        os.makedirs(os.path.dirname(login))
        open(login, "w").write("<session>keep-me</session>")

        new = os.path.join(tmp, "new", "Quartix.exe")
        os.makedirs(os.path.dirname(new))
        shutil.copy(exe_src, new)
        with open(new, "ab") as f:
            f.write(b"\0" * 4096)
        # Signed as 9.0.0 while the binary itself still says the current version: the restarted
        # window must not install it again.
        sig = sign(key, new, "9.0.0")
        srv.files["Quartix.exe.sig"] = open(sig, "rb").read()
        srv.files["Quartix.exe"] = open(new, "rb").read()

        env = dict(os.environ, QUARTIX_DATA_DIR=winpath(data), QUARTIX_TEST_CA=winpath(tls["ca"]), WINEDEBUG="-all")
        cmd = ([] if os.name == "nt" else [WINE]) + [exe]
        proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 60
        while time.time() < deadline and sha256(exe) != sha256(new):
            time.sleep(0.5)
        check("window installs an available update on start", sha256(exe) == sha256(new))
        deadline = time.time() + 30
        while time.time() < deadline and srv.hits.get("Quartix.exe.sig", 0) < 2:
            time.sleep(0.5)
        time.sleep(5)
        check("restarted window checks again", srv.hits.get("Quartix.exe.sig", 0) >= 2, str(srv.hits))
        check("restarted window does not install again", srv.hits.get("Quartix.exe", 0) == 1, str(srv.hits))
        check("login data survived the automatic update", open(login).read() == "<session>keep-me</session>")
        proc.kill()
    finally:
        if os.path.exists(wineserver):
            subprocess.run([wineserver, "-k"], capture_output=True)
        srv.stop()


def main():
    for t in (test_apk, test_runtime, test_update, test_online, test_gui):
        tmp = tempfile.mkdtemp(prefix="quartix-")
        try:
            t(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
