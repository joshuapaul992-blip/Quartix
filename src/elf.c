/*
 * Quartix - read-only inspection of the Android native library (ELF64).
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 *
 * The Roblox Android app ships its engine as lib/x86_64/libroblox.so, an ELF64
 * shared object built for Linux/Android. Running it the way Sober and Mocktail
 * do on Unix means loading this ELF ourselves, so the first step - shared by
 * every later phase - is to read and understand it. This module only parses it;
 * it maps and runs nothing. Everything here is bounded and tolerates garbage,
 * since the file is attacker-influenced (it comes from an APK the user supplies).
 */
#include "quartix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Minimal ELF64 definitions (mingw has no <elf.h>); little-endian on disk. */
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_STRTAB 5
#define DT_STRSZ 10
#define DT_SONAME 14

#define PHNUM_CAP 128
#define DYN_CAP 8192
#define STRTAB_CAP (256 * 1024)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

struct phdr {
    uint32_t type;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
};

/* Translate a virtual address to a file offset using the PT_LOAD segments. */
static int vaddr_to_off(const struct phdr *ph, unsigned nph, uint64_t va, uint64_t *off)
{
    unsigned i;
    for (i = 0; i < nph; i++) {
        if (ph[i].type == PT_LOAD && va >= ph[i].vaddr && va < ph[i].vaddr + ph[i].filesz) {
            *off = ph[i].offset + (va - ph[i].vaddr);
            return 0;
        }
    }
    return -1;
}

int qx_elf_read(const wchar_t *path, qx_elf_info *out)
{
    FILE *f;
    uint8_t eh[64];
    struct phdr ph[PHNUM_CAP];
    uint64_t phoff, dyn_off = 0, dyn_filesz = 0, strtab_va = 0, soname_off = (uint64_t)-1;
    uint64_t lo = (uint64_t)~0ull, hi = 0;
    unsigned phnum, phentsize, i, nph = 0;
    long strtab_off = -1;
    uint64_t strtab_sz = 0;
    uint8_t *strtab = NULL;
    uint64_t needed_va[32];
    unsigned needed_n = 0;

    memset(out, 0, sizeof(*out));
    f = _wfopen(path, L"rb");
    if (!f)
        return -1;
    if (fread(eh, 1, sizeof(eh), f) != sizeof(eh))
        goto fail;

    /* ELF magic, 64-bit (EI_CLASS=2), little-endian (EI_DATA=1). */
    if (memcmp(eh, "\x7f""ELF", 4) != 0 || eh[4] != 2 || eh[5] != 1) {
        fclose(f);
        return 0; /* readable, but not an ELF64 we can run */
    }
    out->type = rd16(eh + 16);
    out->machine = rd16(eh + 18);
    out->entry = rd64(eh + 24);
    out->is_elf64_x86_64 = (out->machine == EM_X86_64);

    phoff = rd64(eh + 32);
    phentsize = rd16(eh + 54);
    phnum = rd16(eh + 56);
    if (phentsize != 56 || phnum == 0 || phnum > PHNUM_CAP)
        goto done; /* unusual layout; report what the header already told us */

    if (fseek(f, (long)phoff, SEEK_SET) != 0)
        goto done;
    for (i = 0; i < phnum; i++) {
        uint8_t pe[56];
        if (fread(pe, 1, sizeof(pe), f) != sizeof(pe))
            goto done;
        ph[nph].type = rd32(pe);
        ph[nph].offset = rd64(pe + 8);
        ph[nph].vaddr = rd64(pe + 16);
        ph[nph].filesz = rd64(pe + 32);
        ph[nph].memsz = rd64(pe + 40);
        if (ph[nph].type == PT_LOAD) {
            out->load_segments++;
            if (ph[nph].vaddr < lo)
                lo = ph[nph].vaddr;
            if (ph[nph].vaddr + ph[nph].memsz > hi)
                hi = ph[nph].vaddr + ph[nph].memsz;
        } else if (ph[nph].type == PT_DYNAMIC) {
            dyn_off = ph[nph].offset;
            dyn_filesz = ph[nph].filesz;
        } else if (ph[nph].type == PT_TLS) {
            out->has_tls = 1;
        }
        nph++;
    }
    if (hi > lo)
        out->mapped_size = hi - lo;

    /* Walk the dynamic section: collect DT_NEEDED and locate the string table. */
    if (dyn_off && dyn_filesz && dyn_filesz <= DYN_CAP * 16 && fseek(f, (long)dyn_off, SEEK_SET) == 0) {
        uint64_t n = dyn_filesz / 16, k;
        if (n > DYN_CAP)
            n = DYN_CAP;
        for (k = 0; k < n; k++) {
            uint8_t de[16];
            uint64_t tag, val;
            if (fread(de, 1, sizeof(de), f) != sizeof(de))
                break;
            tag = rd64(de);
            val = rd64(de + 8);
            if (tag == DT_NULL)
                break;
            if (tag == DT_NEEDED) {
                out->needed_count++;
                if (needed_n < 32)
                    needed_va[needed_n++] = val;
            } else if (tag == DT_SONAME) {
                soname_off = val;
            } else if (tag == DT_STRTAB) {
                strtab_va = val;
            } else if (tag == DT_STRSZ) {
                strtab_sz = val;
            }
        }
    }

    /* Resolve names in the string table, if we can map it from the file. */
    if (strtab_va && strtab_sz && strtab_sz <= STRTAB_CAP) {
        uint64_t so;
        if (vaddr_to_off(ph, nph, strtab_va, &so) == 0) {
            strtab_off = (long)so;
            strtab = (uint8_t *)malloc((size_t)strtab_sz + 1);
            if (strtab && fseek(f, strtab_off, SEEK_SET) == 0) {
                size_t got = fread(strtab, 1, (size_t)strtab_sz, f);
                strtab[got] = 0;
                strtab_sz = got;
            } else if (strtab) {
                free(strtab);
                strtab = NULL;
            }
        }
    }
    if (strtab) {
        if (soname_off != (uint64_t)-1 && soname_off < strtab_sz)
            snprintf(out->soname, sizeof(out->soname), "%s", (char *)strtab + soname_off);
        for (i = 0; i < needed_n && out->needed_listed < QX_ELF_NEEDED_MAX; i++) {
            if (needed_va[i] < strtab_sz) {
                snprintf(out->needed[out->needed_listed], sizeof(out->needed[0]), "%s",
                         (char *)strtab + needed_va[i]);
                out->needed_listed++;
            }
        }
        free(strtab);
    }

done:
    (void)strtab_off;
    fclose(f);
    return 0;
fail:
    fclose(f);
    return -1;
}
