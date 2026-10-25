#!/usr/bin/env python3
"""Check that a PE image is built to load on Windows Vista x64 (NT 6.0).

NT 6.0 is Quartix's floor: a build that loads on Vista also loads on 7 (6.1) and
8.1 (6.3). This verifies the machine type, the OS/subsystem version fields, that
every imported DLL ships with Vista x64, and that no import is a known API that
arrived after Vista (Windows 7, 8/8.1 or 10+). The API list is a guard against
common slips, not a full Vista export table; running on real hardware across the
Vista/7/8.1 range remains the final check.
"""
import struct
import sys

# DLLs that ship with Windows Vista x64 (a superset of the XP x64 set).
WIN_DLLS = {
    "kernel32.dll", "msvcrt.dll", "shell32.dll", "user32.dll", "gdi32.dll",
    "advapi32.dll", "opengl32.dll", "ws2_32.dll", "winmm.dll", "dsound.dll",
    "crypt32.dll", "ole32.dll", "shlwapi.dll", "comctl32.dll", "ntdll.dll",
    "comdlg32.dll", "iphlpapi.dll", "version.dll", "setupapi.dll",
}

# APIs introduced after the NT 6.0 floor, so unavailable on Vista (and often on
# 7). Importing any of these would break the floor; keep them out until a runtime
# phase genuinely needs one and its minimum OS has been confirmed.
NEWER_THAN_VISTA = {
    "GetSystemTimePreciseAsFileTime",  # Windows 8
    "CopyFile2", "CreateFile2",        # Windows 8
    "SetThreadErrorMode",              # Windows 7
    "GetErrorMode",                    # Windows 7
    "RtlCaptureStackBackTrace_Win7",   # guard entry
    "PathCchCanonicalizeEx", "PathCchCombineEx",  # Windows 8
    "GetFileVersionInfoExW",           # Windows 7
    "CreateFile2FromAppW",             # Windows 10 / app APIs
}


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def main(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        fail("not a PE image")
    machine, nsec, _, _, _, optsize, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    if machine != 0x8664:
        fail("machine is 0x%x, expected AMD64" % machine)
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        fail("not a PE32+ image")
    os_maj, os_min = struct.unpack_from("<HH", data, opt + 40)
    ss_maj, ss_min = struct.unpack_from("<HH", data, opt + 48)
    if (os_maj, os_min) > (6, 0) or (ss_maj, ss_min) > (6, 0):
        fail("image requires NT %d.%d / subsystem %d.%d; the Vista x64 floor is 6.0" % (os_maj, os_min, ss_maj, ss_min))

    sections = []
    sec = opt + optsize
    for i in range(nsec):
        _, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", data, sec + 40 * i)
        sections.append((va, max(vsize, rawsize), rawptr))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        fail("RVA 0x%x outside sections" % rva)

    def cstr(o):
        return data[o:data.index(b"\0", o)].decode("ascii")

    imp_rva = struct.unpack_from("<I", data, opt + 112 + 8)[0]
    bad = []
    imports = 0
    d = off(imp_rva)
    while True:
        ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", data, d)
        if name_rva == 0:
            break
        dll = cstr(off(name_rva))
        if dll.lower() not in WIN_DLLS:
            bad.append("imports %s, which Vista x64 does not ship" % dll)
        t = off(ilt or iat)
        while True:
            entry = struct.unpack_from("<Q", data, t)[0]
            if entry == 0:
                break
            if not entry >> 63:
                fn = cstr(off(entry & 0x7FFFFFFF) + 2)
                imports += 1
                if fn in NEWER_THAN_VISTA:
                    bad.append("%s!%s arrived after Vista (NT 6.0)" % (dll, fn))
            t += 8
        d += 20
    if bad:
        fail("; ".join(bad))
    print("OK: %s targets NT 6.0 (Vista/7/8.1), %d imports checked" % (path, imports))


if __name__ == "__main__":
    main(sys.argv[1])
