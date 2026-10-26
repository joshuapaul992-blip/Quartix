/*
 * Quartix - thread-local storage (%fs) emulation for the Android engine.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 *
 * bionic (the engine's C library) keeps its thread pointer and the stack-
 * protector canary in a TLS block reached through the %fs segment: %fs:0 is the
 * self-pointer (TLS_SLOT_SELF) and %fs:0x28 is the stack canary
 * (TLS_SLOT_STACK_GUARD, slot 5). libroblox.so's compiler emitted ~2,358
 * `mov reg, %fs:0x28` reads - one in nearly every function with a stack buffer.
 *
 * On Linux the kernel sets each thread's %fs base to its TLS block, so those
 * reads just work. Windows x64 leaves the %fs base at 0 and offers no way to set
 * it from user mode before Windows 10 (wrfsbase needs CR4.FSGSBASE; the LDT path
 * is 32-bit only). So on Vista/7/8.1 every %fs access faults.
 *
 * Rather than modify the engine's code (which would be a ban risk), Quartix
 * leaves %fs base at 0 and services the faults: a vectored exception handler
 * decodes the faulting %fs instruction, redirects its memory operand to a
 * per-thread block Quartix owns, emulates the operation, and resumes. The engine
 * runs completely untouched.
 *
 * The engine only uses the absolute `%fs:[disp32]` addressing form, and only a
 * few opcodes (mov/cmp/xor, verified by scanning the binary), so the decoder
 * handles exactly those. Under Wine the host Linux %fs is valid, so these never
 * fault and this handler is simply never invoked there.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define QX_TLS_SIZE 0x10000u          /* per-thread block; far larger than bionic needs */
#define QX_TLS_CANARY_OFF 0x28        /* TLS_SLOT_STACK_GUARD */

/* Any fixed non-zero value. It only has to be identical at a function's entry
 * and exit, which it is: every thread reads it from its own block. */
#define QX_TLS_CANARY ((uintptr_t)0x2b7e151628aed2a6ull)

/* x86 EFLAGS bits we maintain for the flag-setting forms. */
#define FL_CF 0x0001u
#define FL_PF 0x0004u
#define FL_AF 0x0010u
#define FL_ZF 0x0040u
#define FL_SF 0x0080u
#define FL_OF 0x0800u

static DWORD g_tls_slot = TLS_OUT_OF_INDEXES;
static PVOID g_veh;
static PVOID g_diag_veh;

/* The mapped engine image, so a fault can be located as image+offset. */
static const unsigned char *g_img_lo;
static const unsigned char *g_img_hi;

void qx_tls_set_image_range(const void *base, uint64_t span)
{
    g_img_lo = (const unsigned char *)base;
    g_img_hi = g_img_lo ? g_img_lo + span : NULL;
}

static int in_image(const void *p)
{
    return g_img_lo && (const unsigned char *)p >= g_img_lo && (const unsigned char *)p < g_img_hi;
}

