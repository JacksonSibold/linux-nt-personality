/*
 * test_k32_env_pure.c - host tests for src/ntdll/k32_env_pure.h: the environment
 * block length, the name lookup (case-insensitive, hidden "=X:" names, entries
 * with no '='), set and delete (order kept, size limit), and %NAME% expansion.
 *
 * The expected values are written here by hand from the rules in the header.
 */
#include "minitest.h"
#include "k32_env_pure.h"

#include <string.h>

#define BLK_CAP 256

/* ASCII to WCHAR, NUL terminated. */
static void wstr(WCHAR *out, const char *s)
{
    size_t i = 0;
    for (; s[i] != '\0'; ++i)
        out[i] = (WCHAR)(unsigned char)s[i];
    out[i] = 0;
}

/* Builds "e0\0e1\0...\0\0" from n ASCII entries. Returns its length in units. */
static size_t make_block(WCHAR *out, size_t cap, const char *const *entries, size_t n)
{
    size_t pos = 0;
    for (size_t e = 0; e < n; ++e) {
        for (size_t i = 0; entries[e][i] != '\0'; ++i) {
            if (pos + 2 >= cap) return 0;
            out[pos++] = (WCHAR)(unsigned char)entries[e][i];
        }
        out[pos++] = 0;
    }
    if (pos + 1 > cap) return 0;
    out[pos++] = 0;
    return pos;
}

/* Does the value at (off, len) in blk equal the ASCII string want? */
static int value_is(const WCHAR *blk, size_t off, size_t len, const char *want)
{
    size_t wl = strlen(want);
    if (len != wl) return 0;
    for (size_t i = 0; i < wl; ++i) {
        if (blk[off + i] != (WCHAR)(unsigned char)want[i]) return 0;
    }
    return 1;
}

/* Compares the first n units of a block with an expected block of w units. */
static int block_equals(const WCHAR *got, size_t gn, const WCHAR *want, size_t wn)
{
    if (gn != wn) return 0;
    return memcmp(got, want, wn * sizeof(WCHAR)) == 0;
}

static void block_units_cases(void)
{
    WCHAR b[BLK_CAP];
    const char *two[] = { "A=1", "B=2" };
    size_t n = make_block(b, BLK_CAP, two, 2);
    MT_EXPECT(n == 9);                              /* "A=1\0B=2\0\0" */
    MT_EXPECT(k32e_block_units(b, BLK_CAP) == 9);
    MT_EXPECT(k32e_block_units(b, 8) == 0);          /* terminator beyond the bound */
    WCHAR empty[2] = { 0, 0 };
    MT_EXPECT(k32e_block_units(empty, 2) == 1);      /* the empty entry at index 0 */
    WCHAR open[3] = { 'A', '=', '1' };               /* no NUL at all */
    MT_EXPECT(k32e_block_units(open, 3) == 0);
}

static void find_cases(void)
{
    WCHAR b[BLK_CAP];
    const char *e[] = { "PATH=C:\\bin", "Empty=", "NOEQUALS", "=C:=C:\\work", "PA=short" };
    size_t units = make_block(b, BLK_CAP, e, 5);
    MT_ASSERT(units > 0);
    size_t off = 0, len = 0;
    WCHAR name[32];

    wstr(name, "PATH");
    MT_ASSERT(k32e_find(b, units, name, 4, &off, &len) == 1);
    MT_EXPECT(value_is(b, off, len, "C:\\bin"));

    /* Names compare case-insensitively. */
    wstr(name, "path");
    MT_ASSERT(k32e_find(b, units, name, 4, &off, &len) == 1);
    MT_EXPECT(value_is(b, off, len, "C:\\bin"));

    /* An exact-length match only: PA is not PATH, and PATH is not PA. */
    wstr(name, "PA");
    MT_ASSERT(k32e_find(b, units, name, 2, &off, &len) == 1);
    MT_EXPECT(value_is(b, off, len, "short"));
    wstr(name, "PAT");
    MT_EXPECT(k32e_find(b, units, name, 3, &off, &len) == 0);

    /* An entry with '=' and an empty value. */
    wstr(name, "empty");
    MT_ASSERT(k32e_find(b, units, name, 5, &off, &len) == 1);
    MT_EXPECT(len == 0);

    /* An entry with no '=' has an empty value. */
    wstr(name, "NOEQUALS");
    MT_ASSERT(k32e_find(b, units, name, 8, &off, &len) == 1);
    MT_EXPECT(len == 0);

    /* A hidden entry: its name keeps the leading '='. */
    wstr(name, "=C:");
    MT_ASSERT(k32e_find(b, units, name, 3, &off, &len) == 1);
    MT_EXPECT(value_is(b, off, len, "C:\\work"));

    wstr(name, "MISSING");
    MT_EXPECT(k32e_find(b, units, name, 7, &off, &len) == 0);
}

