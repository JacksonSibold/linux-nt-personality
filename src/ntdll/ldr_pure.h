/*
 * ldr_pure.h - pure address translation and bounds checks for the loader
 * (src/ntdll/ldr.c). No NT types and no I/O: the functions take buffers and
 * RVAs, so tests/ntdll-host can exercise them on an in-memory image.
 *
 * The loader holds two address spaces for a module. The file bytes are
 * indexed by file offset (pe_rva_to_offset). The mapped image is indexed by
 * RVA. Anything written into the image takes an RVA, never a file offset
 * (docs/review/ntdll.md LDR-2, decision 0030).
 */
#ifndef NTDLL_LDR_PURE_H
#define NTDLL_LDR_PURE_H

#include <string.h>
#include "pe_bounds.h"

/* RVA of the IAT for one import descriptor. FirstThunk is the IAT. A zero
 * FirstThunk falls back to the ILT RVA, which is the loader's old fallback,
 * now kept in RVA space. */
static inline unsigned long long ldr_iat_base_rva(unsigned int first_thunk,
                                                  unsigned int original_first_thunk)
{
    return first_thunk ? first_thunk : original_first_thunk;
}

/* Store one resolved address in IAT slot `index`, at image RVA
 * base_rva + index * 8. The slot must lie inside the image. A rejected write
 * changes nothing. Returns 1 when written, 0 when the slot is out of range.
 * The arithmetic is ordered so that no step can wrap. */
static inline int ldr_iat_write(unsigned char *image, unsigned long long image_size,
                                unsigned long long base_rva, unsigned long long index,
                                unsigned long long value)
{
    const unsigned long long width = sizeof(value);

    if (base_rva > image_size) return 0;
    if (index > (image_size - base_rva) / width) return 0;

    unsigned long long slot = base_rva + index * width;
    if (!pe_range_fits(slot, width, image_size)) return 0;

    memcpy(image + slot, &value, sizeof(value));
    return 1;
}

/* Status for an import that cannot be bound (Windows loader). A DLL that
 * cannot be found fails with STATUS_DLL_NOT_FOUND (0xC0000135). A DLL that is
 * found but lacks the imported name or ordinal fails with
 * STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139). Returns 0 (STATUS_SUCCESS) when
 * both were found. The caller must fail the load on any non-zero result. */
#define LDR_PURE_STATUS_SUCCESS 0x00000000UL
#define LDR_PURE_STATUS_DLL_NOT_FOUND 0xC0000135UL
#define LDR_PURE_STATUS_ENTRYPOINT_NOT_FOUND 0xC0000139UL

static inline unsigned long ldr_unbound_import_status(int dll_found, int symbol_found)
{
    if (!dll_found) return LDR_PURE_STATUS_DLL_NOT_FOUND;
    if (!symbol_found) return LDR_PURE_STATUS_ENTRYPOINT_NOT_FOUND;
    return LDR_PURE_STATUS_SUCCESS;
}

/* An IAT slot that the binder never patched still holds the import's name or
 * hint RVA, which is an address inside the image's own RVA range. A slot bound
 * to a real export holds image base + RVA, so it lies above the image size
 * (the mapped base is never below the image size in the personality). Returns
 * 1 when the slot value is a raw RVA (unbound), 0 otherwise. */
static inline int ldr_iat_slot_is_raw_rva(unsigned long long slot_value,
                                          unsigned long long image_size)
{
    return slot_value != 0 && slot_value < image_size;
}

/* TLS callback table (LDR-4, decision 0032). AddressOfCallBacks in the TLS
 * directory is a VA on the preferred base. The table itself lives in the
 * image, at VA - OriginalBase. */
typedef enum {
    LDR_TLS_CB_NONE = 0,  /* no table: AddressOfCallBacks is 0 */
    LDR_TLS_CB_OK = 1,    /* table head lies in the image; *out_rva is set */
    LDR_TLS_CB_BAD = 2    /* table is outside the image: do not read it */
} ldr_tls_cb_status_t;

static inline ldr_tls_cb_status_t ldr_tls_cb_table_rva(unsigned long long table_va,
                                                       unsigned long long original_base,
                                                       unsigned long long image_size,
                                                       unsigned int *out_rva)
{
    unsigned long long rva;

    if (table_va == 0) return LDR_TLS_CB_NONE;
    if (table_va < original_base) return LDR_TLS_CB_BAD;

    rva = table_va - original_base;
    if (rva > 0xFFFFFFFFULL) return LDR_TLS_CB_BAD;
    if (!pe_range_fits(rva, sizeof(unsigned long long), image_size)) return LDR_TLS_CB_BAD;

    *out_rva = (unsigned int)rva;
    return LDR_TLS_CB_OK;
}

