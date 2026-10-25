/*
 * Quartix - map the Android ELF into memory and apply its relocations.
 * Copyright (C) 2026 Quartix contributors. GPLv3 (see LICENSE).
 *
 * Phase 2+ of the runtime (see docs/PLAN.md). Reserve one span for the library,
 * copy each PT_LOAD segment to a chosen load bias, apply the relocations, and
 * give every segment its final page protection.
 *
 * libroblox.so is a position-independent Android build, so:
 *   - its ~628k relative relocations are packed in Android's APS2 format
 *     (DT_ANDROID_RELA), not a plain RELA table, so we decode that here;
 *   - it is linked BIND_NOW, so all symbols are resolved at load. Symbolic
 *     relocations are bound through the shim resolver (qx_shim_resolve); any it
 *     does not implement yet are bound to a trap so the image is still complete.
 *
 * Nothing here transfers control to the mapped code - the SysV-ABI bridge and
 * the init-array call are the next phase. Every file offset, size and
 * relocation target is validated against the reserved span, since the library
 * comes from a user-supplied APK.
 */
#include "quartix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define DT_NULL 0
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRSZ 10
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define DT_ANDROID_RELA 0x60000011
#define DT_ANDROID_RELASZ 0x60000012
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define PHNUM_CAP 128
#define DYN_CAP 8192
#define SPAN_CAP (2048ull * 1024 * 1024) /* refuse absurd images (2 GiB) */

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

/* Everything the relocation pass needs, gathered from the dynamic section. All
 * addresses are already translated to pointers inside the mapped image. */
struct reloc_ctx {
    qx_elf_image *img;
    uint64_t lo;
    const uint8_t *symtab;   /* DT_SYMTAB, or NULL */
    uint64_t symtab_bytes;   /* room from symtab to end of span */
    const char *strtab;      /* DT_STRTAB, or NULL */
    uint64_t strsz;
};

static uint64_t page_down(uint64_t v) { return v & ~(uint64_t)0xfff; }
static uint64_t page_up(uint64_t v) { return (v + 0xfff) & ~(uint64_t)0xfff; }

/* Map a mapped-image virtual address to a pointer, or NULL if out of range for
 * `need` bytes. */
static uint8_t *at(struct reloc_ctx *c, uint64_t vaddr, uint64_t need)
{
    uint64_t off = vaddr - c->lo;
    if (vaddr < c->lo || off >= c->img->span || c->img->span - off < need)
        return NULL;
    return c->img->base + off;
}

/* Resolve the symbol a relocation names to an address to store. */
static void *resolve_symbol(struct reloc_ctx *c, uint32_t sym, int *resolved)
{
    uint64_t name_off;
    const char *name;
    void *addr;
    *resolved = 0;
    if (!c->symtab || !c->strtab || (uint64_t)sym * 24 + 24 > c->symtab_bytes)
        return qx_shim_trap_addr();
    name_off = rd32(c->symtab + (uint64_t)sym * 24); /* st_name */
    if (name_off >= c->strsz)
        return qx_shim_trap_addr();
    name = c->strtab + name_off;
    addr = qx_shim_resolve(name);
    if (addr) {
        *resolved = 1;
        return addr;
    }
    return qx_shim_trap_addr(); /* complete the image; traps if ever called */
}

/* Apply one relocation given its decoded fields. */
static void apply_one(struct reloc_ctx *c, uint64_t r_offset, uint64_t r_info, int64_t r_addend)
{
    uint32_t type = (uint32_t)(r_info & 0xffffffff);
    uint32_t sym = (uint32_t)(r_info >> 32);
    uint8_t *target = at(c, r_offset, 8);
    if (!target)
        return; /* out-of-range target: ignore rather than corrupt memory */
    if (type == R_X86_64_RELATIVE) {
        uint64_t v = c->img->bias + (uint64_t)r_addend;
        memcpy(target, &v, 8);
        c->img->relative_relocs++;
    } else if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT || type == R_X86_64_64) {
        int resolved;
        uint64_t v = (uint64_t)(uintptr_t)resolve_symbol(c, sym, &resolved);
        if (type == R_X86_64_64)
            v += (uint64_t)r_addend;
        memcpy(target, &v, 8);
        c->img->symbolic_relocs++;
        c->img->symbolic_resolved += resolved;
    } else {
        c->img->other_relocs++;
    }
}

/* Standard Elf64_Rela table (24 bytes each). */
static void apply_rela(struct reloc_ctx *c, uint64_t table_va, uint64_t size)
{
    uint8_t *p = size ? at(c, table_va, size) : NULL;
    uint64_t i, n = size / 24;
    if (!p)
        return;
    for (i = 0; i < n; i++)
        apply_one(c, rd64(p + i * 24), rd64(p + i * 24 + 8), (int64_t)rd64(p + i * 24 + 16));
}

