/*
 * ucrt_ctype.h - <ctype.h> and the wide classification functions for the "C" locale.
 *
 * Narrow classification covers 0 to 255 with the "C" locale's rules: only ASCII letters, digits, punctuation,
 * space and control characters classify; bytes above 0x7F are in no class. The table also serves the
 * runtime's __pctype_func (it is indexed from -1 so EOF is valid). Wide classification follows Unicode for the
 * common ranges: Latin-1 and Latin Extended-A letters with case, Greek, Cyrillic, general punctuation,
 * and the CJK and Hangul blocks as alphabetic. Host tests: tests/crt-host/test_ctype.c.
 */
#ifndef UCRT_CTYPE_H
#define UCRT_CTYPE_H

#include "ucrt_types.h"

/* The classification bits of the Microsoft runtime (the values _pctype holds). */
#define UCRT_UPPER 0x0001u
#define UCRT_LOWER 0x0002u
#define UCRT_DIGIT 0x0004u
#define UCRT_SPACE 0x0008u
#define UCRT_PUNCT 0x0010u
#define UCRT_CONTROL 0x0020u
#define UCRT_BLANK 0x0040u
#define UCRT_HEX 0x0080u
#define UCRT_LEADBYTE 0x8000u
#define UCRT_ALPHA 0x0103u /* a letter: _ALPHA includes UPPER|LOWER semantics through 0x100 */

/* The class bits of byte c in the "C" locale. */
static inline unsigned ucrt_ctype_class(int c)
{
    unsigned f = 0;
    if (c < 0 || c > 0x7F)
        return 0;
    if (c >= 'A' && c <= 'Z')
        f |= UCRT_UPPER | 0x100u;
    if (c >= 'a' && c <= 'z')
        f |= UCRT_LOWER | 0x100u;
    if (c >= '0' && c <= '9')
        f |= UCRT_DIGIT;
    if (c == ' ' || (c >= 9 && c <= 13))
        f |= UCRT_SPACE;
    if (c == ' ' || c == '\t')
        f |= UCRT_BLANK;
    if (c < 0x20 || c == 0x7F)
        f |= UCRT_CONTROL;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
        f |= UCRT_HEX;
    if (c > 0x20 && c < 0x7F && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
        f |= UCRT_PUNCT;
    return f;
}

/* The table behind __pctype_func: 257 entries indexed by c + 1 so that EOF (-1) is valid and 0. */
static unsigned short ucrt_pctype_storage[257];
static int ucrt_pctype_ready;

static inline const unsigned short *ucrt_pctype(void)
{
    if (!ucrt_pctype_ready) {
        for (int c = -1; c < 256; ++c)
            ucrt_pctype_storage[c + 1] = (unsigned short)ucrt_ctype_class(c);
        ucrt_pctype_ready = 1;
    }
    return ucrt_pctype_storage + 1; /* callers index it with -1 .. 255 */
}

static inline int ucrt_isalpha(int c) { return (ucrt_ctype_class(c) & (UCRT_UPPER | UCRT_LOWER)) != 0; }
static inline int ucrt_isupper(int c) { return (ucrt_ctype_class(c) & UCRT_UPPER) != 0; }
static inline int ucrt_islower(int c) { return (ucrt_ctype_class(c) & UCRT_LOWER) != 0; }
static inline int ucrt_isdigit(int c) { return (ucrt_ctype_class(c) & UCRT_DIGIT) != 0; }
static inline int ucrt_isxdigit(int c) { return (ucrt_ctype_class(c) & UCRT_HEX) != 0; }
static inline int ucrt_isspace(int c) { return (ucrt_ctype_class(c) & UCRT_SPACE) != 0; }
static inline int ucrt_ispunct(int c) { return (ucrt_ctype_class(c) & UCRT_PUNCT) != 0; }
static inline int ucrt_iscntrl(int c) { return (ucrt_ctype_class(c) & UCRT_CONTROL) != 0; }
static inline int ucrt_isblank(int c) { return (ucrt_ctype_class(c) & UCRT_BLANK) != 0; }
static inline int ucrt_isalnum(int c) { return (ucrt_ctype_class(c) & (UCRT_UPPER | UCRT_LOWER | UCRT_DIGIT)) != 0; }
static inline int ucrt_isgraph(int c) { return c > 0x20 && c < 0x7F; }
static inline int ucrt_isprint(int c) { return c >= 0x20 && c < 0x7F; }
static inline int ucrt_isascii(int c) { return (unsigned)c < 0x80u; }

static inline int ucrt_toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static inline int ucrt_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* ---- Wide characters -------------------------------------------------------------------------------- */

/* Simple one-to-one case mapping for Latin-1, Latin Extended-A, Greek and Cyrillic (the Unicode simple
 * mappings; the irregular letters of Extended-A are listed). Characters with no mapping return themselves. */
static inline ucrt_wchar ucrt_towupper(ucrt_wchar c)
{
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') ? (ucrt_wchar)(c - 0x20) : c;
    if (c == 0xB5)
        return 0x39C;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (ucrt_wchar)(c - 0x20);
    if (c == 0xFF)
        return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131)
            return 'I';
        if (c == 0x17F)
            return 'S';
        if (c == 0x130 || c == 0x138 || c == 0x149)
            return c;
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
            return (c & 1) ? (ucrt_wchar)(c - 1) : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? c : (ucrt_wchar)(c - 1);
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2)
        return (ucrt_wchar)(c - 0x20);
    if (c == 0x3C2)
        return 0x3A3;
    if (c >= 0x3AC && c <= 0x3AF)
        return c == 0x3AC ? 0x386 : (ucrt_wchar)(c - 0x25);
    if (c >= 0x430 && c <= 0x44F)
        return (ucrt_wchar)(c - 0x20);
    if (c >= 0x450 && c <= 0x45F)
        return (ucrt_wchar)(c - 0x50);
    return c;
}

