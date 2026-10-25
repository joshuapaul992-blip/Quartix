/*
 * Quartix - map the Android ELF into memory and apply the relocations that need
 * no symbols. Copyright (C) 2026 Quartix contributors. GPLv3 (see LICENSE).
 *
 * This is phase 2 of the runtime (see docs/PLAN.md): reserve one span for the
 * library, copy each PT_LOAD segment into place at a chosen load bias, apply the
 * relative relocations (R_X86_64_RELATIVE, which only need the bias), and give
 * every segment its final page protection. Symbolic relocations (GLOB_DAT,
 * JUMP_SLOT, and 64-bit symbol relocations) are counted but left for the
 * Linux-syscall + bionic shim that resolves imports - the phase after this one.
 * Nothing here transfers control to the mapped code; that too waits for the shim.
 *
 * The file comes from a user-supplied APK, so every offset, size and relocation
 * target is validated against the reserved span before it is used.
 */
#include "quartix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define PHNUM_CAP 128
#define DYN_CAP 8192
#define SPAN_CAP (1024ull * 1024 * 1024) /* refuse absurd images (1 GiB) */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

struct phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
};

static uint64_t page_down(uint64_t v) { return v & ~(uint64_t)0xfff; }
static uint64_t page_up(uint64_t v) { return (v + 0xfff) & ~(uint64_t)0xfff; }

/* Walk DT_RELA and DT_JMPREL tables and apply R_X86_64_RELATIVE; tally the rest. */
static void apply_relocs(qx_elf_image *img, uint64_t lo, const uint8_t *dyn, uint64_t dyn_n)
{
    uint64_t rela_va = 0, rela_sz = 0, jmprel_va = 0, pltrel_sz = 0, i, t;

    for (i = 0; i < dyn_n; i++) {
        uint64_t tag = rd64(dyn + i * 16), val = rd64(dyn + i * 16 + 8);
        if (tag == DT_NULL)
            break;
        else if (tag == DT_RELA) rela_va = val;
        else if (tag == DT_RELASZ) rela_sz = val;
        else if (tag == DT_JMPREL) jmprel_va = val;
        else if (tag == DT_PLTRELSZ) pltrel_sz = val;
    }

    for (t = 0; t < 2; t++) {
        uint64_t va = t == 0 ? rela_va : jmprel_va;
        uint64_t sz = t == 0 ? rela_sz : pltrel_sz;
        uint64_t off, k, n;
        if (!va || !sz)
            continue;
        off = va - lo; /* the tables live inside a mapped PT_LOAD segment */
        if (off >= img->span || sz > img->span - off)
            continue;
        n = sz / 24; /* sizeof(Elf64_Rela) */
        for (k = 0; k < n; k++) {
            const uint8_t *r = img->base + off + k * 24;
            uint64_t r_offset = rd64(r), r_info = rd64(r + 8);
            int64_t r_addend = (int64_t)rd64(r + 16);
            uint32_t type = (uint32_t)(r_info & 0xffffffff);
            uint64_t target = r_offset - lo;
            if (target >= img->span || img->span - target < 8)
                continue;
            if (type == R_X86_64_RELATIVE) {
                uint64_t v = img->bias + (uint64_t)r_addend;
                memcpy(img->base + target, &v, 8);
                img->relative_relocs++;
            } else if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT || type == R_X86_64_64) {
                img->symbolic_relocs++; /* needs symbol resolution - the shim's job */
            } else {
                img->other_relocs++;
            }
        }
    }
}

