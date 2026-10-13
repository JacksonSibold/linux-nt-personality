/*
 * image_plan.h - host-testable layout planner for SEC_IMAGE sections.
 *
 * Pure: no kernel and no NT headers. The parse and validation come from
 * src/loader/pe_parse.c. Given the whole file, ip_plan_build() produces the
 * mapping plan that NtCreateSection(SEC_IMAGE) records and NtMapViewOfSection
 * applies: one header region, then one segment per section, each with a
 * protection and a zero-filled tail.
 *
 * Status: planner stub. ip_plan_build() refuses everything until the planner
 * is implemented (failing-first tests in tests/kernel-host/image_plan_test.c).
 *
 * This header is used only by the userspace build (mm.c under !__KERNEL__)
 * and the host tests. The kernel build does not compile src/loader, so the
 * kernel branch of NtCreateSection does not include it.
 */
#ifndef NT_IMAGE_PLAN_H
#define NT_IMAGE_PLAN_H

#include "pe_parse.h"

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

/* Stub: every call fails. The real planner replaces this body. */
static inline ip_prot_t ip_prot_from_characteristics(u32 characteristics)
{
    (void)characteristics;
    return IP_PROT_NOACCESS;
}

/* Stub: every image is refused until the planner is implemented. */
static inline ip_status_t ip_plan_build(const u8 *data, size_t file_size, ip_plan_t *out)
{
    (void)data;
    (void)file_size;
    (void)out;
    return IP_ERR_PARAM;
}

#endif /* NT_IMAGE_PLAN_H */