/* Android APS2 packed relocations: a sleb128 stream that expands to Elf64_Rela
 * triples. The format matches bionic's packed_reloc_iterator. */
struct sleb {
    const uint8_t *p, *end;
    int ok;
};

static int64_t sleb_next(struct sleb *s)
{
    int64_t result = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        if (s->p >= s->end) {
            s->ok = 0;
            return 0;
        }
        b = *s->p++;
        result |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        result |= -((int64_t)1 << shift);
    return result;
}

#define APS2_GROUPED_BY_INFO 1
#define APS2_GROUPED_BY_OFFSET_DELTA 2
#define APS2_GROUPED_BY_ADDEND 4
#define APS2_HAS_ADDEND 8

static void apply_android_rela(struct reloc_ctx *c, uint64_t blob_va, uint64_t size)
{
    uint8_t *blob = size ? at(c, blob_va, size) : NULL;
    struct sleb s;
    int64_t count, done = 0, r_offset, r_addend = 0, group_addend = 0;

    if (!blob || size < 4 || memcmp(blob, "APS2", 4) != 0)
        return;
    s.p = blob + 4;
    s.end = blob + size;
    s.ok = 1;
    count = sleb_next(&s);
    r_offset = sleb_next(&s);
    while (s.ok && done < count) {
        int64_t group_size = sleb_next(&s);
        int64_t group_flags = sleb_next(&s);
        int64_t group_offset_delta = 0, group_info = 0, i;
        if (group_flags & APS2_GROUPED_BY_OFFSET_DELTA)
            group_offset_delta = sleb_next(&s);
        if (group_flags & APS2_GROUPED_BY_INFO)
            group_info = sleb_next(&s);
        if ((group_flags & APS2_HAS_ADDEND) && (group_flags & APS2_GROUPED_BY_ADDEND))
            group_addend += sleb_next(&s);
        else if (!(group_flags & APS2_HAS_ADDEND))
            group_addend = 0;
        if (group_size < 0 || !s.ok || done + group_size > count)
            return; /* malformed */
        for (i = 0; i < group_size; i++) {
            int64_t r_info;
            r_offset += (group_flags & APS2_GROUPED_BY_OFFSET_DELTA) ? group_offset_delta : sleb_next(&s);
            r_info = (group_flags & APS2_GROUPED_BY_INFO) ? group_info : sleb_next(&s);
            if (group_flags & APS2_HAS_ADDEND) {
                if (!(group_flags & APS2_GROUPED_BY_ADDEND))
                    r_addend += sleb_next(&s);
                else
                    r_addend = group_addend;
            } else {
                r_addend = 0;
            }
            if (!s.ok)
                return;
            apply_one(c, (uint64_t)r_offset, (uint64_t)r_info, r_addend);
            done++;
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
            fread(dyn, 1, (size_t)dyn_filesz, f) == dyn_filesz) {
            struct reloc_ctx c;
            uint64_t n = dyn_filesz / 16, k;
            uint64_t rela_va = 0, rela_sz = 0, andrela_va = 0, andrela_sz = 0;
            uint64_t jmprel_va = 0, pltrel_sz = 0, symtab_va = 0, strtab_va = 0, strsz = 0;
            for (k = 0; k < n; k++) {
                uint64_t tag = rd64(dyn + k * 16), val = rd64(dyn + k * 16 + 8);
                if (tag == DT_NULL) break;
                else if (tag == DT_RELA) rela_va = val;
                else if (tag == DT_RELASZ) rela_sz = val;
                else if (tag == DT_ANDROID_RELA) andrela_va = val;
                else if (tag == DT_ANDROID_RELASZ) andrela_sz = val;
                else if (tag == DT_JMPREL) jmprel_va = val;
                else if (tag == DT_PLTRELSZ) pltrel_sz = val;
                else if (tag == DT_SYMTAB) symtab_va = val;
                else if (tag == DT_STRTAB) strtab_va = val;
                else if (tag == DT_STRSZ) strsz = val;
            }
            memset(&c, 0, sizeof(c));
            c.img = out;
            c.lo = lo;
            if (symtab_va) {
                uint8_t *st = at(&c, symtab_va, 24);
                if (st) {
                    c.symtab = st;
                    c.symtab_bytes = out->span - (symtab_va - lo);
                }
            }
            if (strtab_va && strsz) {
                uint8_t *str = at(&c, strtab_va, strsz);
                if (str) {
                    c.strtab = (const char *)str;
                    c.strsz = strsz;
                }
            }
            apply_rela(&c, rela_va, rela_sz);
            apply_android_rela(&c, andrela_va, andrela_sz);
            apply_rela(&c, jmprel_va, pltrel_sz);
        }
        free(dyn);
    }

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
