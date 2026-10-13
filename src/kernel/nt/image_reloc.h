/*
 * image_reloc.h - host-testable base relocation applier for SEC_IMAGE views.
 *
 * Pure: no kernel and no NT headers, no allocation. Given the bytes of a mapped
 * image (laid out at their RVAs, as the mapping copies them), the preferred
 * base from the header and the actual base of the view, ir_apply() adds the
 * base delta to every fixup named by the base relocation directory. Integer
 * arithmetic is modular, so a view below the preferred base (delta negative)
 * works the same way.
 *
 * Two passes. Pass one walks the whole directory and checks every block and
 * every fixup, marking each target byte in `seen`; it writes nothing. Pass two
 * applies the fixups. A failed call therefore leaves the image unchanged. The
 * directory bytes are marked too, so no fixup can rewrite the directory it is
 * read from.
 *
 * Fixup types: ABSOLUTE (0, no-op, not counted), HIGH (1), LOW (2), HIGHLOW
 * (3), DIR64 (10). HIGHADJ (4) takes a second word and is refused, as are the
 * reserved types (6 and 11-15).
 *
 * Tests: tests/kernel-host/image_reloc_test.c (synthesized images and the real
 * MinGW fixtures, read as bytes and never executed). The userspace
 * NtMapViewOfSection(SEC_IMAGE) applies relocations through this header.
 */
#ifndef NT_IMAGE_RELOC_H
#define NT_IMAGE_RELOC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* IMAGE_BASE_RELOCATION: VirtualAddress, SizeOfBlock. */
#define IR_BLOCK_HDR 8u

typedef enum ir_status {
    IR_OK = 0,
    IR_ERR_PARAM,   /* NULL pointer, image shorter than SizeOfImage, or seen bitmap too small */
    IR_ERR_DIR,     /* directory outside SizeOfImage (or wrapping) */
    IR_ERR_BLOCK,   /* block size under 8, not a multiple of 4, past the directory, or a trailing fragment */
    IR_ERR_PAGE,    /* block VirtualAddress at or beyond SizeOfImage */
    IR_ERR_TYPE,    /* fixup type not supported (HIGHADJ, reserved) */
    IR_ERR_TARGET,  /* a fixup's write is not fully inside SizeOfImage */
    IR_ERR_OVERLAP  /* two fixups write the same byte, or a fixup writes into the directory */
} ir_status_t;

enum {
    IR_REL_ABSOLUTE = 0,
    IR_REL_HIGH     = 1,
    IR_REL_LOW      = 2,
    IR_REL_HIGHLOW  = 3,
    IR_REL_DIR64    = 10
};

/* Bytes of the seen bitmap for an image of size_of_image bytes (one bit each). */
static inline size_t ir_seen_bytes(uint32_t size_of_image)
{
    return ((size_t)size_of_image + 7) / 8;
}

static inline uint64_t ir_ld(const uint8_t *p, unsigned w)
{
    uint64_t v = 0;
    unsigned k;
    for (k = 0; k < w; k++) v |= (uint64_t)p[k] << (8 * k);
    return v;
}

static inline void ir_st(uint8_t *p, uint64_t v, unsigned w)
{
    unsigned k;
    for (k = 0; k < w; k++) p[k] = (uint8_t)(v >> (8 * k));
}

/* Sets bits [start, start + n) and returns 1 if any was already set. */
static inline int ir_mark_range(uint8_t *seen, uint64_t start, uint64_t n)
{
    int hit = 0;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t b = start + i;
        uint8_t m = (uint8_t)(1u << (b & 7));
        if (seen[b >> 3] & m) hit = 1;
        seen[b >> 3] |= m;
    }
    return hit;
}

/* Walks the directory. With apply == 0 it validates and marks `seen` (must be
 * non-NULL). With apply != 0 it writes the fixups; `seen` is then not used. */