/* -------------------------------------------------------------------------- */
/* Startup modules: headers and exports of an image laid out by RVA           */
/* -------------------------------------------------------------------------- */

/* The startup binder (ntdll_main.c) maps the DLLs it binds as NT sections,
 * so their bytes are indexed by RVA, not by file offset. These helpers read
 * such an image in place. Every read is bounds-checked against the span the
 * caller passes. */

typedef struct ldr_mapped_hdr {
    unsigned long long image_base;      /* OptionalHeader.ImageBase */
    unsigned long entry_rva;            /* AddressOfEntryPoint */
    unsigned long size_of_image;        /* SizeOfImage */
    unsigned long time_date_stamp;      /* FileHeader.TimeDateStamp */
    unsigned long export_rva;           /* export data directory */
    unsigned long export_size;
} ldr_mapped_hdr_t;

#define LDR_PURE_EXPORT_FOUND      0
#define LDR_PURE_EXPORT_NOT_FOUND  1
#define LDR_PURE_EXPORT_BAD_IMAGE  2

static inline unsigned long ldr_mapped_u32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static inline unsigned long long ldr_mapped_u64(const unsigned char *p)
{
    return (unsigned long long)ldr_mapped_u32(p) |
           ((unsigned long long)ldr_mapped_u32(p + 4) << 32);
}

/* Reads a PE32+ header from an image whose first `avail` bytes are readable.
 * Returns 1 and fills *out, or 0 for a bad signature, a PE32 (not PE32+)
 * image, or headers that run past avail. */
static inline int ldr_mapped_headers(const unsigned char *image,
                                     unsigned long long avail,
                                     ldr_mapped_hdr_t *out)
{
    unsigned long long nt, oh;
    unsigned long ndirs;

    if (!pe_range_fits(0, 0x40, avail)) return 0;
    if (image[0] != 'M' || image[1] != 'Z') return 0;

    nt = ldr_mapped_u32(image + 0x3C);
    if (!pe_range_fits(nt, 24, avail)) return 0;
    if (image[nt] != 'P' || image[nt + 1] != 'E' || image[nt + 2] != 0 || image[nt + 3] != 0)
        return 0;

    oh = nt + 24;
    if (image[oh] != 0x0B || image[oh + 1] != 0x02) return 0;   /* 0x20B, PE32+ */
    if (!pe_range_fits(oh, 112, avail)) return 0;

    ndirs = ldr_mapped_u32(image + oh + 108);
    out->image_base = ldr_mapped_u64(image + oh + 24);
    out->entry_rva = ldr_mapped_u32(image + oh + 16);
    out->size_of_image = ldr_mapped_u32(image + oh + 56);
    out->time_date_stamp = ldr_mapped_u32(image + nt + 8);
    out->export_rva = 0;
    out->export_size = 0;
    if (ndirs > 0) {
        if (!pe_range_fits(oh, 112 + 8, avail)) return 0;
        out->export_rva = ldr_mapped_u32(image + oh + 112);
        out->export_size = ldr_mapped_u32(image + oh + 116);
    }
    return 1;
}

/* The NUL-terminated string at rva, which must end inside the image. Sets
 * *out and *len (not counting the NUL). Returns 0 when no NUL lies before the
 * image end. */
static inline int ldr_mapped_cstr(const unsigned char *image, unsigned long long image_size,
                                  unsigned long long rva, const char **out, size_t *len)
{
    if (rva >= image_size) return 0;
    for (unsigned long long i = rva; i < image_size; ++i) {
        if (image[i] == 0) {
            *out = (const char *)(image + rva);
            *len = (size_t)(i - rva);
            return 1;
        }
    }
    return 0;
}

/* Export lookup by name (case-sensitive, LDR-7) or by ordinal (Base-relative),
 * in an image of image_size bytes laid out by RVA. Returns one of the
 * LDR_PURE_EXPORT_* codes. On FOUND, *out_rva is the export RVA and
 * *out_forwarder is 1 when that RVA lies inside the export directory (its
 * bytes are then a "DLL.Name" forwarder string, read with ldr_mapped_cstr),
 * else 0. */
