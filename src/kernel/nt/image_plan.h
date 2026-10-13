/*
 * image_plan.h - host-testable layout planner for SEC_IMAGE sections.
 *
 * Pure: no kernel and no NT headers. The parse and validation come from
 * src/loader/pe_parse.c. Given the whole file, ip_plan_build() produces the
 * mapping plan that NtCreateSection(SEC_IMAGE) records and NtMapViewOfSection
 * applies: one header region, then one segment per section, each with a
 * protection and a zero-filled tail.
 *
 * Tests: tests/kernel-host/image_plan_test.c (synthesized images and the real
 * MinGW fixtures). The userspace NtCreateSection(SEC_IMAGE) uses this planner.
 *
 * This header is used only by the userspace build (mm.c under !__KERNEL__)
 * and the host tests. The kernel build does not compile src/loader, so the
 * kernel branch of NtCreateSection does not include it.
 */
#ifndef NT_IMAGE_PLAN_H
#define NT_IMAGE_PLAN_H

#include "../../loader/pe_parse.h"
#include <string.h>

#define IP_PAGE_SIZE          0x1000u
#define IP_ALLOC_GRANULARITY  0x10000u
/* Exclusive top of the x64 user address space that an image may occupy. */
#define IP_USER_LIMIT         0x00007FFFFFFF0000ULL
#define IP_MAX_SECTIONS       96u

typedef enum ip_status {
    IP_OK = 0,
    IP_ERR_PARAM,             /* NULL pointer or zero file size */
    IP_ERR_FORMAT,            /* not a PE32+ image: MZ/PE signature, machine, truncated headers, structure */
    IP_ERR_PE32,              /* 32-bit optional header (magic 0x10B); this personality maps 64-bit only */
    IP_ERR_ALIGN,             /* ImageBase zero or not 64 KiB aligned; raw pointer not FileAlignment-aligned */
    IP_ERR_UNSUPPORTED_ALIGN, /* SectionAlignment below the page size: NT maps these flat, not built */
    IP_ERR_LAYOUT,            /* section table, overlapping ranges, or sections beyond SizeOfImage */
    IP_ERR_RAW_RANGE,         /* raw section data beyond the end of the file */
    IP_ERR_BASE               /* preferred base plus SizeOfImage outside the user address space */
} ip_status_t;

/* Page protection of one segment. Images are COW for writable sections, so a
 * write never reaches the file. */
typedef enum ip_prot {
    IP_PROT_NOACCESS = 0,
    IP_PROT_READONLY,
    IP_PROT_EXECUTE,
    IP_PROT_EXECUTE_READ,
    IP_PROT_WRITECOPY,
    IP_PROT_EXECUTE_WRITECOPY
} ip_prot_t;

typedef struct ip_segment {
    u32 rva;          /* offset from the image base, page aligned */
    u32 size;         /* mapped bytes: a multiple of SectionAlignment */
    u32 file_offset;  /* PointerToRawData, or 0 for the header region */
    u32 file_size;    /* bytes copied from the file, at most size */
    u32 zero_size;    /* size - file_size: zero-filled tail */
    u32 prot;         /* ip_prot_t */
    u32 is_header;    /* 1 for seg[0], the header region */
} ip_segment_t;

typedef struct ip_plan {
    u64 preferred_base;
    u32 size_of_image;
    u32 size_of_headers;
    u32 section_alignment;
    u32 file_alignment;
    u32 entry_rva;
    u32 relocatable;      /* 1: a base relocation directory exists and the image is not RELOCS_STRIPPED */
    u32 relocs_stripped;  /* IMAGE_FILE_RELOCS_STRIPPED in the file header */
    u32 reloc_rva;
    u32 reloc_size;
    u32 num_segments;     /* seg[0] is the header region; sections follow in table order */
    ip_segment_t seg[IP_MAX_SECTIONS + 1];
} ip_plan_t;

/* Section characteristics to a protection. Writable means COW for an image.
 * W without X is WRITECOPY and W with X is EXECUTE_WRITECOPY; R is implied by W.
 * Otherwise X gives EXECUTE or EXECUTE_READ, R alone gives READONLY, and no
 * flag gives NOACCESS. */
