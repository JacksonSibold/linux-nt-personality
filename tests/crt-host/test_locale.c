/*
 * test_locale.c - tests for src/crt/ucrt_locale.h: setlocale and the locale-aware character classes, multibyte
 * conversion for the C locale, Windows-1252 and UTF-8 (UTF-8 validation compared with glibc's mbrtowc).
 */
#include "minitest.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ucrt_locale.h"

#define MT_FAIL(...) do { char fb_[400]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static uint64_t rng = 0xE7037ED1A0B428DBull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void reset_locale(void)
{
    g_ucrt_loc.initialised = 0;
    ucrt_setlocale(UCRT_LC_ALL, "C");
}

static void setlocale_names(void)
{
    reset_locale();
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, NULL), "C") == 0);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, ""), "English_United States.1252") == 0);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, "en-US"), "English_United States.1252") == 0);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, ".UTF8"), "English_United States.65001") == 0);
    MT_EXPECT_EQ(g_ucrt_loc.mb_cur_max, 4);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, "C"), "C") == 0);
    MT_EXPECT_EQ(g_ucrt_loc.mb_cur_max, 1);
    MT_EXPECT(ucrt_setlocale(UCRT_LC_ALL, "xx_YY.nonsense") == NULL);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, NULL), "C") == 0); /* a failed call changes nothing */
    MT_EXPECT(ucrt_setlocale(99, "C") == NULL);
    /* one category at a time, and the composite name */
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_CTYPE, "English_United States.1252"), "English_United States.1252") == 0);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_NUMERIC, NULL), "C") == 0);
    MT_EXPECT(strcmp(ucrt_setlocale(UCRT_LC_ALL, NULL),
                     "LC_COLLATE=C;LC_CTYPE=English_United States.1252;LC_MONETARY=C;LC_NUMERIC=C;LC_TIME=C") == 0);
    reset_locale();
}

static void localeconv_values(void)
{
    ucrt_lconv *lc;
    reset_locale();
    lc = ucrt_localeconv();
    MT_EXPECT(strcmp(lc->decimal_point, ".") == 0 && strcmp(lc->thousands_sep, "") == 0 && lc->frac_digits == 127);
    ucrt_setlocale(UCRT_LC_ALL, "English_United States");
    lc = ucrt_localeconv();
    MT_EXPECT(strcmp(lc->thousands_sep, ",") == 0 && strcmp(lc->grouping, "\3") == 0 && strcmp(lc->currency_symbol, "$") == 0);
    MT_EXPECT(lc->frac_digits == 2 && lc->p_cs_precedes == 1 && lc->_W_decimal_point[0] == '.');
    reset_locale();
}

static void character_classes(void)
{
    reset_locale();
    MT_EXPECT(!ucrt_loc_isalpha(0xE9) && ucrt_loc_isalpha('a') && ucrt_loc_toupper(0xE9) == 0xE9);
    ucrt_setlocale(UCRT_LC_ALL, "English_United States.1252");
    MT_EXPECT(ucrt_loc_isalpha(0xE9) && ucrt_loc_islower(0xE9) && ucrt_loc_isupper(0xC9) && !ucrt_loc_isalpha(0xD7) && !ucrt_loc_isalpha(0xF7));
    MT_EXPECT(ucrt_loc_toupper(0xE9) == 0xC9 && ucrt_loc_tolower(0xC9) == 0xE9 && ucrt_loc_toupper(0xFF) == 0x9F && ucrt_loc_tolower(0x9F) == 0xFF);
    MT_EXPECT(ucrt_loc_toupper(0x9A) == 0x8A && ucrt_loc_tolower(0x8A) == 0x9A);
    MT_EXPECT(ucrt_loc_isspace(0xA0) && ucrt_loc_ispunct(0xA1) && ucrt_loc_isalnum('7'));
    MT_EXPECT_EQ(ucrt_pctype()[0xE9] & 0x103, 0x102); /* the table agrees: a lower-case letter */
    ucrt_setlocale(UCRT_LC_ALL, ".UTF8");
    MT_EXPECT(!ucrt_loc_isalpha(0xE9)); /* bytes above 0x7F are not characters of their own in UTF-8 */
    reset_locale();
    MT_EXPECT(!ucrt_loc_isalpha(0xE9));
}

static void single_byte_conversion(void)
{
    ucrt_wchar w[8];
    char b[8];
    ucrt_mbstate st;
    reset_locale();
    __builtin_memset(&st, 0, sizeof st);
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\xE9", 1, &st), 1);
    MT_EXPECT_EQ(w[0], 0xE9); /* the C locale passes bytes through */
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0xE9, &st), 1);
    MT_EXPECT((unsigned char)b[0] == 0xE9);
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0x20AC, &st), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EILSEQ);
    ucrt_setlocale(UCRT_LC_ALL, "English_United States.1252");
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\x80", 1, &st), 1);
    MT_EXPECT_EQ(w[0], 0x20AC); /* the euro sign */
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0x20AC, &st), 1);
    MT_EXPECT((unsigned char)b[0] == 0x80);
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0x4E2D, &st), -1);
    MT_EXPECT_EQ(ucrt_btowc(0x92), 0x2019);
    MT_EXPECT_EQ(ucrt_wctob(0x2019), 0x92);
    MT_EXPECT_EQ(ucrt_wctob(0x4E2D), UCRT_EOF);
    MT_EXPECT_EQ((long)ucrt_mbstowcs(w, "ab\x80", 8), 3);
    MT_EXPECT(w[0] == 'a' && w[2] == 0x20AC && w[3] == 0);
    reset_locale();
}

