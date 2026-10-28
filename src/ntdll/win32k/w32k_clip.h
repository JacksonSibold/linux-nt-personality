/* w32k_clip.h - text conversions for synthesised clipboard formats (see w32k_clip.c). */
#ifndef W32K_CLIP_H
#define W32K_CLIP_H

#include <stddef.h>
#include <stdint.h>

/* Standard clipboard formats (CF_*) used by the model. */
#define W32K_CF_TEXT 1u
#define W32K_CF_BITMAP 2u
#define W32K_CF_OEMTEXT 7u
#define W32K_CF_DIB 8u
#define W32K_CF_UNICODETEXT 13u
#define W32K_CF_LOCALE 16u
#define W32K_CF_DIBV5 17u
/* Registered formats start here (RegisterClipboardFormat). */
#define W32K_CF_REGISTER_BASE 0xC000u

/* One UTF-16 unit from a 1252 byte. Undefined bytes map to U+FFFD. */
uint16_t w32k_clip_cp1252_to_wide(uint8_t b);

/* The 1252 byte for a UTF-16 unit, or -1 when the unit has no 1252 byte. */
int w32k_clip_wide_to_cp1252(uint16_t u);

/* CF_TEXT bytes (up to the first NUL, or n) to a NUL-terminated UTF-16 buffer of cap
 * units. Returns the number of units written including the NUL, or 0 when cap is too small. */
size_t w32k_clip_text_to_wide(const uint8_t *src, size_t n, uint16_t *dst, size_t cap);

/* NUL-terminated UTF-16 (up to units, or the first NUL) to CF_TEXT bytes (oem = 0) or
 * CF_OEMTEXT bytes (oem != 0). Returns the number of bytes written including the NUL,
 * or 0 when cap is too small. */
size_t w32k_clip_wide_to_text(const uint16_t *src, size_t units, uint8_t *dst, size_t cap, int oem);

/* Length of a UTF-16 string in units, not counting the NUL, bounded by units. */
size_t w32k_clip_wide_units(const uint16_t *src, size_t units);

#endif /* W32K_CLIP_H */