/* Set and delete on a block with two variables. */
static void set_build_cases(void)
{
    WCHAR src[BLK_CAP];
    const char *e[] = { "A=1", "B=2", "C=3" };
    size_t units = make_block(src, BLK_CAP, e, 3);
    WCHAR name[16], value[16];
    size_t need = 0;

    /* New variable: appended at the end. */
    wstr(name, "D");
    wstr(value, "4");
    MT_EXPECT(k32e_set_build(NULL, 0, src, units, name, 1, value, 1, 1, &need) == 0);
    WCHAR want_new[BLK_CAP];
    const char *e_new[] = { "A=1", "B=2", "C=3", "D=4" };
    size_t wn = make_block(want_new, BLK_CAP, e_new, 4);
    MT_EXPECT(need == wn);
    WCHAR got[BLK_CAP];
    size_t gn = 0;
    MT_EXPECT(k32e_set_build(got, BLK_CAP, src, units, name, 1, value, 1, 1, &gn) == 0);
    MT_EXPECT(block_equals(got, gn, want_new, wn));

    /* Replace in place, matching the name case-insensitively. */
    wstr(name, "b");
    wstr(value, "twenty");
    MT_EXPECT(k32e_set_build(NULL, 0, src, units, name, 1, value, 6, 1, &need) == 0);
    WCHAR want_rep[BLK_CAP];
    const char *e_rep[] = { "A=1", "b=twenty", "C=3" };
    size_t rn = make_block(want_rep, BLK_CAP, e_rep, 3);
    MT_EXPECT(need == rn);
    MT_EXPECT(k32e_set_build(got, BLK_CAP, src, units, name, 1, value, 6, 1, &gn) == 0);
    MT_EXPECT(block_equals(got, gn, want_rep, rn));

    /* Delete: has_value 0, the entry goes and the rest keeps order. */
    wstr(name, "A");
    MT_EXPECT(k32e_set_build(NULL, 0, src, units, name, 1, NULL, 0, 0, &need) == 0);
    WCHAR want_del[BLK_CAP];
    const char *e_del[] = { "B=2", "C=3" };
    size_t dn = make_block(want_del, BLK_CAP, e_del, 2);
    MT_EXPECT(need == dn);
    MT_EXPECT(k32e_set_build(got, BLK_CAP, src, units, name, 1, NULL, 0, 0, &gn) == 0);
    MT_EXPECT(block_equals(got, gn, want_del, dn));

    /* An empty value is a value: "E=". */
    wstr(name, "E");
    MT_EXPECT(k32e_set_build(NULL, 0, src, units, name, 1, value, 0, 1, &need) == 0);
    MT_EXPECT(need == units + 3);   /* "E=" and its NUL; the list terminator is already in units */

    /* Over the limit: the result is refused, need still reports the size. */
    static WCHAR big[K32E_MAX_UNITS + 10];
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); ++i)
        big[i] = 'x';
    wstr(name, "F");
    MT_EXPECT(k32e_set_build(NULL, 0, src, units, name, 1, big, K32E_MAX_UNITS, 1, &need)
              == K32E_ERROR_NOT_ENOUGH_MEMORY);
    MT_EXPECT(need > K32E_MAX_UNITS);
}

