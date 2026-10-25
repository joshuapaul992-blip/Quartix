/*
 * Quartix - resolver for the Android/libc symbols libroblox.so imports.
 * Copyright (C) 2026 Quartix contributors. GPLv3 (see LICENSE).
 *
 * libroblox.so needs ~585 symbols from the Android system libraries (libc, libm,
 * libdl, liblog, libandroid, libEGL, libGLESv2, libOpenSLES, libOpenMAXAL,
 * libmediandk). Those are the next phases. Two things matter up front:
 *
 *   - The Android code uses the System V AMD64 calling convention, not the
 *     Microsoft x64 one, so every shim function it calls must be declared
 *     __attribute__((sysv_abi)). The trap below is, which is why binding an
 *     unresolved import to it is safe even if the engine calls it.
 *   - Until an implementation exists, qx_shim_resolve returns NULL and the
 *     loader binds the import to the trap, which logs the name and exits rather
 *     than jumping into nothing.
 */
#include "quartix.h"
#include <stdlib.h>

/* SysV-ABI so it is call-compatible with the Android code that might reach it. */
static __attribute__((sysv_abi)) void qx_shim_trap(void)
{
    qx_err("libroblox.so called an Android symbol Quartix does not implement yet");
    ExitProcess(3);
}

void *qx_shim_trap_addr(void)
{
    return (void *)qx_shim_trap;
}

void *qx_shim_resolve(const char *name)
{
    (void)name; /* no implementations yet; later phases fill this in */
    return NULL;
}
