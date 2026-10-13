/*
 * pe_synth.h - builds a small valid PE32+ image in memory for host tests.
 * The bytes are only read, never executed.
 *
 * Default layout (SectionAlignment 0x1000, FileAlignment 0x200):
 *   0x000-0x400  headers (SizeOfHeaders 0x400)
 *   RVA 0x1000   .text  raw 0x400 size 0x200, VirtualSize 0x100 (RX)
 *   RVA 0x2000   .data  raw 0x600 size 0x200, VirtualSize 0x1800 (RW, COW)
 *   SizeOfImage 0x4000, file size 0x800, ImageBase 0x20000000 (the host test image itself is linked at 0x140000000).
 *
 * Field offsets in the section table (for patching in tests):
 *   .text header at PES_TEXT_HDR, .data header at PES_DATA_HDR. Each entry is
 *   40 bytes: VirtualSize +8, VirtualAddress +12, SizeOfRawData +16,
 *   PointerToRawData +20, Characteristics +36.
 */
#ifndef KHOST_PE_SYNTH_H
#define KHOST_PE_SYNTH_H

#include <string.h>
#include "pe_parse.h"

#define PES_FILE_SIZE   0x800u
#define PES_OPT_OFF     0x98u     /* optional header: 0x80 + 4 + 20 */
#define PES_TEXT_HDR    0x188u    /* PES_OPT_OFF + 240 */
#define PES_DATA_HDR    0x1B0u
#define PES_SIZE_OF_IMAGE_OFF (PES_OPT_OFF + 56)
#define PES_BASE_DEFAULT 0x20000000ULL  /* not 0x140000000: MinGW places the test exe there */

typedef struct pe_synth_opts {
    int pe32;               /* 1: optional magic 0x10B and machine i386 */
    int relocs;             /* 1: BASERELOC directory present, RELOCS_STRIPPED clear */
    u64 image_base;         /* 0: PES_BASE_DEFAULT */
    u32 section_alignment;  /* 0: 0x1000 */
} pe_synth_opts_t;

static inline void pes_put16(u8 *p, u32 off, u32 v)
{
    p[off] = (u8)v;
    p[off + 1] = (u8)(v >> 8);
}

static inline void pes_put32(u8 *p, u32 off, u32 v)
{
    for (int i = 0; i < 4; i++) p[off + i] = (u8)(v >> (8 * i));
}

static inline void pes_put64(u8 *p, u32 off, u64 v)
{
    for (int i = 0; i < 8; i++) p[off + i] = (u8)(v >> (8 * i));
}

/* Fills buf (at least PES_FILE_SIZE bytes) and returns the file size. */
static inline size_t pe_synth_build(u8 *buf, const pe_synth_opts_t *o)
{
    int pe32 = o && o->pe32;
    int relocs = o && o->relocs;
    u64 base = (o && o->image_base) ? o->image_base : PES_BASE_DEFAULT;
    u32 sa = (o && o->section_alignment) ? o->section_alignment : 0x1000u;
    u32 i;

    memset(buf, 0, PES_FILE_SIZE);
    buf[0] = 'M';
    buf[1] = 'Z';
    pes_put32(buf, 0x3C, 0x80);            /* e_lfanew */
    pes_put32(buf, 0x80, 0x00004550u);     /* "PE\0\0" */

    /* IMAGE_FILE_HEADER at 0x84 */
    pes_put16(buf, 0x84, pe32 ? 0x014Cu : 0x8664u);  /* Machine */
    pes_put16(buf, 0x86, 2);                          /* NumberOfSections */
    pes_put16(buf, 0x94, 240);                        /* SizeOfOptionalHeader */
    pes_put16(buf, 0x96, relocs ? 0x0022u : 0x0023u); /* EXECUTABLE|LARGE_ADDR [|RELOCS_STRIPPED] */

    /* IMAGE_OPTIONAL_HEADER64 at PES_OPT_OFF */
    pes_put16(buf, PES_OPT_OFF, pe32 ? 0x010Bu : 0x020Bu);
    pes_put32(buf, PES_OPT_OFF + 16, 0x1000);         /* AddressOfEntryPoint */
    pes_put32(buf, PES_OPT_OFF + 20, 0x1000);         /* BaseOfCode */
    pes_put64(buf, PES_OPT_OFF + 24, base);           /* ImageBase */
    pes_put32(buf, PES_OPT_OFF + 32, sa);             /* SectionAlignment */
    pes_put32(buf, PES_OPT_OFF + 36, 0x200);          /* FileAlignment */
    pes_put32(buf, PES_SIZE_OF_IMAGE_OFF, 0x4000);    /* SizeOfImage */
    pes_put32(buf, PES_OPT_OFF + 60, 0x400);          /* SizeOfHeaders */
    pes_put16(buf, PES_OPT_OFF + 68, 3);              /* Subsystem: console */
    pes_put32(buf, PES_OPT_OFF + 108, 16);            /* NumberOfRvaAndSizes */
    if (relocs) {
        pes_put32(buf, PES_OPT_OFF + 112 + 5 * 8, 0x1000);  /* BASERELOC RVA */
        pes_put32(buf, PES_OPT_OFF + 112 + 5 * 8 + 4, 0x0C); /* BASERELOC size */
    }

    /* Section table: .text then .data */
    memcpy(buf + PES_TEXT_HDR, ".text\0\0\0", 8);
    pes_put32(buf, PES_TEXT_HDR + 8, 0x100);          /* VirtualSize */
    pes_put32(buf, PES_TEXT_HDR + 12, 0x1000);        /* VirtualAddress */
    pes_put32(buf, PES_TEXT_HDR + 16, 0x200);         /* SizeOfRawData */
    pes_put32(buf, PES_TEXT_HDR + 20, 0x400);         /* PointerToRawData */
    pes_put32(buf, PES_TEXT_HDR + 36, 0x60000020u);   /* CODE|EXEC|READ */

    memcpy(buf + PES_DATA_HDR, ".data\0\0\0", 8);
    pes_put32(buf, PES_DATA_HDR + 8, 0x1800);         /* VirtualSize */
    pes_put32(buf, PES_DATA_HDR + 12, 0x2000);        /* VirtualAddress */
    pes_put32(buf, PES_DATA_HDR + 16, 0x200);         /* SizeOfRawData */
    pes_put32(buf, PES_DATA_HDR + 20, 0x600);         /* PointerToRawData */
    pes_put32(buf, PES_DATA_HDR + 36, 0xC0000040u);   /* INITIALIZED|READ|WRITE */

    /* Section contents: a pattern per section so tests can compare bytes. */
    for (i = 0; i < 0x200; i++) {
        buf[0x400 + i] = (u8)(i * 7 + 3);
        buf[0x600 + i] = (u8)(i * 11 + 5);
    }
    return PES_FILE_SIZE;
}

#endif /* KHOST_PE_SYNTH_H */
