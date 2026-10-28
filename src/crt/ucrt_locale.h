/*
 * ucrt_locale.h - locales and multibyte conversion: setlocale and localeconv for the "C" locale, the
 * English_United States.1252 locale, and UTF-8 locales; the locale-aware character class table behind isalpha and
 * friends; and mbrtowc, wcrtomb, mbstowcs, wcstombs with their _s forms for single-byte code pages and UTF-8.
 * Host tests: tests/crt-host/test_locale.c.
 */
#ifndef UCRT_LOCALE_H
#define UCRT_LOCALE_H

#include "ucrt_ctype.h"
#include "ucrt_state.h"
#include "ucrt_string.h"

#define UCRT_LC_ALL 0
#define UCRT_LC_COLLATE 1
#define UCRT_LC_CTYPE 2
#define UCRT_LC_MONETARY 3
#define UCRT_LC_NUMERIC 4
#define UCRT_LC_TIME 5

#define UCRT_CP_ACP_DEFAULT 1252
#define UCRT_CP_UTF8 65001

typedef struct ucrt_locale_cat {
    char name[96];
    int codepage; /* 0 for the "C" locale */
} ucrt_locale_cat;

typedef struct ucrt_locale_state {
    ucrt_locale_cat cat[6]; /* [0] is unused: LC_ALL is the set of the others */
    char all[6 * 110];
    int initialised;
    int mb_cur_max;
    int acp;     /* the system's ANSI code page, for converting things the OS hands over */
} ucrt_locale_state;

static ucrt_locale_state g_ucrt_loc;

/* Windows-1252: the characters at 0x80..0x9F that are not the same code point (0 where the byte is unassigned). */
static const unsigned short ucrt_cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

static inline void ucrt_loc_refresh_ctype(void);

static inline void ucrt_loc_init(void)
{
    if (g_ucrt_loc.initialised)
        return;
    g_ucrt_loc.initialised = 1;
    for (int i = 1; i < 6; ++i) {
        ucrt_strcpy(g_ucrt_loc.cat[i].name, "C");
        g_ucrt_loc.cat[i].codepage = 0;
    }
    g_ucrt_loc.mb_cur_max = 1;
    if (!g_ucrt_loc.acp)
        g_ucrt_loc.acp = UCRT_CP_ACP_DEFAULT;
}

static inline int ucrt_loc_codepage(void)
{
    ucrt_loc_init();
    return g_ucrt_loc.cat[UCRT_LC_CTYPE].codepage;
}

static inline int ucrt_loc_is_utf8(void)
{
    return ucrt_loc_codepage() == UCRT_CP_UTF8;
}

