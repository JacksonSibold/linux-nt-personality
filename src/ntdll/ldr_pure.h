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

#endif /* NTDLL_LDR_PURE_H */