/* A variable set through the block must be found again afterwards. */
static void set_then_find(void)
{
    WCHAR src[BLK_CAP];
    const char *e[] = { "KEEP=yes" };
    size_t units = make_block(src, BLK_CAP, e, 1);
    WCHAR name[16], value[16], out[BLK_CAP];
    wstr(name, "NTPERS_S9");
    wstr(value, "nine");
    size_t need = 0, got = 0;
    MT_EXPECT(k32e_set_build(NULL, 0, src, units, name, 9, value, 4, 1, &need) == 0);
    MT_ASSERT(need <= BLK_CAP);
    MT_EXPECT(k32e_set_build(out, BLK_CAP, src, units, name, 9, value, 4, 1, &got) == 0);
    size_t off = 0, len = 0;
    MT_ASSERT(k32e_find(out, need, name, 9, &off, &len) == 1);
    MT_EXPECT(value_is(out, off, len, "nine"));
    wstr(name, "KEEP");
    MT_ASSERT(k32e_find(out, need, name, 4, &off, &len) == 1);
    MT_EXPECT(value_is(out, off, len, "yes"));
}

/* Expansion over a block with A=1, EMPTY= and B=two. */
static void expand_one(const char *src_ascii, const char *want_ascii)
{
    WCHAR blk[BLK_CAP];
    const char *e[] = { "A=1", "EMPTY=", "B=two" };
    size_t units = make_block(blk, BLK_CAP, e, 3);
    WCHAR src[BLK_CAP], want[BLK_CAP], got[BLK_CAP];
    wstr(src, src_ascii);
    size_t slen = strlen(src_ascii);
    size_t wlen = strlen(want_ascii);
    wstr(want, want_ascii);
    size_t need = 0;
    MT_EXPECT(k32e_expand(NULL, 0, src, slen, blk, units, &need) == 0);
    MT_EXPECT(need == wlen + 1);    /* the count includes the NUL */
    MT_EXPECT(k32e_expand(got, BLK_CAP, src, slen, blk, units, &need) == 0);
    MT_EXPECT(need == wlen + 1);
    MT_EXPECT(memcmp(got, want, (wlen + 1) * sizeof(WCHAR)) == 0);
}

static void expand_cases(void)
{
    expand_one("%A%", "1");
    expand_one("x%A%y%B%z", "x1ytwoz");
    expand_one("[%EMPTY%]", "[]");
    expand_one("%NOPE%", "%NOPE%");        /* unset name stays as written */
    expand_one("100%", "100%");            /* unpaired % stays */
    expand_one("%A", "%A");                /* no closing % */
    expand_one("%%", "%%");                /* not an escape */
    expand_one("%a%", "1");                /* name match is case-insensitive */
    expand_one("", "");                    /* empty source: NUL only */
}

/* Expansion with no block (no variables): every %NAME% stays. */
static void expand_without_block(void)
{
    WCHAR src[16], got[16];
    wstr(src, "%PATH%");
    size_t need = 0;
    MT_EXPECT(k32e_expand(got, 16, src, 6, NULL, 0, &need) == 0);
    MT_EXPECT(need == 7);
    MT_EXPECT(memcmp(got, src, 7 * sizeof(WCHAR)) == 0);
}

static const mt_case_t cases[] = {
    { "block_units", block_units_cases },
    { "find", find_cases },
    { "set_build", set_build_cases },
    { "set_then_find", set_then_find },
    { "expand", expand_cases },
    { "expand_without_block", expand_without_block },
};

int main(void)
{
    return MT_RUN("k32_env_pure", cases);
}