/* Case-insensitive comparison of ASCII strings. */
static inline int ucrt_loc_ieq(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* Turns a locale string into a canonical name and code page. Returns 0, or -1 for a locale that does not exist. */
static inline int ucrt_loc_resolve(const char *s, char *name, size_t cap, int *cp)
{
    const char *dot;
    char lang[64];
    size_t ll = 0;
    int codepage = -1;
    if (!s)
        return -1;
    if (ucrt_loc_ieq(s, "C")) {
        ucrt_strncpy(name, "C", cap);
        *cp = 0;
        return 0;
    }
    if (!*s) { /* the user's default */
        ucrt_strncpy(name, "English_United States.1252", cap);
        *cp = 1252;
        return 0;
    }
    dot = ucrt_strchr(s, '.');
    ll = dot ? (size_t)(dot - s) : ucrt_strlen(s);
    if (ll >= sizeof lang)
        return -1;
    ucrt_memcpy(lang, s, ll);
    lang[ll] = 0;
    if (dot) {
        const char *cs = dot + 1;
        if (ucrt_loc_ieq(cs, "UTF8") || ucrt_loc_ieq(cs, "UTF-8") || ucrt_loc_ieq(cs, "65001"))
            codepage = UCRT_CP_UTF8;
        else if (ucrt_loc_ieq(cs, "ACP") || ucrt_loc_ieq(cs, "OCP") || ucrt_loc_ieq(cs, "1252"))
            codepage = 1252;
        else if (ucrt_loc_ieq(cs, "437"))
            codepage = 437;
        else if (ucrt_loc_ieq(cs, "1250") || ucrt_loc_ieq(cs, "1251") || ucrt_loc_ieq(cs, "1253") || ucrt_loc_ieq(cs, "28591"))
            codepage = 1252; /* other single-byte pages are treated like Latin-1/1252 */
        else
            return -1;
    }
    if (ll == 0) { /* ".UTF8", ".1252" */
        ucrt_strncpy(name, codepage == UCRT_CP_UTF8 ? ".UTF8" : "English_United States.1252", cap);
        if (codepage == UCRT_CP_UTF8) {
            ucrt_strcpy(name, "English_United States.65001");
        }
        *cp = codepage < 0 ? 1252 : codepage;
        return 0;
    }
    if (ucrt_loc_ieq(lang, "English") || ucrt_loc_ieq(lang, "English_United States") || ucrt_loc_ieq(lang, "en-US") ||
        ucrt_loc_ieq(lang, "en_US") || ucrt_loc_ieq(lang, "american") || ucrt_loc_ieq(lang, "us") || ucrt_loc_ieq(lang, "usa") ||
        ucrt_loc_ieq(lang, "en") || ucrt_loc_ieq(lang, "english-us")) {
        int c = codepage < 0 ? 1252 : codepage;
        ucrt_strcpy(name, "English_United States.");
        {
            char num[12];
            int n = 0, v = c;
            char tmp[12];
            while (v) {
                tmp[n++] = (char)('0' + v % 10);
                v /= 10;
            }
            for (int i = 0; i < n; ++i)
                num[i] = tmp[n - 1 - i];
            num[n] = 0;
            ucrt_strcat(name, num);
        }
        *cp = c;
        return 0;
    }
    return -1;
}

static inline const char *ucrt_setlocale(int category, const char *locale)
{
    char name[96];
    int cp;
    ucrt_loc_init();
    if (category < 0 || category > 5) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    if (locale) {
        if (ucrt_loc_resolve(locale, name, sizeof name, &cp) != 0)
            return NULL;
        for (int c = 1; c < 6; ++c) {
            if (category == UCRT_LC_ALL || category == c) {
                ucrt_strcpy(g_ucrt_loc.cat[c].name, name);
                g_ucrt_loc.cat[c].codepage = cp;
            }
        }
        g_ucrt_loc.mb_cur_max = g_ucrt_loc.cat[UCRT_LC_CTYPE].codepage == UCRT_CP_UTF8 ? 4 : 1;
        ucrt_loc_refresh_ctype();
    }
    if (category != UCRT_LC_ALL)
        return g_ucrt_loc.cat[category].name;
    {
        int same = 1;
        for (int c = 2; c < 6; ++c)
            if (!ucrt_strcmp(g_ucrt_loc.cat[c].name, g_ucrt_loc.cat[1].name) == 0)
                same = 0;
        if (same)
            return g_ucrt_loc.cat[1].name;
        g_ucrt_loc.all[0] = 0;
        {
            static const char *const names[6] = {"", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME"};
            for (int c = 1; c < 6; ++c) {
                if (c > 1)
                    ucrt_strcat(g_ucrt_loc.all, ";");
                ucrt_strcat(g_ucrt_loc.all, names[c]);
                ucrt_strcat(g_ucrt_loc.all, "=");
                ucrt_strcat(g_ucrt_loc.all, g_ucrt_loc.cat[c].name);
            }
        }
        return g_ucrt_loc.all;
    }
}

/* ---- localeconv ------------------------------------------------------------------------------------------ */

typedef struct ucrt_lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep,
        *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    ucrt_wchar *_W_decimal_point, *_W_thousands_sep, *_W_int_curr_symbol, *_W_currency_symbol, *_W_mon_decimal_point, *_W_mon_thousands_sep,
        *_W_positive_sign, *_W_negative_sign;
} ucrt_lconv;

static inline ucrt_lconv *ucrt_localeconv(void)
{
    static char dot[] = ".", comma[] = ",", empty[] = "", group3[] = "\3", usd[] = "USD ", dollar[] = "$", minus[] = "-";
    static ucrt_wchar wdot[] = {'.', 0}, wcomma[] = {',', 0}, wempty[] = {0}, wusd[] = {'U', 'S', 'D', ' ', 0}, wdollar[] = {'$', 0},
                      wminus[] = {'-', 0};
    static ucrt_lconv c_lc, us_lc;
    static int init;
    if (!init) {
        init = 1;
        c_lc = (ucrt_lconv){dot, empty, empty, empty, empty, empty, empty, empty, empty, empty, 127, 127, 127, 127, 127, 127, 127, 127,
                            wdot, wempty, wempty, wempty, wempty, wempty, wempty, wempty};
        us_lc = (ucrt_lconv){dot, comma, group3, usd, dollar, dot, comma, group3, empty, minus, 2, 2, 1, 0, 1, 0, 3, 0,
                             wdot, wcomma, wusd, wdollar, wdot, wcomma, wempty, wminus};
    }
    ucrt_loc_init();
    /* numeric and monetary categories chosen separately would need their own copy; one structure is enough here */
    return ucrt_strcmp(g_ucrt_loc.cat[UCRT_LC_NUMERIC].name, "C") == 0 && ucrt_strcmp(g_ucrt_loc.cat[UCRT_LC_MONETARY].name, "C") == 0 ? &c_lc : &us_lc;
}

/* ---- Character classes by locale ---------------------------------------------------------------------------- */

/* The class bits of byte c in the current locale: the "C" locale's, or Windows-1252's letters above 0x7F. */
static inline unsigned ucrt_loc_class(int c)
{
    unsigned f = ucrt_ctype_class(c);
    if (c < 0x80 || c > 0xFF || ucrt_loc_codepage() != 1252)
        return f;
    if (c == 0xA0)
        return UCRT_SPACE | UCRT_BLANK;
    if (c == 0x8A || c == 0x8C || c == 0x8E || c == 0x9F || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return UCRT_UPPER | 0x100u;
    if (c == 0x9A || c == 0x9C || c == 0x9E || c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xDF && c <= 0xFF && c != 0xF7))
        return UCRT_LOWER | 0x100u;
    if (c == 0x81 || c == 0x8D || c == 0x8F || c == 0x90 || c == 0x9D)
        return UCRT_CONTROL;
    if (c >= 0x80 && c <= 0x9F)
        return UCRT_PUNCT;
    if (c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA)
        return UCRT_PUNCT;
    if (c == 0xD7 || c == 0xF7)
        return UCRT_PUNCT;
    return f;
}

static inline void ucrt_loc_refresh_ctype(void)
{
    for (int c = -1; c < 256; ++c)
        ucrt_pctype_storage[c + 1] = (unsigned short)ucrt_loc_class(c);
    ucrt_pctype_ready = 1;
}

static inline int ucrt_loc_isalpha(int c) { return (ucrt_loc_class(c) & (UCRT_UPPER | UCRT_LOWER)) != 0; }
static inline int ucrt_loc_isupper(int c) { return (ucrt_loc_class(c) & UCRT_UPPER) != 0; }
static inline int ucrt_loc_islower(int c) { return (ucrt_loc_class(c) & UCRT_LOWER) != 0; }
static inline int ucrt_loc_isalnum(int c) { return (ucrt_loc_class(c) & (UCRT_UPPER | UCRT_LOWER | UCRT_DIGIT)) != 0; }
static inline int ucrt_loc_isspace(int c) { return (ucrt_loc_class(c) & UCRT_SPACE) != 0; }
static inline int ucrt_loc_ispunct(int c) { return (ucrt_loc_class(c) & UCRT_PUNCT) != 0; }
static inline int ucrt_loc_iscntrl(int c) { return (ucrt_loc_class(c) & UCRT_CONTROL) != 0; }
static inline int ucrt_loc_isprint(int c) { return (ucrt_loc_class(c) & (UCRT_UPPER | UCRT_LOWER | UCRT_DIGIT | UCRT_PUNCT | UCRT_BLANK)) != 0 || c == ' '; }
static inline int ucrt_loc_isgraph(int c) { return (ucrt_loc_class(c) & (UCRT_UPPER | UCRT_LOWER | UCRT_DIGIT | UCRT_PUNCT)) != 0; }

static inline int ucrt_loc_toupper(int c)
{
    if (ucrt_loc_codepage() == 1252 && c >= 0x80 && c <= 0xFF) {
        if (c == 0x9A || c == 0x9C || c == 0x9E)
            return c - 0x10;
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
            return c - 0x20;
        if (c == 0xFF)
            return 0x9F;
        return c;
    }
    return ucrt_toupper(c);
}

static inline int ucrt_loc_tolower(int c)
{
    if (ucrt_loc_codepage() == 1252 && c >= 0x80 && c <= 0xFF) {
        if (c == 0x8A || c == 0x8C || c == 0x8E)
            return c + 0x10;
        if (c == 0x9F)
            return 0xFF;
        if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
            return c + 0x20;
        return c;
    }
    return ucrt_tolower(c);
}

/* ---- Multibyte conversion ----------------------------------------------------------------------------------- */

typedef struct ucrt_mbstate {
    uint32_t acc;      /* the code point being assembled from a UTF-8 sequence */
    int8_t remaining;  /* continuation bytes still expected */
    int8_t total;      /* the length of the sequence being read */
    uint16_t low;      /* a low surrogate still to be delivered by the next mbrtowc, or 0 */
    uint16_t high;     /* a high surrogate waiting for its low half in wcrtomb */
} ucrt_mbstate;

#define UCRT_MB_ERR ((size_t)-1)
#define UCRT_MB_INCOMPLETE ((size_t)-2)
#define UCRT_MB_NEXT ((size_t)-3)

static inline int ucrt_mbsinit(const ucrt_mbstate *st)
{
    return !st || (st->remaining == 0 && st->low == 0 && st->high == 0);
}

/* Converts the next character of s (at most n bytes). Returns the bytes consumed (0 for the NUL character), or one of
 * UCRT_MB_ERR (errno is EILSEQ), UCRT_MB_INCOMPLETE, UCRT_MB_NEXT (the second half of a surrogate pair, no input used). */
static inline size_t ucrt_mbrtowc_st(ucrt_wchar *pwc, const char *s, size_t n, ucrt_mbstate *st)
{
    size_t used = 0;
    if (!s) { /* reset */
        __builtin_memset(st, 0, sizeof *st);
        return 0;
    }
    if (st->low) {
        if (pwc)
            *pwc = st->low;
        st->low = 0;
        return UCRT_MB_NEXT;
    }
    if (n == 0)
        return UCRT_MB_INCOMPLETE;
    if (!ucrt_loc_is_utf8()) {
        unsigned char b = (unsigned char)*s;
        ucrt_wchar w = b;
        if (ucrt_loc_codepage() == 1252 && b >= 0x80 && b <= 0x9F)
            w = ucrt_cp1252_high[b - 0x80];
        if (pwc)
            *pwc = w;
        return b ? 1 : 0;
    }
    while (used < n) {
        unsigned char b = (unsigned char)s[used];
        if (st->remaining == 0) {
            if (b < 0x80) {
                if (pwc)
                    *pwc = b;
                ++used;
                return b ? used : 0;
            }
            if (b >= 0xC2 && b <= 0xDF) {
                st->acc = b & 0x1F;
                st->total = 2;
            } else if (b >= 0xE0 && b <= 0xEF) {
                st->acc = b & 0x0F;
                st->total = 3;
            } else if (b >= 0xF0 && b <= 0xF4) {
                st->acc = b & 0x07;
                st->total = 4;
            } else {
                __builtin_memset(st, 0, sizeof *st);
                ucrt_set_errno(UCRT_EILSEQ);
                return UCRT_MB_ERR;
            }
            st->remaining = (int8_t)(st->total - 1);
            ++used;
            continue;
        }
        if ((b & 0xC0) != 0x80) {
            __builtin_memset(st, 0, sizeof *st);
            ucrt_set_errno(UCRT_EILSEQ);
            return UCRT_MB_ERR;
        }
        st->acc = (st->acc << 6) | (b & 0x3F);
        --st->remaining;
        ++used;
        if (st->remaining == 0) {
            uint32_t cp = st->acc;
            int total = st->total;
            __builtin_memset(st, 0, sizeof *st);
            /* reject overlong forms, surrogates and values beyond U+10FFFF */
            if ((total == 3 && cp < 0x800) || (total == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
                ucrt_set_errno(UCRT_EILSEQ);
                return UCRT_MB_ERR;
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                if (pwc)
                    *pwc = (ucrt_wchar)(0xD800 + (cp >> 10));
                st->low = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else if (pwc) {
                *pwc = (ucrt_wchar)cp;
            }
            return used;
        }
    }
    return UCRT_MB_INCOMPLETE;
}

/* Writes the multibyte form of wc to s (at least 4 bytes). Returns the bytes written, or UCRT_MB_ERR (EILSEQ). For UTF-8 a
 * high surrogate writes nothing and waits for the low one. */
static inline size_t ucrt_wcrtomb_st(char *s, ucrt_wchar wc, ucrt_mbstate *st)
{
    if (!s) {
        __builtin_memset(st, 0, sizeof *st);
        return 1;
    }
    if (!ucrt_loc_is_utf8()) {
        if (wc < 0x80 || (wc <= 0xFF && ucrt_loc_codepage() == 0) || (wc <= 0xFF && !(wc >= 0x80 && wc <= 0x9F) && ucrt_loc_codepage() == 1252)) {
            if (wc > 0xFF) {
                ucrt_set_errno(UCRT_EILSEQ);
                return UCRT_MB_ERR;
            }
            *s = (char)wc;
            return 1;
        }
        if (ucrt_loc_codepage() == 1252) {
            for (int i = 0; i < 32; ++i)
                if (ucrt_cp1252_high[i] == wc) {
                    *s = (char)(0x80 + i);
                    return 1;
                }
        }
        ucrt_set_errno(UCRT_EILSEQ);
        return UCRT_MB_ERR;
    }
    {
        uint32_t cp = wc;
        if (st->high) {
            if (wc >= 0xDC00 && wc <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)st->high - 0xD800) << 10) + (wc - 0xDC00);
                st->high = 0;
            } else {
                st->high = 0;
                ucrt_set_errno(UCRT_EILSEQ);
                return UCRT_MB_ERR;
            }
        } else if (wc >= 0xD800 && wc <= 0xDBFF) {
            st->high = wc;
            return 0;
        } else if (wc >= 0xDC00 && wc <= 0xDFFF) {
            ucrt_set_errno(UCRT_EILSEQ);
            return UCRT_MB_ERR;
        }
        if (cp < 0x80) {
            s[0] = (char)cp;
            return 1;
        }
        if (cp < 0x800) {
            s[0] = (char)(0xC0 | (cp >> 6));
            s[1] = (char)(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp < 0x10000) {
            s[0] = (char)(0xE0 | (cp >> 12));
            s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[2] = (char)(0x80 | (cp & 0x3F));
            return 3;
        }
        s[0] = (char)(0xF0 | (cp >> 18));
        s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static ucrt_mbstate g_ucrt_mbrtowc_state, g_ucrt_wcrtomb_state, g_ucrt_mbrlen_state;

static inline size_t ucrt_mbrtowc(ucrt_wchar *pwc, const char *s, size_t n, ucrt_mbstate *st)
{
    ucrt_loc_init();
    return ucrt_mbrtowc_st(pwc, s, n, st ? st : &g_ucrt_mbrtowc_state);
}

static inline size_t ucrt_wcrtomb(char *s, ucrt_wchar wc, ucrt_mbstate *st)
{
    ucrt_loc_init();
    return ucrt_wcrtomb_st(s, wc, st ? st : &g_ucrt_wcrtomb_state);
}

static inline size_t ucrt_mbrlen(const char *s, size_t n, ucrt_mbstate *st)
{
    return ucrt_mbrtowc(NULL, s, n, st ? st : &g_ucrt_mbrlen_state);
}

/* mbtowc and wctomb: stateless on Windows; a character outside the basic plane cannot be produced as one wchar_t. */
static inline int ucrt_mbtowc(ucrt_wchar *pwc, const char *s, size_t n)
{
    ucrt_mbstate st;
    size_t r;
    ucrt_loc_init();
    if (!s)
        return 0; /* no state-dependent encodings */
    __builtin_memset(&st, 0, sizeof st);
    r = ucrt_mbrtowc_st(pwc, s, n, &st);
    if (r == UCRT_MB_ERR || r == UCRT_MB_INCOMPLETE) {
        ucrt_set_errno(UCRT_EILSEQ);
        return -1;
    }
    if (st.low) { /* a supplementary character: it needs two wchar_t */
        ucrt_set_errno(UCRT_EILSEQ);
        return -1;
    }
    return (int)r;
}

static inline int ucrt_mblen(const char *s, size_t n)
{
    return ucrt_mbtowc(NULL, s, n);
}

static inline int ucrt_wctomb(char *s, ucrt_wchar wc)
{
    ucrt_mbstate st;
    size_t r;
    ucrt_loc_init();
    if (!s)
        return 0;
    __builtin_memset(&st, 0, sizeof st);
    r = ucrt_wcrtomb_st(s, wc, &st);
    if (r == UCRT_MB_ERR || (r == 0 && st.high)) {
        ucrt_set_errno(UCRT_EILSEQ);
        return -1;
    }
    return (int)r;
}

static inline ucrt_errno_t ucrt_wctomb_s(int *ret, char *s, size_t size, ucrt_wchar wc)
{
    char tmp[4];
    int n;
    if (!s && size) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (!s) {
        if (ret)
            *ret = 0;
        return 0;
    }
    n = ucrt_wctomb(tmp, wc);
    if (n < 0) {
        if (ret)
            *ret = -1;
        if (size)
            s[0] = 0;
        return UCRT_EILSEQ;
    }
    if ((size_t)n > size) {
        if (ret)
            *ret = -1;
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memcpy(s, tmp, (size_t)n);
    if (ret)
        *ret = n;
    return 0;
}

static inline int ucrt_btowc(int c)
{
    ucrt_wchar w;
    ucrt_mbstate st;
    char b;
    if (c == UCRT_EOF)
        return UCRT_WEOF;
    b = (char)c;
    __builtin_memset(&st, 0, sizeof st);
    if (ucrt_loc_is_utf8() && (unsigned char)b >= 0x80)
        return UCRT_WEOF;
    if (ucrt_mbrtowc_st(&w, &b, 1, &st) == UCRT_MB_ERR)
        return UCRT_WEOF;
    return w;
}

static inline int ucrt_wctob(ucrt_wchar wc)
{
    char b[4];
    ucrt_mbstate st;
    size_t r;
    __builtin_memset(&st, 0, sizeof st);
    r = ucrt_wcrtomb_st(b, wc, &st);
    if (r != 1)
        return UCRT_EOF;
    return (unsigned char)b[0];
}

/* mbstowcs: dst may be NULL to count. Stops at a NUL or when count wide characters were stored. */
static inline size_t ucrt_mbstowcs(ucrt_wchar *dst, const char *src, size_t count)
{
    ucrt_mbstate st;
    size_t n = 0;
    __builtin_memset(&st, 0, sizeof st);
    ucrt_loc_init();
    if (!src) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_MB_ERR;
    }
    for (;;) {
        ucrt_wchar w = 0;
        size_t r;
        if (dst && n >= count)
            break;
        r = ucrt_mbrtowc_st(&w, src, 4, &st);
        if (r == UCRT_MB_ERR)
            return UCRT_MB_ERR;
        if (r == UCRT_MB_INCOMPLETE) { /* the string ends inside a character */
            ucrt_set_errno(UCRT_EILSEQ);
            return UCRT_MB_ERR;
        }
        if (r == 0 && w == 0 && !st.low) {
            if (dst && n < count)
                dst[n] = 0;
            break;
        }
        if (dst)
            dst[n] = w;
        ++n;
        if (r != UCRT_MB_NEXT)
            src += r;
    }
    return n;
}

/* wcstombs: dst may be NULL to count; a character that does not fit whole is left out. */
static inline size_t ucrt_wcstombs(char *dst, const ucrt_wchar *src, size_t count)
{
    ucrt_mbstate st;
    size_t n = 0;
    __builtin_memset(&st, 0, sizeof st);
    ucrt_loc_init();
    if (!src) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_MB_ERR;
    }
    for (;;) {
        char tmp[4];
        size_t r;
        if (*src == 0) {
            if (st.high) {
                ucrt_set_errno(UCRT_EILSEQ);
                return UCRT_MB_ERR;
            }
            if (dst && n < count)
                dst[n] = 0;
            break;
        }
        r = ucrt_wcrtomb_st(tmp, *src, &st);
        if (r == UCRT_MB_ERR)
            return UCRT_MB_ERR;
        if (dst) {
            if (n + r > count)
                break;
            ucrt_memcpy(dst + n, tmp, r);
        }
        n += r;
        ++src;
    }
    return n;
}

#define UCRT_TRUNCATE_ ((size_t)-1)

/* mbstowcs_s: the secure form. count is the most wide characters to store, not counting the NUL, or _TRUNCATE; *ret
 * receives the number stored including the NUL. A destination that is too small is an error, except with _TRUNCATE. */
static inline ucrt_errno_t ucrt_mbstowcs_s(size_t *ret, ucrt_wchar *dst, size_t size, const char *src, size_t count)
{
    size_t total, want, n;
    int truncated = 0;
    if (ret)
        *ret = 0;
    if ((!dst && size) || (dst && !size) || !src) {
        if (dst && size)
            dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    total = ucrt_mbstowcs(NULL, src, 0);
    if (total == UCRT_MB_ERR) {
        if (dst)
            dst[0] = 0;
        return ucrt_get_errno();
    }
    if (!dst) {
        if (ret)
            *ret = total + 1;
        return 0;
    }
    want = count == UCRT_TRUNCATE_ || total < count ? total : count;
    if (want >= size) {
        if (count != UCRT_TRUNCATE_) {
            dst[0] = 0;
            return ucrt_invalid(UCRT_ERANGE);
        }
        want = size - 1;
        truncated = 1;
    }
    n = ucrt_mbstowcs(dst, src, want);
    dst[n < want ? n : want] = 0;
    if (ret)
        *ret = (n < want ? n : want) + 1;
    return truncated ? UCRT_STRUNCATE : 0;
}

static inline ucrt_errno_t ucrt_wcstombs_s(size_t *ret, char *dst, size_t size, const ucrt_wchar *src, size_t count)
{
    size_t total, want, n;
    int truncated = 0;
    if (ret)
        *ret = 0;
    if ((!dst && size) || (dst && !size) || !src) {
        if (dst && size)
            dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    total = ucrt_wcstombs(NULL, src, 0);
    if (total == UCRT_MB_ERR) {
        if (dst)
            dst[0] = 0;
        return ucrt_get_errno();
    }
    if (!dst) {
        if (ret)
            *ret = total + 1;
        return 0;
    }
    want = count == UCRT_TRUNCATE_ || total < count ? total : count;
    if (want >= size) {
        if (count != UCRT_TRUNCATE_) {
            dst[0] = 0;
            return ucrt_invalid(UCRT_ERANGE);
        }
        want = size - 1;
        truncated = 1;
    }
    n = ucrt_wcstombs(dst, src, want); /* never splits a character */
    dst[n] = 0;
    if (ret)
        *ret = n + 1;
    return truncated ? UCRT_STRUNCATE : 0;
}

/* _mbstrlen: the number of multibyte characters, or (size_t)-1 for an invalid sequence. */
static inline size_t ucrt_mbstrlen(const char *s)
{
    size_t n = 0;
    ucrt_mbstate st;
    __builtin_memset(&st, 0, sizeof st);
    ucrt_loc_init();
    for (;;) {
        ucrt_wchar w;
        size_t r = ucrt_mbrtowc_st(&w, s, 4, &st);
        if (r == UCRT_MB_ERR || r == UCRT_MB_INCOMPLETE) {
            ucrt_set_errno(UCRT_EILSEQ);
            return UCRT_MB_ERR;
        }
        if (r == 0)
            return n;
        if (r == UCRT_MB_NEXT) /* the second half of a surrogate pair is the same character */
            continue;
        s += r;
        ++n;
    }
}

#endif /* UCRT_LOCALE_H */
