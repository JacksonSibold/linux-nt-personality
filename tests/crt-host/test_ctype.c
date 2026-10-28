/*
 * test_ctype.c - tests for src/crt/ucrt_ctype.h: the narrow classification against the system library in the
 * "C" locale, the pctype table, and the wide case mapping and classification against values from Unicode.
 */
#include "minitest.h"
#include "ucrt_ctype.h"

#include <ctype.h>
#include <locale.h>

#define BOOLEQ(a, b) (((a) != 0) == ((b) != 0))

static void narrow_matches_c_locale(void)
{
    int c;
    setlocale(LC_ALL, "C");
    for (c = -1; c < 256; ++c) {
        if (c == -1 || c < 128) { /* the system library's behaviour above 0x7F depends on the locale: C gives 0 */
            MT_EXPECT(BOOLEQ(ucrt_isalpha(c), isalpha(c)));
            MT_EXPECT(BOOLEQ(ucrt_isupper(c), isupper(c)));
            MT_EXPECT(BOOLEQ(ucrt_islower(c), islower(c)));
            MT_EXPECT(BOOLEQ(ucrt_isdigit(c), isdigit(c)));
            MT_EXPECT(BOOLEQ(ucrt_isxdigit(c), isxdigit(c)));
            MT_EXPECT(BOOLEQ(ucrt_isspace(c), isspace(c)));
            MT_EXPECT(BOOLEQ(ucrt_ispunct(c), ispunct(c)));
            MT_EXPECT(BOOLEQ(ucrt_iscntrl(c), iscntrl(c)));
            MT_EXPECT(BOOLEQ(ucrt_isalnum(c), isalnum(c)));
            MT_EXPECT(BOOLEQ(ucrt_isgraph(c), isgraph(c)));
            MT_EXPECT(BOOLEQ(ucrt_isprint(c), isprint(c)));
            MT_EXPECT(BOOLEQ(ucrt_isblank(c), isblank(c)));
            MT_EXPECT_EQ(ucrt_toupper(c), toupper(c));
            MT_EXPECT_EQ(ucrt_tolower(c), tolower(c));
        } else {
            MT_EXPECT(!ucrt_isalpha(c) && !ucrt_isdigit(c) && !ucrt_ispunct(c) && !ucrt_isspace(c));
            MT_EXPECT_EQ(ucrt_toupper(c), c);
            MT_EXPECT_EQ(ucrt_tolower(c), c);
        }
    }
    MT_EXPECT(ucrt_isascii(0x7F) && !ucrt_isascii(0x80) && !ucrt_isascii(-1));
}

static void pctype_table(void)
{
    const unsigned short *t = ucrt_pctype();
    MT_EXPECT_EQ(t[-1], 0); /* EOF is in no class */
    MT_EXPECT_EQ(t[0], UCRT_CONTROL);
    MT_EXPECT_EQ(t['A'], UCRT_UPPER | UCRT_HEX | 0x100);
    MT_EXPECT_EQ(t['g'], UCRT_LOWER | 0x100);
    MT_EXPECT_EQ(t['5'], UCRT_DIGIT | UCRT_HEX);
    MT_EXPECT_EQ(t[' '], UCRT_SPACE | UCRT_BLANK);
    MT_EXPECT_EQ(t['\t'], UCRT_SPACE | UCRT_BLANK | UCRT_CONTROL);
    MT_EXPECT_EQ(t['\n'], UCRT_SPACE | UCRT_CONTROL);
    MT_EXPECT_EQ(t['!'], UCRT_PUNCT);
    MT_EXPECT_EQ(t[0x80], 0);
    MT_EXPECT_EQ(t[255], 0);
    /* The classic test the headers do: ((_pctype[c] & _ALPHA) != 0). */
    MT_EXPECT(t['x'] & UCRT_ALPHA);
    MT_EXPECT(!(t['3'] & UCRT_ALPHA));
}