static inline int ldr_mapped_export(const unsigned char *image, unsigned long long image_size,
                                    const char *name, unsigned long ordinal, int by_ordinal,
                                    unsigned long *out_rva, int *out_forwarder)
{
    ldr_mapped_hdr_t h;
    unsigned long long ed, base, nfuncs, nnames, afuncs, anames, aord, func_idx, rva;
    int found = 0;

    if (!ldr_mapped_headers(image, image_size, &h)) return LDR_PURE_EXPORT_BAD_IMAGE;
    if (h.export_rva == 0 || h.export_size == 0) return LDR_PURE_EXPORT_NOT_FOUND;
    if (!pe_range_fits(h.export_rva, h.export_size, image_size)) return LDR_PURE_EXPORT_BAD_IMAGE;
    if (!pe_range_fits(h.export_rva, 40, image_size)) return LDR_PURE_EXPORT_BAD_IMAGE;

    ed = h.export_rva;
    base = ldr_mapped_u32(image + ed + 0x10);
    nfuncs = ldr_mapped_u32(image + ed + 0x14);
    nnames = ldr_mapped_u32(image + ed + 0x18);
    afuncs = ldr_mapped_u32(image + ed + 0x1C);
    anames = ldr_mapped_u32(image + ed + 0x20);
    aord = ldr_mapped_u32(image + ed + 0x24);

    if (nfuncs == 0) return LDR_PURE_EXPORT_NOT_FOUND;
    if (!pe_range_fits(afuncs, nfuncs * 4, image_size)) return LDR_PURE_EXPORT_BAD_IMAGE;

    if (by_ordinal) {
        if (ordinal < base) return LDR_PURE_EXPORT_NOT_FOUND;
        if ((unsigned long long)ordinal - base >= nfuncs) return LDR_PURE_EXPORT_NOT_FOUND;
        func_idx = (unsigned long long)ordinal - base;
        found = 1;
    } else {
        if (name == NULL) return LDR_PURE_EXPORT_NOT_FOUND;
        if (!pe_range_fits(anames, nnames * 4, image_size)) return LDR_PURE_EXPORT_BAD_IMAGE;
        if (!pe_range_fits(aord, nnames * 2, image_size)) return LDR_PURE_EXPORT_BAD_IMAGE;
        size_t want = strlen(name);
        func_idx = 0;
        for (unsigned long long i = 0; i < nnames && !found; ++i) {
            const char *s;
            size_t len;
            unsigned long long name_rva = ldr_mapped_u32(image + anames + i * 4);
            if (!ldr_mapped_cstr(image, image_size, name_rva, &s, &len)) continue;
            if (len != want || memcmp(s, name, len) != 0) continue;
            func_idx = (unsigned long long)(image[aord + i * 2] | (image[aord + i * 2 + 1] << 8));
            if (func_idx >= nfuncs) return LDR_PURE_EXPORT_BAD_IMAGE;
            found = 1;
        }
        if (!found) return LDR_PURE_EXPORT_NOT_FOUND;
    }

    rva = ldr_mapped_u32(image + afuncs + func_idx * 4);
    if (rva == 0) return LDR_PURE_EXPORT_NOT_FOUND;
    if (rva >= image_size) return LDR_PURE_EXPORT_BAD_IMAGE;

    *out_rva = (unsigned long)rva;
    *out_forwarder = (rva >= h.export_rva && rva < (unsigned long long)h.export_rva + h.export_size);
    return LDR_PURE_EXPORT_FOUND;
}

/* Callback entry `index` of the table at table_rva. Returns 1 with *out set
 * to a non-zero callback VA. Returns 0 at the zero terminator, and also when
 * the entry would lie past the image, so a table with no terminator stops at
 * the image end. *out is written whenever the entry is inside the image, zero
 * included, and left alone otherwise. */
static inline int ldr_tls_cb_entry(const unsigned char *image, unsigned long long image_size,
                                   unsigned long long table_rva, unsigned long long index,
                                   unsigned long long *out)
{
    const unsigned long long width = sizeof(*out);
    unsigned long long slot, value;

    if (table_rva > image_size) return 0;
    if (index > (image_size - table_rva) / width) return 0;

    slot = table_rva + index * width;
    if (!pe_range_fits(slot, width, image_size)) return 0;

    memcpy(&value, image + slot, sizeof(value));
    *out = value;
    return value != 0;
}

#endif /* NTDLL_LDR_PURE_H */