static void utf8_known_sequences(void)
{
    ucrt_wchar w[8];
    char b[8];
    ucrt_mbstate st;
    ucrt_setlocale(UCRT_LC_ALL, ".UTF8");
    __builtin_memset(&st, 0, sizeof st);
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "A", 1, &st), 1);
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\xC3\xA9", 2, &st), 2);
    MT_EXPECT_EQ(w[0], 0xE9);
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\xE2\x82\xAC", 3, &st), 3);
    MT_EXPECT_EQ(w[0], 0x20AC);
    /* U+1F600: the first call gives the high surrogate and consumes the bytes; the next gives the low one, using none */
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\xF0\x9F\x98\x80", 4, &st), 4);
    MT_EXPECT_EQ(w[0], 0xD83D);
    MT_EXPECT(!ucrt_mbsinit(&st));
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "", 1, &st), (long)UCRT_MB_NEXT);
    MT_EXPECT_EQ(w[0], 0xDE00);
    MT_EXPECT(ucrt_mbsinit(&st));
    /* a sequence split across calls */
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\xE2\x82", 2, &st), (long)UCRT_MB_INCOMPLETE);
    MT_EXPECT_EQ((long)ucrt_mbrtowc(w, "\xAC", 1, &st), 1);
    MT_EXPECT_EQ(w[0], 0x20AC);
    /* encoding, including a pair of surrogates written one at a time */
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0x20AC, &st), 3);
    MT_EXPECT(memcmp(b, "\xE2\x82\xAC", 3) == 0);
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0xD83D, &st), 0);
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0xDE00, &st), 4);
    MT_EXPECT(memcmp(b, "\xF0\x9F\x98\x80", 4) == 0);
    MT_EXPECT_EQ((long)ucrt_wcrtomb(b, 0xDE00, &st), -1); /* a lone low surrogate */
    /* whole strings */
    {
        static const ucrt_wchar ws[] = {'h', 0xE9, 0x20AC, 0xD83D, 0xDE00, 0};
        char out[16];
        MT_EXPECT_EQ((long)ucrt_wcstombs(out, ws, sizeof out), 10);
        MT_EXPECT(strcmp(out, "h\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == 0);
        MT_EXPECT_EQ((long)ucrt_wcstombs(NULL, ws, 0), 10);
        MT_EXPECT_EQ((long)ucrt_wcstombs(out, ws, 5), 3); /* h + é, and the euro sign does not fit whole */
        out[3] = 0;
        MT_EXPECT_EQ((long)ucrt_mbstowcs(w, out, 8), 2);
        MT_EXPECT_EQ((long)ucrt_mbstowcs(w, "h\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", 8), 5);
        MT_EXPECT(memcmp(w, ws, 5 * sizeof(ucrt_wchar)) == 0 && w[5] == 0);
        MT_EXPECT_EQ((long)ucrt_mbstrlen("h\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"), 4);
    }
    MT_EXPECT_EQ(ucrt_mbtowc(w, "\xC3\xA9", 2), 2);
    MT_EXPECT_EQ(ucrt_mbtowc(w, "\xF0\x9F\x98\x80", 4), -1); /* one wchar_t cannot hold it */
    MT_EXPECT_EQ(ucrt_mblen("\xE2\x82\xAC", 3), 3);
    MT_EXPECT_EQ(ucrt_btowc('A'), 'A');
    MT_EXPECT_EQ(ucrt_btowc(0xE9), UCRT_WEOF);
    reset_locale();
}

/* Random byte sequences must be accepted or rejected exactly as glibc's UTF-8 does. */
static void utf8_validation_matches_glibc(void)
{
    locale_t l = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t)0);
    if (!l) {
        printf("    (C.UTF-8 is not installed; skipping)\n");
        return;
    }
    ucrt_setlocale(UCRT_LC_ALL, ".UTF8");
    for (int i = 0; i < 400000; ++i) {
        unsigned char s[6];
        size_t n = 1 + rnd64() % 4;
        mbstate_t gs;
        ucrt_mbstate us;
        wchar_t gw = 0;
        ucrt_wchar uw = 0;
        size_t gr, ur;
        for (size_t k = 0; k < n; ++k) {
            switch (rnd64() % 4) {
            case 0: s[k] = (unsigned char)(0x80 + rnd64() % 64); break;
            case 1: s[k] = (unsigned char)(0xC0 + rnd64() % 64); break;
            case 2: s[k] = (unsigned char)(rnd64() % 128); break;
            default: s[k] = (unsigned char)rnd64(); break;
            }
        }
        if (i % 3 == 0) { /* more likely valid: start from a real encoding */
            unsigned cp = (unsigned)(rnd64() % 0x110000);
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0x41;
            n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (n == 1)
                s[0] = (unsigned char)cp;
            else if (n == 2) {
                s[0] = (unsigned char)(0xC0 | (cp >> 6));
                s[1] = (unsigned char)(0x80 | (cp & 0x3F));
            } else if (n == 3) {
                s[0] = (unsigned char)(0xE0 | (cp >> 12));
                s[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                s[2] = (unsigned char)(0x80 | (cp & 0x3F));
            } else {
                s[0] = (unsigned char)(0xF0 | (cp >> 18));
                s[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                s[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                s[3] = (unsigned char)(0x80 | (cp & 0x3F));
            }
        }
        memset(&gs, 0, sizeof gs);
        memset(&us, 0, sizeof us);
        {
            locale_t old = uselocale(l);
            gr = mbrtowc(&gw, (const char *)s, n, &gs);
            uselocale(old);
        }
        ur = ucrt_mbrtowc(&uw, (const char *)s, n, &us);
        if (s[0] == 0 || s[0] >= 0xF5) /* glibc still waits for the continuation of the old 5- and 6-byte forms */
            continue;
        if (gr == (size_t)-1 || gr == (size_t)-2) {
            if (ur != gr && !(gr == (size_t)-1 && ur == (size_t)-1)) {
                MT_FAIL("bytes %02x %02x %02x %02x (n=%zu): glibc %zd, here %zd", s[0], s[1], s[2], s[3], n, (ssize_t)gr, (ssize_t)ur);
                break;
            }
        } else if (gw > 0x10FFFF) { /* glibc accepts code points no Unicode encoding allows; the Microsoft runtime does not */
            if (ur != (size_t)-1) {
                MT_FAIL("bytes %02x %02x %02x %02x accepted beyond U+10FFFF", s[0], s[1], s[2], s[3]);
                break;
            }
        } else {
            wchar_t expect_hi = gw >= 0x10000 ? (wchar_t)(0xD800 + ((gw - 0x10000) >> 10)) : gw;
            if (ur != gr || uw != (ucrt_wchar)expect_hi) {
                MT_FAIL("bytes %02x %02x %02x %02x (n=%zu): glibc %zd/%x, here %zd/%x", s[0], s[1], s[2], s[3], n, (ssize_t)gr, (unsigned)gw,
                        (ssize_t)ur, (unsigned)uw);
                break;
            }
        }
    }
    freelocale(l);
    reset_locale();
}

static void secure_conversions(void)
{
    ucrt_wchar w[8];
    char b[8];
    size_t ret;
    reset_locale();
    MT_EXPECT_EQ(ucrt_mbstowcs_s(&ret, w, 8, "hello", UCRT_TRUNCATE_), 0);
    MT_EXPECT(ret == 6 && w[4] == 'o' && w[5] == 0);
    MT_EXPECT_EQ(ucrt_mbstowcs_s(&ret, NULL, 0, "hello", 0), 0);
    MT_EXPECT_EQ((long)ret, 6);
    MT_EXPECT_EQ(ucrt_mbstowcs_s(&ret, w, 4, "hello", UCRT_TRUNCATE_), UCRT_STRUNCATE);
    MT_EXPECT(ret == 4 && w[2] == 'l' && w[3] == 0);
    MT_EXPECT_EQ(ucrt_mbstowcs_s(&ret, w, 4, "hello", 10), UCRT_ERANGE);
    MT_EXPECT_EQ(w[0], 0);
    MT_EXPECT_EQ(ucrt_mbstowcs_s(&ret, w, 8, "hello", 2), 0); /* count limits what is copied */
    MT_EXPECT(ret == 3 && w[1] == 'e' && w[2] == 0);
    {
        static const ucrt_wchar ws[] = {'a', 'b', 'c', 0};
        MT_EXPECT_EQ(ucrt_wcstombs_s(&ret, b, 8, ws, UCRT_TRUNCATE_), 0);
        MT_EXPECT(ret == 4 && strcmp(b, "abc") == 0);
        MT_EXPECT_EQ(ucrt_wcstombs_s(&ret, b, 3, ws, UCRT_TRUNCATE_), UCRT_STRUNCATE);
        MT_EXPECT(strcmp(b, "ab") == 0);
        MT_EXPECT_EQ(ucrt_wcstombs_s(&ret, b, 3, ws, 5), UCRT_ERANGE);
    }
    {
        int n;
        MT_EXPECT_EQ(ucrt_wctomb_s(&n, b, 8, 'x'), 0);
        MT_EXPECT(n == 1 && b[0] == 'x');
        MT_EXPECT_EQ(ucrt_wctomb_s(&n, b, 8, 0x20AC), UCRT_EILSEQ); /* the C locale has no euro */
    }
}

static const mt_case_t cases[] = {
    MT_CASE(setlocale_names), MT_CASE(localeconv_values), MT_CASE(character_classes), MT_CASE(single_byte_conversion),
    MT_CASE(utf8_known_sequences), MT_CASE(utf8_validation_matches_glibc), MT_CASE(secure_conversions),
};

int main(void)
{
    return MT_RUN("crt_locale", cases);
}
