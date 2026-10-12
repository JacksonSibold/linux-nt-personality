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