static void *tls_block_for_current_thread(void)
{
    void *b;
    if (g_tls_slot == TLS_OUT_OF_INDEXES)
        return NULL;
    b = TlsGetValue(g_tls_slot);
    if (!b) {
        b = VirtualAlloc(NULL, QX_TLS_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (b) {
            /* The block is untyped storage, so write through memcpy to stay
             * clear of strict-aliasing assumptions. */
            void *self = b;
            uintptr_t canary = QX_TLS_CANARY;
            memcpy(b, &self, sizeof self);                              /* slot 0: self-pointer */
            memcpy((char *)b + QX_TLS_CANARY_OFF, &canary, sizeof canary); /* slot 5: canary */
            TlsSetValue(g_tls_slot, b);
        }
    }
    return b;
}

void *qx_tls_block(void) { return tls_block_for_current_thread(); }

/* Map a 0-15 register index to its CONTEXT field. */
static DWORD64 *ctx_reg(CONTEXT *c, int idx)
{
    switch (idx) {
    case 0:  return &c->Rax; case 1:  return &c->Rcx; case 2:  return &c->Rdx; case 3:  return &c->Rbx;
    case 4:  return &c->Rsp; case 5:  return &c->Rbp; case 6:  return &c->Rsi; case 7:  return &c->Rdi;
    case 8:  return &c->R8;  case 9:  return &c->R9;  case 10: return &c->R10; case 11: return &c->R11;
    case 12: return &c->R12; case 13: return &c->R13; case 14: return &c->R14; case 15: return &c->R15;
    }
    return NULL;
}

/* Set ZF/SF/PF from a result of the given width (w: 1 = 64-bit, 0 = 32-bit). */
static DWORD flags_zsp(DWORD f, uint64_t res, int w)
{
    uint64_t r = w ? res : (res & 0xffffffffu);
    unsigned char lo = (unsigned char)r;
    int bit, parity = 0;
    f &= ~(FL_ZF | FL_SF | FL_PF);
    if (r == 0)
        f |= FL_ZF;
    if (r >> (w ? 63 : 31))
        f |= FL_SF;
    for (bit = 0; bit < 8; bit++)
        parity ^= (lo >> bit) & 1;
    if (!parity)
        f |= FL_PF; /* PF set when the low byte has even parity */
    return f;
}

/*
 * Decode and emulate one %fs:[disp32] instruction at `p`, acting on the register
 * file and flags in `c`, redirecting the memory operand into `block`. Returns the
 * instruction's length in bytes, or 0 if `p` is not a form we handle (the caller
 * then lets the fault proceed). Does not touch c->Rip; the caller advances it.
 *
 * Factored out of the exception handler so the decode/emulation can be unit
 * tested deterministically - the handler path itself cannot run under Wine,
 * where %fs never faults.
 */
int qx_fs_emulate(const unsigned char *p, CONTEXT *c, void *block)
{
    int i = 1, w, reg_idx;
    int rex = 0;
    unsigned char opcode, modrm, sib;
    uint32_t disp;
    uint64_t memv;
    DWORD64 *reg;

    if (!p || !c || !block)
        return 0;
    if (p[0] != 0x64)                 /* FS segment override prefix */
        return 0;
    if (p[i] >= 0x40 && p[i] <= 0x4f) /* optional REX */
        rex = p[i++];
    opcode = p[i++];
    modrm = p[i++];
    if ((modrm & 0xc7) != 0x04)       /* mod=00, rm=100 (SIB follows) */
        return 0;
    sib = p[i++];
    if (sib != 0x25)                  /* scale=0, index=none, base=none -> disp32 */
        return 0;
    disp = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
           ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
    i += 4;
    if (disp >= QX_TLS_SIZE)          /* outside our block: not something we can serve */
        return 0;

    w = (rex & 0x08) ? 1 : 0;                       /* REX.W -> 64-bit operand */
    reg_idx = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0); /* REX.R extends reg */
    reg = ctx_reg(c, reg_idx);
    if (!reg)
        return 0;
    /* Read through memcpy: the block is untyped storage, so a typed deref would
     * invite strict-aliasing miscompiles. 32-bit form zero-extends. */
    memv = 0;
    memcpy(&memv, (const char *)block + disp, w ? 8 : 4);

    switch (opcode) {
    case 0x8b: /* mov reg, [ea] */
        *reg = memv;                                /* 32-bit form zero-extends */
        break;
    case 0x33: { /* xor reg, [ea] */
        uint64_t r = (*reg) ^ memv;
        *reg = w ? r : (r & 0xffffffffu);
        c->EFlags = flags_zsp(c->EFlags & ~(FL_CF | FL_OF | FL_AF), r, w);
        break;
    }
    case 0x3b: { /* cmp reg, [ea]  (reg - mem) */
        uint64_t a = w ? *reg : (*reg & 0xffffffffu);
        uint64_t r = a - memv;
        uint64_t sign = w ? 0x8000000000000000ull : 0x80000000ull;
        DWORD f = flags_zsp(c->EFlags & ~(FL_CF | FL_OF | FL_AF), r, w);
        if (w ? (a < memv) : ((uint32_t)a < (uint32_t)memv))
            f |= FL_CF;
        if (((a ^ memv) & (a ^ r)) & sign)
            f |= FL_OF;
        c->EFlags = f;
        break;
    }
    default:
        return 0; /* opcode we don't emulate: let the fault proceed */
    }
    return i; /* total instruction length */
}

