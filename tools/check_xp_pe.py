#!/usr/bin/env python3
"""Check that a PE image is built to load on Windows XP x64 (NT 5.2).

Verifies the machine type, the OS/subsystem version fields, that every imported
DLL ships with XP x64, and that no import is a known Vista-or-later API. The API
list is a guard against common slips, not a full XP export table; running the
build on real XP x64 remains the final check.
"""
import struct
import sys

XP_DLLS = {
    "kernel32.dll", "msvcrt.dll", "shell32.dll", "user32.dll", "gdi32.dll",
    "advapi32.dll", "opengl32.dll", "ws2_32.dll", "winmm.dll", "dsound.dll",
    "crypt32.dll", "ole32.dll", "shlwapi.dll", "comctl32.dll", "ntdll.dll",
    "comdlg32.dll",
}

VISTA_PLUS = {
    "GetTickCount64", "InitializeSRWLock", "AcquireSRWLockExclusive",
    "AcquireSRWLockShared", "ReleaseSRWLockExclusive", "ReleaseSRWLockShared",
    "TryAcquireSRWLockExclusive", "InitializeConditionVariable",
    "SleepConditionVariableCS", "SleepConditionVariableSRW",
    "WakeConditionVariable", "WakeAllConditionVariable", "InitOnceExecuteOnce",
    "InitOnceBeginInitialize", "InitializeCriticalSectionEx",
    "CreateSymbolicLinkW", "GetFinalPathNameByHandleW", "CompareStringEx",
    "LCIDToLocaleName", "LocaleNameToLCID", "GetLocaleInfoEx",
    "GetUserDefaultLocaleName", "CreateEventExW", "CreateMutexExW",
    "CreateSemaphoreExW", "SetFileInformationByHandle",
    "GetFileInformationByHandleEx", "CancelIoEx", "QueryFullProcessImageNameW",
    "GetSystemTimePreciseAsFileTime", "GetErrorMode", "CopyFile2",
    "CreateFile2", "GetNamedPipeClientProcessId", "SetThreadErrorMode",
    "Wow64GetThreadContext", "RtlCaptureStackBackTrace_Win7",
    "inet_ntop", "inet_pton", "WSAPoll", "if_nametoindex",
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
    if (os_maj, os_min) > (5, 2) or (ss_maj, ss_min) > (5, 2):
        fail("image requires NT %d.%d / subsystem %d.%d; XP x64 is 5.2" % (os_maj, os_min, ss_maj, ss_min))

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
        if dll.lower() not in XP_DLLS:
            bad.append("imports %s, which XP x64 does not ship" % dll)
        t = off(ilt or iat)
        while True:
            entry = struct.unpack_from("<Q", data, t)[0]
            if entry == 0:
                break
            if not entry >> 63:
                fn = cstr(off(entry & 0x7FFFFFFF) + 2)
                imports += 1
                if fn in VISTA_PLUS:
                    bad.append("%s!%s is Vista or later" % (dll, fn))
            t += 8
        d += 20
    if bad:
        fail("; ".join(bad))
    print("OK: %s targets NT 5.2, %d imports checked" % (path, imports))


if __name__ == "__main__":
    main(sys.argv[1])
