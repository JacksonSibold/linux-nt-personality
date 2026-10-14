/* pe_text.h - UTF-8/UTF-16 conversion, command-line and environment block
 * builders, and PE protection to mmap() protection, for the PE loader.
 *
 * Pure functions over caller-supplied buffers: no allocation, no kernel API.
 * Every builder returns the number of UTF-16 code units written, including
 * the terminating NUL(s), or 0 if the output does not fit in `cap` units.
 */
#ifndef PE_TEXT_H
#define PE_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include "pe_prot.h"

/* UTF-8 string to UTF-16 (invalid bytes become U+FFFD; code points above
 * U+FFFF become surrogate pairs). Returns units including the NUL. */
size_t pe_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);

/* argv joined with single spaces. An argument with a space or tab, or an
 * empty argument, is wrapped in double quotes, with embedded quotes escaped
 * as \". Returns units including the NUL. */
size_t pe_build_command_line(int argc, const char *const *argv, uint16_t *buf, size_t cap);

/* "NAME=VALUE" entries, each NUL-terminated, followed by one extra NUL.
 * Returns units including both final NULs. */
size_t pe_build_environment(int envc, const char *const *envp, uint16_t *buf, size_t cap);

/* PE_PROT_* bits to the PROT_* bits mmap() takes. */
static inline unsigned long pe_prot_to_mmap(u32 prot)
{
    unsigned long p = 0;
    if (prot & PE_PROT_READ)  p |= PROT_READ;
    if (prot & PE_PROT_WRITE) p |= PROT_WRITE;
    if (prot & PE_PROT_EXEC)  p |= PROT_EXEC;
    return p;
}

#endif /* PE_TEXT_H */
