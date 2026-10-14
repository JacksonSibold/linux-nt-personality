/*
 * k32_str_pure.h - pure code page conversion for MultiByteToWideChar and
 * WideCharToMultiByte (kernel32_str.c).
 *
 * Code pages:
 *   CP_UTF8 (65001)  UTF-8 both ways, with surrogate pairs.
 *   CP_ACP  (0)      Windows-1252, the default ANSI page on the target.
 *   CP_OEMCP (1)     code page 437 for ASCII only. A byte above 0x7F is
 *                    ERROR_NO_UNICODE_TRANSLATION; the 437 high table is not
 *                    built yet.
 *   other pages      ERROR_INVALID_PARAMETER.
 *
 * Errors: 0 success; ERROR_INVALID_PARAMETER 87; ERROR_INSUFFICIENT_BUFFER
 * 122 when the output does not fit; ERROR_NO_UNICODE_TRANSLATION 1113 for an
 * invalid sequence when the MB_ERR_INVALID_CHARS or WC_ERR_INVALID_CHARS flag
 * is set. Without those flags an invalid sequence becomes U+FFFD (or the
 * default character when writing a narrow string).
 */
#ifndef NTDLL_K32_STR_PURE_H
#define NTDLL_K32_STR_PURE_H

#include "nt_types.h"

#define K32STR_CP_ACP    0u
#define K32STR_CP_OEMCP  1u
#define K32STR_CP_437    437u
#define K32STR_CP_1252   1252u
#define K32STR_CP_UTF8   65001u

#define K32STR_MB_ERR_INVALID_CHARS 0x00000008u
#define K32STR_WC_ERR_INVALID_CHARS 0x00000080u

#define K32STR_ERROR_INVALID_PARAMETER         87
#define K32STR_ERROR_INSUFFICIENT_BUFFER       122
#define K32STR_ERROR_NO_UNICODE_TRANSLATION    1113

#define K32STR_REPLACEMENT 0xFFFDu

/* Windows-1252 0x80..0x9F. The entries 0x81, 0x8D, 0x8F, 0x90 and 0x9D are
 * the C1 controls, which the code page maps to themselves. */
static const unsigned short k32str_cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/* The code page after the CP_ACP and CP_OEMCP aliases, or 0 if unsupported. */
static inline unsigned int k32str_resolve_cp(unsigned int cp)
{
    if (cp == K32STR_CP_ACP) return K32STR_CP_1252;
    if (cp == K32STR_CP_OEMCP) return K32STR_CP_437;
    if (cp == K32STR_CP_1252 || cp == K32STR_CP_437 || cp == K32STR_CP_UTF8) return cp;
    return 0;
}

/* One UTF-8 sequence at s (n bytes left). On success returns its length (1 to
 * 4) and stores the code point. On an invalid sequence returns 0. Overlong
 * forms, surrogates (U+D800..U+DFFF) and values above U+10FFFF are invalid. */
static inline size_t k32str_utf8_decode(const unsigned char *s, size_t n, unsigned long *cp)
{
    unsigned char c = s[0];
    if (c < 0x80u) {
        *cp = c;
        return 1;
    }
    size_t len;
    unsigned long v;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2u && c <= 0xDFu) { len = 2; v = c & 0x1Fu; }
    else if (c >= 0xE0u && c <= 0xEFu) {
        len = 3; v = c & 0x0Fu;
        if (c == 0xE0u) lo = 0xA0u;
        if (c == 0xEDu) hi = 0x9Fu;
    }
    else if (c >= 0xF0u && c <= 0xF4u) {
        len = 4; v = c & 0x07u;
        if (c == 0xF0u) lo = 0x90u;
        if (c == 0xF4u) hi = 0x8Fu;
    }
    else return 0;
    if (n < len) return 0;
    if (s[1] < lo || s[1] > hi) return 0;
    v = (v << 6) | (s[1] & 0x3Fu);
    for (size_t i = 2; i < len; ++i) {
        if (s[i] < 0x80u || s[i] > 0xBFu) return 0;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    *cp = v;
    return len;
}