static LONG CALLBACK qx_fs_veh(PEXCEPTION_POINTERS info)
{
    EXCEPTION_RECORD *er = info->ExceptionRecord;
    CONTEXT *c = info->ContextRecord;
    void *block;
    int len;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    block = tls_block_for_current_thread(); /* lazily set up engine-spawned threads too */
    if (!block)
        return EXCEPTION_CONTINUE_SEARCH;
    len = qx_fs_emulate((const unsigned char *)c->Rip, c, block);
    if (len == 0)
        return EXCEPTION_CONTINUE_SEARCH;   /* not a %fs access we handle */
    c->Rip += (DWORD64)len;                 /* step over the emulated instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/*
 * Last-chance crash diagnostics. The %fs handler above (installed first)
 * resolves the engine's TLS accesses; any access violation it declines is a
 * real fault. Rather than let it surface only as Windows' bare "stopped
 * working" dialog, log what is needed to locate it - the fault address, the
 * faulting instruction's address and its offset into the mapped engine image,
 * and the instruction bytes - then let the normal crash path proceed. This
 * turns a silent hardware crash into actionable output. It never resumes
 * execution and never swallows the fault, so it cannot mask or alter a genuine
 * engine SEH handler; it only reports. Capped so a handled, repeated fault
 * cannot flood the log.
 */
/* True if the whole [p, p+n) range is committed and readable (no re-fault). */
static int mem_readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char *end;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return 0;
    end = (const char *)mbi.BaseAddress + mbi.RegionSize;
    return (const char *)p + n <= end; /* conservative: must not span regions */
}

/* Basename of the module that contains `addr`, into `out` (wide). "?" if none. */
static void module_at(const void *addr, wchar_t *out, size_t cap)
{
    HMODULE h = NULL;
    wchar_t path[MAX_PATH];
    out[0] = L'?'; out[1] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &h) && h &&
        GetModuleFileNameW(h, path, MAX_PATH)) {
        wchar_t *slash = wcsrchr(path, L'\\');
        const wchar_t *base = slash ? slash + 1 : path;
        size_t i = 0;
        for (; base[i] && i + 1 < cap; i++)
            out[i] = base[i];
        out[i] = 0;
    }
}