int qx_elf_load(const wchar_t *path, qx_elf_image *out)
{
    FILE *f;
    uint8_t eh[64];
    struct phdr ph[PHNUM_CAP];
    uint64_t phoff, lo = (uint64_t)~0ull, hi = 0, dyn_off = 0, dyn_filesz = 0;
    unsigned phnum, phentsize, i, nph = 0, loads = 0;
    uint8_t *dyn = NULL;

    memset(out, 0, sizeof(*out));
    f = _wfopen(path, L"rb");
    if (!f) {
        qx_err("cannot open the native library");
        return -1;
    }
    if (fread(eh, 1, sizeof(eh), f) != sizeof(eh) || memcmp(eh, "\x7f""ELF", 4) != 0 ||
        eh[4] != 2 || eh[5] != 1 || rd16(eh + 18) != EM_X86_64) {
        qx_err("not a 64-bit x86-64 ELF");
        goto fail;
    }
    phoff = rd64(eh + 32);
    phentsize = rd16(eh + 54);
    phnum = rd16(eh + 56);
    if (phentsize != 56 || phnum == 0 || phnum > PHNUM_CAP || fseek(f, (long)phoff, SEEK_SET) != 0) {
        qx_err("unsupported program header layout");
        goto fail;
    }
    for (i = 0; i < phnum; i++) {
        uint8_t pe[56];
        if (fread(pe, 1, sizeof(pe), f) != sizeof(pe)) {
            qx_err("truncated program headers");
            goto fail;
        }
        ph[nph].type = rd32(pe);
        ph[nph].flags = rd32(pe + 4);
        ph[nph].offset = rd64(pe + 8);
        ph[nph].vaddr = rd64(pe + 16);
        ph[nph].filesz = rd64(pe + 32);
        ph[nph].memsz = rd64(pe + 40);
        if (ph[nph].type == PT_LOAD) {
            loads++;
            if (ph[nph].vaddr < lo)
                lo = page_down(ph[nph].vaddr);
            if (ph[nph].vaddr + ph[nph].memsz > hi)
                hi = ph[nph].vaddr + ph[nph].memsz;
        } else if (ph[nph].type == PT_DYNAMIC) {
            dyn_off = ph[nph].offset;
            dyn_filesz = ph[nph].filesz;
        }
        nph++;
    }
    if (loads == 0 || hi <= lo) {
        qx_err("no loadable segments");
        goto fail;
    }
    out->span = page_up(hi) - lo;
    if (out->span > SPAN_CAP) {
        qx_err("library is too large to map (%llu bytes)", (unsigned long long)out->span);
        goto fail;
    }

    out->base = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)out->span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!out->base) {
        qx_err("cannot reserve %llu bytes for the library", (unsigned long long)out->span);
        goto fail;
    }
    out->bias = (uint64_t)(uintptr_t)out->base - lo;
    out->entry = out->bias + rd64(eh + 24);

    /* Copy each PT_LOAD segment to base + (vaddr - lo); the tail to memsz is
     * already zero because VirtualAlloc zero-fills committed pages. */
    for (i = 0; i < nph; i++) {
        uint64_t dest;
        if (ph[i].type != PT_LOAD || ph[i].filesz == 0)
            continue;
        dest = ph[i].vaddr - lo;
        if (dest >= out->span || ph[i].filesz > out->span - dest) {
            qx_err("segment %u does not fit the reserved span", i);
            goto fail_mapped;
        }
        if (fseek(f, (long)ph[i].offset, SEEK_SET) != 0 ||
            fread(out->base + dest, 1, (size_t)ph[i].filesz, f) != ph[i].filesz) {
            qx_err("cannot read segment %u from the library", i);
            goto fail_mapped;
        }
    }

    /* Relocations run while everything is still writable. */
    if (dyn_off && dyn_filesz && dyn_filesz <= DYN_CAP * 16) {
        dyn = (uint8_t *)malloc((size_t)dyn_filesz);
        if (dyn && fseek(f, (long)dyn_off, SEEK_SET) == 0 &&
            fread(dyn, 1, (size_t)dyn_filesz, f) == dyn_filesz)
            apply_relocs(out, lo, dyn, dyn_filesz / 16);
        free(dyn);
    }

    /* Give each segment its final protection now that it is written and relocated. */
    for (i = 0; i < nph; i++) {
        DWORD prot, old;
        uint64_t start, end;
        if (ph[i].type != PT_LOAD)
            continue;
        start = page_down(ph[i].vaddr) - lo;
        end = page_up(ph[i].vaddr + ph[i].memsz) - lo;
        if (end > out->span)
            end = out->span;
        if (ph[i].flags & PF_X)
            prot = (ph[i].flags & PF_W) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        else
            prot = (ph[i].flags & PF_W) ? PAGE_READWRITE : PAGE_READONLY;
        VirtualProtect(out->base + start, (SIZE_T)(end - start), prot, &old);
    }

    out->load_segments = loads;
    fclose(f);
    return 0;

fail_mapped:
    VirtualFree(out->base, 0, MEM_RELEASE);
    out->base = NULL;
fail:
    fclose(f);
    return -1;
}

void qx_elf_unload(qx_elf_image *img)
{
    if (img && img->base) {
        VirtualFree(img->base, 0, MEM_RELEASE);
        img->base = NULL;
    }
}
