/*
 * pe_bounds.h - pure range checks for reading and writing PE images.
 *
 * The startup mapper (ntdll_main.c) and the loader (ldr.c) take offsets and
 * sizes from the file. Each one must be checked against the buffer it lands
 * in before the copy or the write. The checks use unsigned 64-bit arithmetic
 * with no addition that can wrap, so a huge RVA or size cannot pass.
 */
#ifndef NTDLL_PE_BOUNDS_H
#define NTDLL_PE_BOUNDS_H

/* True when the byte range [off, off + len) lies inside [0, limit). */
static inline int pe_range_fits(unsigned long long off, unsigned long long len,
                                unsigned long long limit)
{
    if (off > limit) return 0;
    return len <= limit - off;
}

/* Bytes to copy from a section's raw data: no more than its virtual size.
 * A zero virtual size means "use the raw size", as the PE loader does. */
static inline unsigned long long pe_copy_len(unsigned long long raw_size,
                                             unsigned long long virtual_size)
{
    unsigned long long vs = virtual_size ? virtual_size : raw_size;
    return raw_size < vs ? raw_size : vs;
}

#endif /* NTDLL_PE_BOUNDS_H */