static inline ip_prot_t ip_prot_from_characteristics(u32 characteristics)
{
    int r = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    int w = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    int x = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    if (w) return x ? IP_PROT_EXECUTE_WRITECOPY : IP_PROT_WRITECOPY;
    if (x) return r ? IP_PROT_EXECUTE_READ : IP_PROT_EXECUTE;
    if (r) return IP_PROT_READONLY;
    return IP_PROT_NOACCESS;
}

/* Maps a pe_validate() failure to a planner status. Alignment failures that
 * pe_validate reports as section errors come back as IP_ERR_LAYOUT. */
static inline ip_status_t ip_status_from_pe(pe_status_t s)
{
    switch (s) {
    case PE_ERR_SECTIONS: return IP_ERR_LAYOUT;
    case PE_ERR_BOUNDS:   return IP_ERR_RAW_RANGE;
    default:              return IP_ERR_FORMAT;
    }
}

/* Reads the optional-header magic from the raw bytes, before any other check,
 * so a 32-bit image is named as such rather than failing the machine check.
 * Returns 0 when the headers are too short to contain the magic. */
static inline int ip_peek_magic(const u8 *d, size_t n, u16 *magic)
{
    u32 nt, off;
    if (n < 64) return 0;
    nt = (u32)d[60] | ((u32)d[61] << 8) | ((u32)d[62] << 16) | ((u32)d[63] << 24);
    if ((u64)nt + 4 + sizeof(IMAGE_FILE_HEADER) + 2 > (u64)n) return 0;
    off = nt + 4 + (u32)sizeof(IMAGE_FILE_HEADER);
    *magic = (u16)(d[off] | ((u16)d[off + 1] << 8));
    return 1;
}

/* Builds the mapping plan for a whole file. The steps, in order:
 *   1. refuse a 32-bit optional header (IP_ERR_PE32) before any other check;
 *   2. pe_validate() for structure, sizes, section table and overlaps;
 *   3. this planner's own rules: SectionAlignment at least a page, ImageBase
 *      64 KiB aligned and non-zero, the image inside user space, raw pointers
 *      FileAlignment-aligned, raw data inside the file, every mapped range
 *      inside SizeOfImage, and no overlap once rounded to pages (the header
 *      region included).
 * Segment 0 is the header region (read-only). The sections follow, sorted by
 * RVA. A section with no VirtualSize and no raw data maps nothing and is
 * left out. On success *out is fully written and out->num_segments is set. */
