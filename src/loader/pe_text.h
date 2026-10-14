/* pe_text.h - UTF-8/UTF-16 conversion and command-line and environment block
 * builders for the PE loader. Built into the kernel (fs/binfmt_pe.c, where
 * __KERNEL__ is set) and into the userspace tests. It does not include
 * pe_prot.h, whose declarations clash with binfmt_pe.c's statics.
 *
 * Pure functions over caller-supplied buffers: no allocation, no kernel API.
 * Every builder returns the number of UTF-16 code units written, including
 * the terminating NUL(s), or 0 if the output does not fit in `cap` units.
 */
#ifndef PE_TEXT_H
#define PE_TEXT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* UTF-8 string to UTF-16 (invalid bytes become U+FFFD; code points above
 * U+FFFF become surrogate pairs). Returns units including the NUL. */
size_t pe_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);

/* argv joined with single spaces. An argument with a space or tab, or an
 * empty argument, is wrapped in double quotes, with embedded quotes escaped
 * as \". Returns units including the NUL. */
size_t pe_build_command_line(int argc, const char *const *argv, uint16_t *buf, size_t cap);

/* "NAME=VALUE" entries, each NUL-terminated, followed by one extra NUL. An
 * empty block is two NULs, so it is still double-NUL terminated. Returns units
 * including both final NULs. */
size_t pe_build_environment(int envc, const char *const *envp, uint16_t *buf, size_t cap);

#endif /* PE_TEXT_H */
