/*
 * test_msvcrt_policy.c - host tests for the size arithmetic in
 * src/ntdll/msvcrt_policy.h, used by the msvcrt calloc (CRT-3), and the
 * snprintf return value and truncation of src/ntdll/msvcrt_format.h (CRT-2).
 */
#include "minitest.h"
#include "msvcrt_policy.h"
#include "msvcrt_format.h"
#include <stdarg.h>
#include <string.h>

#define MS_SIZE_MAX ((SIZE_T)-1)

/* CRT-3: calloc(n, s) must refuse a product that wraps. */
static void mul_accepts_products_that_fit(void)
{
    SIZE_T out = 0;
    MT_EXPECT(ms_mul_size(3, 4, &out));
    MT_EXPECT_EQ(out, 12);

    out = 0;
    MT_EXPECT(ms_mul_size(0, MS_SIZE_MAX, &out));
    MT_EXPECT(out == 0);

    out = 0;
    MT_EXPECT(ms_mul_size(MS_SIZE_MAX, 1, &out));
    MT_EXPECT(out == MS_SIZE_MAX);

    out = 0;
    MT_EXPECT(ms_mul_size(MS_SIZE_MAX / 2, 2, &out));
    MT_EXPECT(out == MS_SIZE_MAX - 1);
}

static void mul_rejects_wrapping_products(void)
{
    SIZE_T out = 0x5a5a;
    MT_EXPECT(!ms_mul_size(MS_SIZE_MAX, 2, &out));
    MT_EXPECT(out == 0x5a5a); /* *out is left unchanged on failure */

    MT_EXPECT(!ms_mul_size(MS_SIZE_MAX / 2 + 1, 2, &out));
    MT_EXPECT(!ms_mul_size(2, MS_SIZE_MAX / 2 + 1, &out));
    MT_EXPECT(!ms_mul_size(MS_SIZE_MAX, MS_SIZE_MAX, &out));
    MT_EXPECT(out == 0x5a5a);
}

/* CRT-1: sprintf never writes past MS_SPRINTF_LIMIT bytes. */
static void sprintf_write_size_is_output_plus_nul(void)
{
    MT_EXPECT_EQ(ms_sprintf_write_size(0), 1);
    MT_EXPECT_EQ(ms_sprintf_write_size(5), 6);
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT - 2), MS_SPRINTF_LIMIT - 1);
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT - 1), MS_SPRINTF_LIMIT);
}

static void sprintf_write_size_is_bounded(void)
{
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT), MS_SPRINTF_LIMIT);
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT * 8), MS_SPRINTF_LIMIT);
    MT_EXPECT(ms_sprintf_write_size((SIZE_T)-1) == MS_SPRINTF_LIMIT);
}

/* The msvcrt snprintf is my_vsprintf plus va_start. */
static int fmt(char *out, size_t outsz, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = my_vsprintf(out, outsz, f, ap);
    va_end(ap);
    return r;
}

/* CRT-2: snprintf returns the length the output needs, so a caller can size a buffer. */
static void snprintf_size_query_returns_needed_length(void)
{
    MT_EXPECT_EQ(fmt(NULL, 0, "%s", "abc"), 3);
    MT_EXPECT_EQ(fmt(NULL, 0, "n=%d %s%%", -42, "xy"), 9);
    MT_EXPECT_EQ(fmt(NULL, 0, ""), 0);
}

static void snprintf_fitting_output_is_unchanged(void)
{
    char buf[16];
    MT_EXPECT_EQ(fmt(buf, sizeof(buf), "%s-%u", "ab", 7u), 4);
    MT_EXPECT(strcmp(buf, "ab-7") == 0);
}

static void snprintf_truncates_and_still_returns_needed_length(void)
{
    char buf[4];
    memset(buf, 'x', sizeof(buf));
    MT_EXPECT_EQ(fmt(buf, sizeof(buf), "%s", "abcdef"), 6);
    MT_EXPECT(strcmp(buf, "abc") == 0); /* NUL-terminated within the buffer */

    char one[1] = { 'x' };
    MT_EXPECT_EQ(fmt(one, 1, "%s", "abc"), 3);
    MT_EXPECT(one[0] == '\0');

    char guard[2] = { 'x', 'g' };
    MT_EXPECT_EQ(fmt(guard, 0, "%s", "abc"), 3); /* size 0 writes nothing */
    MT_EXPECT(guard[0] == 'x' && guard[1] == 'g');
}

