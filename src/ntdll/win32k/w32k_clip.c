/* w32k_clip.c - text conversions behind the clipboard's synthesised formats.
 *
 * The clipboard holds what each application put there. When an application asks
 * for a format that was not stored, Win32 converts from a stored text format:
 * CF_TEXT and CF_OEMTEXT are synthesised from CF_UNICODETEXT, and CF_UNICODETEXT
 * from CF_TEXT. The conversions here are pure functions over byte and UTF-16
 * buffers, so the rules can be tested without a station.
 *
 * Code page. CF_TEXT is treated as Windows-1252 (the ANSI code page of the
 * Western locales this model targets). Bytes 0x80..0x9F map through the 1252
 * table; the five undefined bytes (0x81, 0x8D, 0x8F, 0x90, 0x9D) map to U+FFFD.
 * Code points with no 1252 byte become '?', as WideCharToMultiByte does with the
 * default character. CF_OEMTEXT is treated as ASCII: any byte above 0x7F in a
 * synthesised OEM buffer is '?'. This is a restriction, not the OEM 437 table.
 *
 * Surrogates. A valid high/low UTF-16 pair is one character and becomes one '?'.
 * A lone surrogate is one '?' as well.
 *
 * Terminators. The text formats are NUL-terminated. A conversion stops at the first
 * NUL in its input (or at the input length when there is none) and always writes the
 * terminator to the output, which must have room for it.
 */
#include "w32k_clip.h"

static const uint16_t cp1252_high[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

uint16_t w32k_clip_cp1252_to_wide(uint8_t b)
{
    if (b >= 0x80 && b <= 0x9F)
        return cp1252_high[b - 0x80];
    return (uint16_t)b;
}

int w32k_clip_wide_to_cp1252(uint16_t u)
{
    if (u < 0x80 || (u >= 0xA0 && u <= 0xFF))
        return (int)u;
    for (unsigned i = 0; i < 32; ++i) {
        if (cp1252_high[i] == u && u != 0xFFFD)
            return (int)(0x80 + i);
    }
    return -1;
}

static int is_high_surrogate(uint16_t u)
{
    return u >= 0xD800 && u <= 0xDBFF;
}

static int is_low_surrogate(uint16_t u)
{
    return u >= 0xDC00 && u <= 0xDFFF;
}

size_t w32k_clip_text_to_wide(const uint8_t *src, size_t n, uint16_t *dst, size_t cap)
{
    size_t out = 0;
    if (!src || !dst)
        return 0;
    for (size_t i = 0; i < n && src[i] != 0; ++i) {
        if (out + 1 >= cap)
            return 0;
        dst[out++] = w32k_clip_cp1252_to_wide(src[i]);
    }
    if (out + 1 > cap)
        return 0;
    dst[out] = 0;
    return out + 1;
}

size_t w32k_clip_wide_to_text(const uint16_t *src, size_t units, uint8_t *dst, size_t cap, int oem)
{
    size_t out = 0;
    if (!src || !dst)
        return 0;
    for (size_t i = 0; i < units && src[i] != 0; ++i) {
        uint16_t u = src[i];
        int b;
        if (is_high_surrogate(u) && i + 1 < units && is_low_surrogate(src[i + 1]))
            ++i; /* a pair is one character */
        if (is_high_surrogate(u) || is_low_surrogate(u)) {
            b = '?';
        } else if (oem) {
            b = u < 0x80 ? (int)u : '?';
        } else {
            b = w32k_clip_wide_to_cp1252(u);
            if (b < 0)
                b = '?';
        }
        if (out + 1 >= cap)
            return 0;
        dst[out++] = (uint8_t)b;
    }
    if (out + 1 > cap)
        return 0;
    dst[out] = 0;
    return out + 1;
}

size_t w32k_clip_wide_units(const uint16_t *src, size_t units)
{
    size_t i = 0;
    while (i < units && src[i] != 0)
        ++i;
    return i;
}