static ir_status_t ir_walk(uint8_t *image, uint32_t size_of_image, uint64_t delta,
                           uint32_t dir_rva, uint32_t dir_size,
                           uint8_t *seen, int apply, uint32_t *applied)
{
    uint64_t cursor = 0;
    uint32_t n = 0;

    while (cursor < dir_size) {
        const uint8_t *blk = image + dir_rva + cursor;
        uint32_t page, bsize, count, i;

        if ((uint64_t)dir_size - cursor < IR_BLOCK_HDR) return IR_ERR_BLOCK;
        page = (uint32_t)ir_ld(blk, 4);
        bsize = (uint32_t)ir_ld(blk + 4, 4);
        if (bsize == 0) break;   /* end of directory, as in pe_relocs_walk */

        if (page >= size_of_image) return IR_ERR_PAGE;
        if (bsize < IR_BLOCK_HDR || (bsize & 3u) != 0) return IR_ERR_BLOCK;
        if ((uint64_t)bsize > (uint64_t)dir_size - cursor) return IR_ERR_BLOCK;

        count = (bsize - IR_BLOCK_HDR) / 2;
        for (i = 0; i < count; i++) {
            uint32_t e = (uint32_t)ir_ld(blk + IR_BLOCK_HDR + 2 * i, 2);
            uint32_t type = e >> 12;
            uint64_t target = (uint64_t)page + (e & 0xFFFu);
            unsigned width;

            switch (type) {
            case IR_REL_ABSOLUTE: continue;
            case IR_REL_HIGH:
            case IR_REL_LOW:      width = 2; break;
            case IR_REL_HIGHLOW:  width = 4; break;
            case IR_REL_DIR64:    width = 8; break;
            default:              return IR_ERR_TYPE;
            }
            if (target + width > (uint64_t)size_of_image) return IR_ERR_TARGET;

            if (!apply) {
                if (ir_mark_range(seen, target, width)) return IR_ERR_OVERLAP;
                continue;
            }
            {
                uint8_t *p = image + target;
                uint64_t v = ir_ld(p, width);
                switch (type) {
                case IR_REL_HIGH:    v = (v + ((delta >> 16) & 0xFFFFu)) & 0xFFFFu; break;
                case IR_REL_LOW:     v = (v + (delta & 0xFFFFu)) & 0xFFFFu; break;
                case IR_REL_HIGHLOW: v = (v + (delta & 0xFFFFFFFFu)) & 0xFFFFFFFFu; break;
                default:             v = v + delta; break;   /* DIR64, modular */
                }
                ir_st(p, v, width);
                n++;
            }
        }
        cursor += bsize;
    }
    if (applied) *applied = n;
    return IR_OK;
}

/* Applies the base relocations of one image view.
 *
 *   image        the mapped image, at least size_of_image bytes
 *   image_len    bytes available at `image`
 *   size_of_image SizeOfImage from the header
 *   preferred_base ImageBase from the header
 *   actual_base  where the view was placed (delta = actual - preferred)
 *   dir_rva/dir_size the BASERELOC data directory (0/0 means none)
 *   seen, seen_len scratch bitmap of ir_seen_bytes(size_of_image) bytes;
 *                  cleared by this call
 *   applied      receives the number of fixups written (ABSOLUTE not counted)
 *
 * Returns IR_OK, or the first error. On any error the image is unchanged and
 * *applied is 0. A zero delta, or no directory, is IR_OK with nothing applied. */
static inline ir_status_t ir_apply(uint8_t *image, size_t image_len, uint32_t size_of_image,
                                   uint64_t preferred_base, uint64_t actual_base,
                                   uint32_t dir_rva, uint32_t dir_size,
                                   uint8_t *seen, size_t seen_len, uint32_t *applied)
{
    uint64_t delta;
    ir_status_t s;

    if (!applied) return IR_ERR_PARAM;
    *applied = 0;
    if (!image || !seen || size_of_image == 0 || image_len < size_of_image) return IR_ERR_PARAM;
    if (seen_len < ir_seen_bytes(size_of_image)) return IR_ERR_PARAM;

    if (dir_rva == 0 || dir_size == 0) return IR_OK;
    if ((uint64_t)dir_rva + dir_size > (uint64_t)size_of_image) return IR_ERR_DIR;

    delta = actual_base - preferred_base;
    if (delta == 0) return IR_OK;

    memset(seen, 0, ir_seen_bytes(size_of_image));
    (void)ir_mark_range(seen, dir_rva, dir_size);
    s = ir_walk(image, size_of_image, delta, dir_rva, dir_size, seen, 0, NULL);
    if (s != IR_OK) return s;
    return ir_walk(image, size_of_image, delta, dir_rva, dir_size, NULL, 1, applied);
}

#endif /* NT_IMAGE_RELOC_H */