static inline ucrt_wchar ucrt_towlower(ucrt_wchar c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') ? (ucrt_wchar)(c + 0x20) : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return (ucrt_wchar)(c + 0x20);
    if (c == 0x178)
        return 0xFF;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130)
            return 'i';
        if (c == 0x131 || c == 0x138 || c == 0x149)
            return c;
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
            return (c & 1) ? c : (ucrt_wchar)(c + 1);
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? (ucrt_wchar)(c + 1) : c;
        return c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return (ucrt_wchar)(c + 0x20);
    if (c == 0x386)
        return 0x3AC;
    if (c >= 0x388 && c <= 0x38A)
        return (ucrt_wchar)(c + 0x25);
    if (c >= 0x410 && c <= 0x42F)
        return (ucrt_wchar)(c + 0x20);
    if (c >= 0x400 && c <= 0x40F)
        return (ucrt_wchar)(c + 0x50);
    return c;
}

/* iswalpha: letters of the Latin, Greek, Cyrillic, Hebrew and Arabic blocks, and the CJK, kana and Hangul
 * blocks. Not exhaustive; the ranges are the ones real programs meet. */
static inline int ucrt_iswalpha(ucrt_wchar c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (c == 0xAA || c == 0xB5 || c == 0xBA)
        return 1;
    if (c >= 0xC0 && c <= 0x24F)
        return c != 0xD7 && c != 0xF7;
    if (c >= 0x370 && c <= 0x3FF)
        return c != 0x375 && c != 0x37E && c != 0x384 && c != 0x385 && c != 0x387;
    if (c >= 0x400 && c <= 0x481)
        return 1;
    if (c >= 0x48A && c <= 0x52F)
        return 1;
    if (c >= 0x5D0 && c <= 0x5EA)
        return 1;
    if (c >= 0x620 && c <= 0x64A)
        return 1;
    if (c >= 0x3041 && c <= 0x30FF)
        return c != 0x30A0 && c != 0x30FB;
    if (c >= 0x4E00 && c <= 0x9FFF)
        return 1;
    if (c >= 0xAC00 && c <= 0xD7A3)
        return 1;
    return 0;
}

static inline int ucrt_iswdigit(ucrt_wchar c) { return c >= '0' && c <= '9'; }
static inline int ucrt_iswxdigit(ucrt_wchar c) { return ucrt_isxdigit(c < 0x80 ? c : -1); }
static inline int ucrt_iswupper(ucrt_wchar c) { return ucrt_iswalpha(c) && ucrt_towlower(c) != c; }
static inline int ucrt_iswlower(ucrt_wchar c) { return ucrt_iswalpha(c) && ucrt_towupper(c) != c; }
static inline int ucrt_iswspace(ucrt_wchar c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static inline int ucrt_iswcntrl(ucrt_wchar c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029; }
static inline int ucrt_iswalnum(ucrt_wchar c) { return ucrt_iswalpha(c) || ucrt_iswdigit(c); }
static inline int ucrt_iswpunct(ucrt_wchar c)
{
    if (c < 0x80)
        return c > 0x20 && c < 0x7F && !ucrt_isalnum(c);
    return !ucrt_iswalpha(c) && !ucrt_iswspace(c) && !ucrt_iswcntrl(c) && !(c >= 0x660 && c <= 0x669);
}
static inline int ucrt_iswprint(ucrt_wchar c) { return !ucrt_iswcntrl(c); }
static inline int ucrt_iswgraph(ucrt_wchar c) { return ucrt_iswprint(c) && !ucrt_iswspace(c); }
static inline int ucrt_iswblank(ucrt_wchar c) { return c == ' ' || c == '\t' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x3000; }

#endif /* UCRT_CTYPE_H */