static LONG CALLBACK qx_fs_diag_veh(PEXCEPTION_POINTERS info)
{
    static volatile LONG logged;
    EXCEPTION_RECORD *er = info->ExceptionRecord;
    CONTEXT *c = info->ContextRecord;
    const unsigned char *rip;
    uintptr_t fault;
    int acc, is_av;
    DWORD code = er->ExceptionCode;
    wchar_t mod[64];

    /* Report the fatal exception codes an engine fault can surface as. A stack
     * overflow (0xC00000FD) is the important addition here: deep C++ recursion
     * crashes with it, not an access violation, and Windows otherwise shows only
     * its bare "stopped working" dialog. The reserved stack cushion
     * (qx_diag_stack_cushion, set on every thread) leaves room for this handler
     * to run after the guard page is hit. */
    is_av = (code == (DWORD)EXCEPTION_ACCESS_VIOLATION);
    if (!is_av &&
        code != (DWORD)EXCEPTION_STACK_OVERFLOW &&
        code != (DWORD)EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != (DWORD)EXCEPTION_PRIV_INSTRUCTION &&
        code != (DWORD)EXCEPTION_IN_PAGE_ERROR &&
        code != (DWORD)EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        code != (DWORD)EXCEPTION_DATATYPE_MISALIGNMENT)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&logged) > 4)
        return EXCEPTION_CONTINUE_SEARCH; /* reported enough already */

    rip = (const unsigned char *)(uintptr_t)c->Rip;
    acc = er->NumberParameters >= 1 ? (int)er->ExceptionInformation[0] : -1;
    fault = er->NumberParameters >= 2 ? (uintptr_t)er->ExceptionInformation[1] : 0;

    if (is_av)
        qx_log("[fault] access violation (%s) at 0x%llx",
               acc == 1 ? "write" : acc == 8 ? "exec" : "read",
               (unsigned long long)fault);
    else
        qx_log("[fault] %s (0x%08lx)",
               code == (DWORD)EXCEPTION_STACK_OVERFLOW ? "stack overflow" :
               code == (DWORD)EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction" :
               code == (DWORD)EXCEPTION_PRIV_INSTRUCTION ? "privileged instruction" :
               code == (DWORD)EXCEPTION_IN_PAGE_ERROR ? "in-page error" :
               code == (DWORD)EXCEPTION_ARRAY_BOUNDS_EXCEEDED ? "array bounds exceeded" :
               code == (DWORD)EXCEPTION_DATATYPE_MISALIGNMENT ? "datatype misalignment" :
               "fatal exception",
               (unsigned long)code);
    module_at(rip, mod, 64);
    if (in_image(rip))
        qx_log("[fault]   RIP 0x%llx = image+0x%llx (%ls)",
               (unsigned long long)(uintptr_t)rip,
               (unsigned long long)(uintptr_t)(rip - g_img_lo), mod);
    else
        qx_log("[fault]   RIP 0x%llx in %ls (outside the engine image)",
               (unsigned long long)(uintptr_t)rip, mod);
    if (in_image((const void *)fault))
        qx_log("[fault]   fault addr = image+0x%llx",
               (unsigned long long)(fault - (uintptr_t)g_img_lo));

    /* The faulting instruction's bytes, wherever RIP is (it was fetched, so it
     * is executable/readable) - shows the exact write and its base register. */
    if (mem_readable(rip, 16)) {
        char hex[16 * 3 + 1];
        int k, n = 0;
        for (k = 0; k < 16; k++)
            n += sprintf(hex + n, "%02x ", rip[k]);
        qx_log("[fault]   bytes@RIP: %s", hex);
        if (rip[0] == 0x64)
            qx_log("[fault]   (an %%fs access the TLS handler did not service)");
    }

    /* Registers, so the near-null base pointer behind the fault is visible. */
    qx_log("[fault]   rax=%llx rcx=%llx rdx=%llx rbx=%llx rsi=%llx rdi=%llx",
           (unsigned long long)c->Rax, (unsigned long long)c->Rcx,
           (unsigned long long)c->Rdx, (unsigned long long)c->Rbx,
           (unsigned long long)c->Rsi, (unsigned long long)c->Rdi);
    qx_log("[fault]   r8=%llx r9=%llx r10=%llx r11=%llx rbp=%llx rsp=%llx",
           (unsigned long long)c->R8, (unsigned long long)c->R9,
           (unsigned long long)c->R10, (unsigned long long)c->R11,
           (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);

    /* Call trail: scan the stack for return addresses that land inside the
     * engine image - these are the engine call sites that led here, so we can
     * see which constructor/shim path triggered the bad call. */
    {
        const uint64_t *sp = (const uint64_t *)(uintptr_t)c->Rsp;
        int shown = 0, k;
        if (mem_readable(sp, 8)) {
            for (k = 0; k < 64 && shown < 8; k++) {
                const void *slot = (const char *)sp + (size_t)k * 8;
                uint64_t v;
                if (!mem_readable(slot, 8))
                    break;
                v = ((const uint64_t *)slot)[0];
                if (in_image((const void *)(uintptr_t)v)) {
                    qx_log("[fault]   stack[+%02x] -> image+0x%llx",
                           k * 8, (unsigned long long)(v - (uint64_t)(uintptr_t)g_img_lo));
                    shown++;
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH; /* report only; let the fault proceed */
}

/* Install the %fs handler and set up the calling thread's TLS block. Idempotent;
 * call once before transferring control to engine code. */
/* Reserve a stack cushion for the crash handler, so that when a thread exhausts
 * its stack (deep engine recursion), enough committed stack remains for
 * qx_fs_diag_veh to run and report instead of the process dying silently. Called
 * on the main thread (here) and on every engine thread (thread_trampoline). */
void qx_diag_stack_cushion(void)
{
    ULONG want = 0x20000; /* 128 KiB - ample for the diagnostic handler */
    SetThreadStackGuarantee(&want);
}

void qx_tls_init(void)
{
    if (g_tls_slot == TLS_OUT_OF_INDEXES)
        g_tls_slot = TlsAlloc();
    qx_diag_stack_cushion();
    tls_block_for_current_thread();
    if (!g_veh)
        g_veh = AddVectoredExceptionHandler(1, qx_fs_veh); /* 1 = first in the chain */
    if (!g_diag_veh)
        g_diag_veh = AddVectoredExceptionHandler(0, qx_fs_diag_veh); /* last: after %fs */
}

/* ---- self-test: exercises the decoder/emulator deterministically ---------- */
int qx_tls_selftest(int *total)
{
    int n = 0, fails = 0, len;
    int pf;
    CONTEXT c;
    unsigned char block[256];
#define TLSCHK(cond) do { n++; pf = (cond); if (!(pf)) { fails++; qx_log("  [tls] check %d FAILED", n); } } while (0)

    {
        uint64_t canary = 0x1122334455667788ull;
        memset(block, 0, sizeof block);
        memcpy(block + 0x28, &canary, sizeof canary);
    }

    /* mov rax, fs:[0x28] : 64 48 8B 04 25 28 00 00 00 -> rax = canary, len 9 */
    {
        static const unsigned char insn[] = { 0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00 };
        memset(&c, 0, sizeof c);
        len = qx_fs_emulate(insn, &c, block);
        TLSCHK(len == 9 && c.Rax == 0x1122334455667788ull);
    }
    /* mov r8, fs:[0x28] : 64 4C 8B 04 25 ... (REX.WR) -> r8 = canary */
    {
        static const unsigned char insn[] = { 0x64, 0x4c, 0x8b, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00 };
        memset(&c, 0, sizeof c);
        len = qx_fs_emulate(insn, &c, block);
        TLSCHK(len == 9 && c.R8 == 0x1122334455667788ull);
    }
    /* xor rax, fs:[0x28] with rax == canary -> rax 0, ZF set (the guard passes) */
    {
        static const unsigned char insn[] = { 0x64, 0x48, 0x33, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00 };
        memset(&c, 0, sizeof c);
        c.Rax = 0x1122334455667788ull;
        len = qx_fs_emulate(insn, &c, block);
        TLSCHK(len == 9 && c.Rax == 0 && (c.EFlags & FL_ZF));
    }
    /* xor rax, fs:[0x28] with rax != canary -> ZF clear (the guard would fail) */
    {
        static const unsigned char insn[] = { 0x64, 0x48, 0x33, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00 };
        memset(&c, 0, sizeof c);
        c.Rax = 0xdeadbeefdeadbeefull;
        len = qx_fs_emulate(insn, &c, block);
        TLSCHK(len == 9 && !(c.EFlags & FL_ZF));
    }
    /* cmp rax, fs:[0x28] with rax == canary -> ZF set */
    {
        static const unsigned char insn[] = { 0x64, 0x48, 0x3b, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00 };
        memset(&c, 0, sizeof c);
        c.Rax = 0x1122334455667788ull;
        len = qx_fs_emulate(insn, &c, block);
        TLSCHK(len == 9 && (c.EFlags & FL_ZF));
    }
    /* self-pointer: mov rax, fs:[0] -> block's slot 0 */
    {
        static const unsigned char insn[] = { 0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00 };
        uint64_t selfp = 0xabcdef0012345678ull;
        memcpy(block, &selfp, sizeof selfp);
        memset(&c, 0, sizeof c);
        len = qx_fs_emulate(insn, &c, block);
        TLSCHK(len == 9 && c.Rax == 0xabcdef0012345678ull);
    }
    /* a non-%fs instruction is rejected (length 0) */
    {
        static const unsigned char insn[] = { 0x48, 0x8b, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00 };
        memset(&c, 0, sizeof c);
        TLSCHK(qx_fs_emulate(insn, &c, block) == 0);
    }

    if (total)
        *total = n;
    return fails;
#undef TLSCHK
}