static inline ip_status_t ip_plan_build(const u8 *data, size_t file_size, ip_plan_t *out)
{
    pe_context_t ctx;
    pe_data_directory_t dir;
    u16 magic = 0, nsec, i;
    u32 nt_off, fchars, n = 0;
    u32 sa, fa, hdr_size;
    u64 base;

    if (!data || !out || file_size == 0) return IP_ERR_PARAM;
    memset(out, 0, sizeof *out);

    /* The PE32 check runs first. Anything that is not MZ is a format error. */
    if (file_size < 2 || data[0] != 'M' || data[1] != 'Z') return IP_ERR_FORMAT;
    if (ip_peek_magic(data, file_size, &magic) && magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return IP_ERR_PE32;

    if (pe_context_init(&ctx, data, file_size) != PE_OK) return IP_ERR_FORMAT;
    {
        pe_status_t s = pe_validate(&ctx);
        if (s != PE_OK) return ip_status_from_pe(s);
    }

    sa = ctx.section_alignment;
    fa = ctx.file_alignment;
    base = ctx.image_base;
    if (sa < IP_PAGE_SIZE) return IP_ERR_UNSUPPORTED_ALIGN;
    if (base == 0 || (base & (IP_ALLOC_GRANULARITY - 1)) != 0) return IP_ERR_ALIGN;
    if (base >= IP_USER_LIMIT || (u64)ctx.size_of_image > IP_USER_LIMIT - base) return IP_ERR_BASE;

    nt_off = ctx.nt_off;
    fchars = pe_file_u16(&ctx, nt_off + 4 + 18);   /* IMAGE_FILE_HEADER.Characteristics */

    out->preferred_base = base;
    out->size_of_image = ctx.size_of_image;
    out->size_of_headers = ctx.size_of_headers;
    out->section_alignment = sa;
    out->file_alignment = fa;
    out->entry_rva = ctx.entry_point;
    out->relocs_stripped = (fchars & IMAGE_FILE_RELOCS_STRIPPED) != 0;
    if (pe_directory_get(&ctx, IMAGE_DIRECTORY_ENTRY_BASERELOC, &dir) == PE_OK && dir.rva != 0 && dir.size != 0) {
        out->reloc_rva = dir.rva;
        out->reloc_size = dir.size;
        out->relocatable = !out->relocs_stripped;
    }

    /* Segment 0: the header region, page-rounded and read-only. The file
     * supplies SizeOfHeaders bytes and the rest of the page is zero. */
    hdr_size = ctx.size_of_headers;
    if (hdr_size == 0 || hdr_size > ctx.size_of_image) return IP_ERR_LAYOUT;
    out->seg[0].rva = 0;
    out->seg[0].size = (u32)(((u64)hdr_size + IP_PAGE_SIZE - 1) & ~((u64)IP_PAGE_SIZE - 1));
    out->seg[0].file_offset = 0;
    out->seg[0].file_size = hdr_size;
    out->seg[0].zero_size = out->seg[0].size - hdr_size;
    out->seg[0].prot = IP_PROT_READONLY;
    out->seg[0].is_header = 1;
    n = 1;

    nsec = ctx.num_sections;
    if (nsec == 0 || nsec > IP_MAX_SECTIONS) return IP_ERR_LAYOUT;
    for (i = 0; i < nsec; i++) {
        pe_section_view_t view;
        const IMAGE_SECTION_HEADER *h;
        u64 vsize, mapped, rva, raw, ptr, copy;
        ip_segment_t *s;

        if (pe_section_get(&ctx, i, &view) != PE_OK) return IP_ERR_FORMAT;
        h = &view.hdr;
        vsize = h->Misc.VirtualSize;
        if (vsize == 0) vsize = h->SizeOfRawData;
        if (vsize == 0) continue;   /* nothing to map */

        rva = h->VirtualAddress;
        raw = h->SizeOfRawData;
        ptr = h->PointerToRawData;
        if (rva % sa != 0) return IP_ERR_LAYOUT;
        mapped = (vsize + sa - 1) & ~((u64)sa - 1);
        if (rva + mapped > ctx.size_of_image) return IP_ERR_LAYOUT;
        if (raw != 0) {
            if (ptr % fa != 0) return IP_ERR_ALIGN;
            if (ptr + raw > (u64)file_size) return IP_ERR_RAW_RANGE;
        }
        /* The file supplies min(raw, VirtualSize) bytes; the rest of the mapped
         * range is zero. Raw bytes beyond VirtualSize are not mapped. */
        copy = raw < vsize ? raw : vsize;

        if (n > IP_MAX_SECTIONS) return IP_ERR_LAYOUT;
        s = &out->seg[n++];
        s->rva = (u32)rva;
        s->size = (u32)mapped;
        s->file_offset = (u32)ptr;
        s->file_size = (u32)copy;
        s->zero_size = (u32)(mapped - copy);
        s->prot = (u32)ip_prot_from_characteristics(h->Characteristics);
        s->is_header = 0;
    }
    out->num_segments = n;

    /* Sort the sections by RVA (insertion sort; at most 96 entries). */
    for (i = 2; i < n; i++) {
        ip_segment_t key = out->seg[i];
        u32 j = i;
        while (j > 1 && out->seg[j - 1].rva > key.rva) {
            out->seg[j] = out->seg[j - 1];
            j--;
        }
        out->seg[j] = key;
    }

    /* Mapped ranges must not overlap once rounded to pages. The header region
     * is seg[0] and is not sorted, so it is checked here too. */
    {
        u32 a, b;
        for (a = 0; a < n; a++) {
            for (b = a + 1; b < n; b++) {
                const ip_segment_t *x = &out->seg[a], *y = &out->seg[b];
                if ((u64)x->rva < (u64)y->rva + y->size && (u64)y->rva < (u64)x->rva + x->size)
                    return IP_ERR_LAYOUT;
            }
        }
    }
    return IP_OK;
}

#endif /* NT_IMAGE_PLAN_H */