static void wide_case_mapping(void)
{
    MT_EXPECT_EQ(ucrt_towupper('a'), 'A');
    MT_EXPECT_EQ(ucrt_towlower('Q'), 'q');
    MT_EXPECT_EQ(ucrt_towupper(0xE9), 0xC9);   /* e acute */
    MT_EXPECT_EQ(ucrt_towlower(0xC9), 0xE9);
    MT_EXPECT_EQ(ucrt_towupper(0xF7), 0xF7);   /* division sign */
    MT_EXPECT_EQ(ucrt_towlower(0xD7), 0xD7);   /* multiplication sign */
    MT_EXPECT_EQ(ucrt_towupper(0xFF), 0x178);  /* y diaeresis */
    MT_EXPECT_EQ(ucrt_towlower(0x178), 0xFF);
    MT_EXPECT_EQ(ucrt_towupper(0xB5), 0x39C);  /* micro sign */
    MT_EXPECT_EQ(ucrt_towupper(0x101), 0x100); /* a macron */
    MT_EXPECT_EQ(ucrt_towlower(0x100), 0x101);
    MT_EXPECT_EQ(ucrt_towupper(0x13A), 0x139); /* l acute: the odd/even pairs flip in this range */
    MT_EXPECT_EQ(ucrt_towlower(0x139), 0x13A);
    MT_EXPECT_EQ(ucrt_towupper(0x17E), 0x17D); /* z caron */
    MT_EXPECT_EQ(ucrt_towupper(0x131), 'I');   /* dotless i */
    MT_EXPECT_EQ(ucrt_towlower(0x130), 'i');   /* dotted capital I */
    MT_EXPECT_EQ(ucrt_towupper(0x3B1), 0x391); /* alpha */
    MT_EXPECT_EQ(ucrt_towupper(0x3C2), 0x3A3); /* final sigma */
    MT_EXPECT_EQ(ucrt_towlower(0x3A3), 0x3C3);
    MT_EXPECT_EQ(ucrt_towupper(0x3AC), 0x386);
    MT_EXPECT_EQ(ucrt_towlower(0x38A), 0x3AF);
    MT_EXPECT_EQ(ucrt_towupper(0x430), 0x410); /* Cyrillic a */
    MT_EXPECT_EQ(ucrt_towupper(0x451), 0x401); /* io */
    MT_EXPECT_EQ(ucrt_towlower(0x40F), 0x45F);
    MT_EXPECT_EQ(ucrt_towupper(0x4E00), 0x4E00);
    MT_EXPECT_EQ(ucrt_towupper(0xFFFF), 0xFFFF);
}

/* Upper and lower mapping are inverse on every character that has a case, for the ranges handled. */
static void wide_case_round_trip(void)
{
    unsigned c;
    for (c = 0; c < 0x500; ++c) {
        ucrt_wchar u = ucrt_towupper((ucrt_wchar)c), l = ucrt_towlower((ucrt_wchar)c);
        if (u != c && c != 0xB5 && c != 0x131 && c != 0x17F && c != 0x3C2)
            MT_EXPECT_EQ(ucrt_towlower(u), c);
        if (l != c && c != 0x130)
            MT_EXPECT_EQ(ucrt_towupper(l), c);
    }
}

static void wide_classification(void)
{
    MT_EXPECT(ucrt_iswalpha('z') && ucrt_iswalpha(0xE9) && ucrt_iswalpha(0x3B1) && ucrt_iswalpha(0x44F));
    MT_EXPECT(ucrt_iswalpha(0x4E2D) && ucrt_iswalpha(0x3042) && ucrt_iswalpha(0xAC00) && ucrt_iswalpha(0x5D0));
    MT_EXPECT(!ucrt_iswalpha('5') && !ucrt_iswalpha(' ') && !ucrt_iswalpha(0xD7) && !ucrt_iswalpha(0xF7));
    MT_EXPECT(!ucrt_iswalpha(0x2014) && !ucrt_iswalpha(0x20AC));
    MT_EXPECT(ucrt_iswupper('Q') && !ucrt_iswupper('q') && ucrt_iswupper(0xC9) && ucrt_iswlower(0xE9));
    MT_EXPECT(!ucrt_iswupper(0x4E00) && !ucrt_iswlower(0x4E00));
    MT_EXPECT(ucrt_iswdigit('7') && !ucrt_iswdigit(0x661)); /* iswdigit is the ASCII digits only */
    MT_EXPECT(ucrt_iswxdigit('f') && !ucrt_iswxdigit('g') && !ucrt_iswxdigit(0xFF21)); /* fullwidth A is not */
    MT_EXPECT(ucrt_iswspace(' ') && ucrt_iswspace('\n') && ucrt_iswspace(0xA0) && ucrt_iswspace(0x3000));
    MT_EXPECT(!ucrt_iswspace('a') && !ucrt_iswspace(0x200B));
    MT_EXPECT(ucrt_iswcntrl(0) && ucrt_iswcntrl(0x1F) && ucrt_iswcntrl(0x7F) && ucrt_iswcntrl(0x9F) && !ucrt_iswcntrl(' '));
    MT_EXPECT(ucrt_iswpunct('!') && ucrt_iswpunct('~') && ucrt_iswpunct(0x2014) && !ucrt_iswpunct('a') && !ucrt_iswpunct(' '));
    MT_EXPECT(ucrt_iswalnum('a') && ucrt_iswalnum('9') && !ucrt_iswalnum('-'));
    MT_EXPECT(ucrt_iswprint('a') && ucrt_iswprint(' ') && !ucrt_iswprint(0x7));
    MT_EXPECT(ucrt_iswgraph('a') && !ucrt_iswgraph(' '));
    MT_EXPECT(ucrt_iswblank(' ') && ucrt_iswblank('\t') && !ucrt_iswblank('\n'));
}

static const mt_case_t cases[] = {
    MT_CASE(narrow_matches_c_locale),
    MT_CASE(pctype_table),
    MT_CASE(wide_case_mapping),
    MT_CASE(wide_case_round_trip),
    MT_CASE(wide_classification),
};

int main(void)
{
    return MT_RUN("crt_ctype", cases);
}