/* Writes a code point as UTF-8 into out (4 bytes). Returns the length. */
static inline size_t k32str_utf8_encode(unsigned long cp, unsigned char *out)
{
    if (cp < 0x80u) { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (unsigned char)(0xC0u | (cp >> 6));
        out[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (unsigned char)(0xE0u | (cp >> 12));
        out[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (unsigned char)(0xF0u | (cp >> 18));
    out[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* The wide character count of a code point: 1, or 2 for a surrogate pair. */
static inline size_t k32str_wide_units(unsigned long cp)
{
    return cp >= 0x10000u ? 2u : 1u;
}

/* Multibyte to wide. n is the input byte count (the caller resolves -1). dst
 * NULL or cap 0 counts only. *need is the number of wide characters. */
static inline int k32str_mb_to_wc(unsigned int cp_in, unsigned long flags,
                                  const unsigned char *src, size_t n,
                                  WCHAR *dst, size_t cap, size_t *need)
{
    unsigned int cp = k32str_resolve_cp(cp_in);
    if (!cp) return K32STR_ERROR_INVALID_PARAMETER;
    int write = dst != NULL && cap > 0;
    size_t out = 0;
    size_t i = 0;
    while (i < n) {
        unsigned long v;
        size_t used = 1;
        if (cp == K32STR_CP_UTF8) {
            used = k32str_utf8_decode(src + i, n - i, &v);
            if (used == 0) {
                if (flags & K32STR_MB_ERR_INVALID_CHARS) return K32STR_ERROR_NO_UNICODE_TRANSLATION;
                v = K32STR_REPLACEMENT;
                used = 1;
            }
        } else {
            unsigned char b = src[i];
            if (b < 0x80u) {
                v = b;
            } else if (cp == K32STR_CP_1252) {
                v = (b >= 0xA0u) ? b : k32str_cp1252_high[b - 0x80u];
            } else {
                /* CP 437 high bytes are not built yet. */
                if (flags & K32STR_MB_ERR_INVALID_CHARS) return K32STR_ERROR_NO_UNICODE_TRANSLATION;
                v = K32STR_REPLACEMENT;
            }
        }
        size_t units = k32str_wide_units(v);
        if (write) {
            if (out + units > cap) {
                *need = out + units;
                return K32STR_ERROR_INSUFFICIENT_BUFFER;
            }
            if (units == 1) {
                dst[out] = (WCHAR)v;
            } else {
                unsigned long u = v - 0x10000u;
                dst[out] = (WCHAR)(0xD800u + (u >> 10));
                dst[out + 1] = (WCHAR)(0xDC00u + (u & 0x3FFu));
            }
        }
        out += units;
        i += used;
    }
    *need = out;
    return 0;
}

/* Wide to multibyte. n is the wide character count (the caller resolves -1).
 * defch is the byte for a character with no mapping (the caller's default, or
 * '?'). *used_default is set when defch was written. */
static inline int k32str_wc_to_mb(unsigned int cp_in, unsigned long flags,
                                  const WCHAR *src, size_t n,
                                  unsigned char *dst, size_t cap, unsigned char defch,
                                  size_t *need, int *used_default)
{
    unsigned int cp = k32str_resolve_cp(cp_in);
    if (!cp) return K32STR_ERROR_INVALID_PARAMETER;
    int write = dst != NULL && cap > 0;
    size_t out = 0;
    int used = 0;
    size_t i = 0;
    while (i < n) {
        unsigned long v = src[i];
        size_t consumed = 1;
        if (v >= 0xD800u && v <= 0xDBFFu && i + 1 < n && src[i + 1] >= 0xDC00u && src[i + 1] <= 0xDFFFu) {
            v = 0x10000u + ((v - 0xD800u) << 10) + ((unsigned long)src[i + 1] - 0xDC00u);
            consumed = 2;
        } else if (v >= 0xD800u && v <= 0xDFFFu) {
            /* A lone surrogate has no encoding. */
            if (cp == K32STR_CP_UTF8) {
                if (flags & K32STR_WC_ERR_INVALID_CHARS) return K32STR_ERROR_NO_UNICODE_TRANSLATION;
                v = K32STR_REPLACEMENT;
            } else {
                v = 0xFFFFFFFFu;   /* no mapping */
            }
        }
        unsigned char buf[4];
        size_t len = 0;
        if (cp == K32STR_CP_UTF8) {
            len = k32str_utf8_encode(v, buf);
        } else if (v == 0xFFFFFFFFu) {
            buf[0] = defch;
            len = 1;
            used = 1;
        } else if (v < 0x80u || (v >= 0xA0u && v <= 0xFFu)) {
            buf[0] = (unsigned char)v;
            len = 1;
        } else if (cp == K32STR_CP_1252) {
            int found = 0;
            for (unsigned int j = 0; j < 32; ++j) {
                if (k32str_cp1252_high[j] == v) {
                    buf[0] = (unsigned char)(0x80u + j);
                    found = 1;
                    break;
                }
            }
            if (found) {
                len = 1;
            } else {
                buf[0] = defch;
                len = 1;
                used = 1;
            }
        } else {
            /* CP 437 characters above ASCII are not built yet. */
            buf[0] = defch;
            len = 1;
            used = 1;
        }
        if (write) {
            if (out + len > cap) {
                *need = out + len;
                *used_default = used;
                return K32STR_ERROR_INSUFFICIENT_BUFFER;
            }
            for (size_t k = 0; k < len; ++k)
                dst[out + k] = buf[k];
        }
        out += len;
        i += consumed;
    }
    *need = out;
    *used_default = used;
    return 0;
}

#endif /* NTDLL_K32_STR_PURE_H */