static void stored_length_is_clamped_to_the_buffer(void)
{
    MT_EXPECT_EQ(ms_stored(3, 0), 0);
    MT_EXPECT_EQ(ms_stored(3, 4), 3);
    MT_EXPECT_EQ(ms_stored(4, 4), 3);
    MT_EXPECT_EQ(ms_stored(100, 4), 3);
}

/* Startup argv split (ms_split_cmdline, msvcrt_policy.h): the Windows rules, with
 * expected tokens written here by hand. */
static int split_is(const char *cmd, const char *const *want, int nwant)
{
    char buf[256];
    char *argv[16];
    int n = ms_split_cmdline(cmd, buf, sizeof(buf), argv, 16);
    if (n != nwant) return 0;
    for (int i = 0; i < nwant; i++) {
        if (strcmp(argv[i], want[i]) != 0) return 0;
    }
    return argv[nwant] == NULL;
}

static void split_plain_and_quoted(void)
{
    const char *w1[] = { "a", "b" };
    MT_EXPECT(split_is("a b", w1, 2));
    MT_EXPECT(split_is("  a 	 b  ", w1, 2));
    const char *w2[] = { "beta gamma", "3" };
    MT_EXPECT(split_is("\"beta gamma\" 3", w2, 2));
    const char *w3[] = { "prog", "say\"hi" };
    MT_EXPECT(split_is("prog say\\\"hi", w3, 2));
    const char *w4[] = { "a", "", "b" };
    MT_EXPECT(split_is("a \"\" b", w4, 3));
}

static void split_backslashes(void)
{
    /* a\\"b: two backslashes then a quote give one backslash and a quote toggle. */
    const char *w1[] = { "a\\b" };
    MT_EXPECT(split_is("a\\\\\"b", w1, 1));
    /* x\\\"y: three backslashes then a quote give one backslash and a literal quote. */
    const char *w2[] = { "x\\\"y" };
    MT_EXPECT(split_is("x\\\\\\\"y", w2, 1));
    /* Backslashes not before a quote stay literal. */
    const char *w3[] = { "c:\\dir\\f" };
    MT_EXPECT(split_is("c:\\dir\\f", w3, 1));
}

static void split_empty_and_limits(void)
{
    char buf[256];
    char *argv[4];
    MT_EXPECT(ms_split_cmdline("", buf, sizeof(buf), argv, 4) == 0);
    MT_EXPECT(argv[0] == NULL);
    MT_EXPECT(ms_split_cmdline("   	 ", buf, sizeof(buf), argv, 4) == 0);
    /* Three tokens need four slots with the NULL. */
    MT_EXPECT(ms_split_cmdline("a b c", buf, sizeof(buf), argv, 3) == -1);
    MT_EXPECT(ms_split_cmdline("a b c", buf, sizeof(buf), argv, 4) == 3);
    /* The token copies must fit in cap, NUL included. */
    MT_EXPECT(ms_split_cmdline("abcdef", buf, 6, argv, 4) == -1);
    MT_EXPECT(ms_split_cmdline("abcde", buf, 6, argv, 4) == 1);
}

static const mt_case_t cases[] = {
    MT_CASE(mul_accepts_products_that_fit),
    MT_CASE(mul_rejects_wrapping_products),
    MT_CASE(sprintf_write_size_is_output_plus_nul),
    MT_CASE(sprintf_write_size_is_bounded),
    MT_CASE(snprintf_size_query_returns_needed_length),
    MT_CASE(snprintf_fitting_output_is_unchanged),
    MT_CASE(snprintf_truncates_and_still_returns_needed_length),
    MT_CASE(stored_length_is_clamped_to_the_buffer),
    MT_CASE(split_plain_and_quoted),
    MT_CASE(split_backslashes),
    MT_CASE(split_empty_and_limits),
};

int main(void)
{
    return MT_RUN("msvcrt_policy", cases);
}
