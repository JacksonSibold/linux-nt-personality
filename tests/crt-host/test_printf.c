/*
 * test_printf.c - tests for src/crt/ucrt_printf.h: random integer and floating point conversions with every
 * flag, width and precision against glibc; Windows specifics (%I64d, %p, %hs and %ws, (null), %Z, the
 * return rules of snprintf and _snprintf, %n refused) against output worked out by hand; and the wide engine.
 */
#include "minitest.h"
#include "ucrt_printf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0xA0761D6478BD642Full;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void build_spec(char *spec, const char *len, char conv, int allow_alt)
{
    char *p = spec;
    *p++ = '%';
    if (rnd64() % 3 == 0)
        *p++ = '-';
    if (rnd64() % 3 == 0)
        *p++ = '+';
    if (rnd64() % 4 == 0)
        *p++ = ' ';
    if (allow_alt && rnd64() % 3 == 0)
        *p++ = '#';
    if (rnd64() % 3 == 0)
        *p++ = '0';
    if (rnd64() % 2)
        p += sprintf(p, "%d", (int)(rnd64() % 30));
    if (rnd64() % 2)
        p += sprintf(p, ".%d", (int)(rnd64() % 25));
    while (*len)
        *p++ = *len++;
    *p++ = conv;
    *p = 0;
}

static void integers_match_glibc(void)
{
    static const char convs[] = "diuoxX";
    static const char *const lens[] = {"", "hh", "h", "ll", "j", "z", "t"};
    char spec[64], want[400], got[400];
    int i;
    for (i = 0; i < 120000; ++i) {
        char conv = convs[rnd64() % 6];
        const char *len = lens[rnd64() % 7];
        long long v = (long long)rnd64();
        int w, g;
        int use_long = (len[0] == 'l' || len[0] == 'j' || len[0] == 'z' || len[0] == 't');
        switch (rnd64() % 4) {
        case 0: v = (long long)(int8_t)v; break;
        case 1: v = (long long)(int32_t)v; break;
        case 2: v = (long long)(rnd64() % 1000) - 500; break;
        default: break;
        }
        build_spec(spec, len, conv, 1);
        if (use_long) {
            w = snprintf(want, sizeof(want), spec, v);
            g = ucrt_snprintf(got, sizeof(got), spec, v);
        } else {
            w = snprintf(want, sizeof(want), spec, (int)v);
            g = ucrt_snprintf(got, sizeof(got), spec, (int)v);
        }
        if (w != g || strcmp(want, got)) {
            printf("    %s of %lld: want \"%s\" (%d), got \"%s\" (%d)\n", spec, v, want, w, got, g);
            MT_EXPECT(0);
            return;
        }
    }
}

static void floats_match_glibc(void)
{
    static const char convs[] = "fFeEgG";
    char spec[64], want[2000], got[2000];
    int i;
    for (i = 0; i < 120000; ++i) {
        char conv = convs[rnd64() % 6];
        double v;
        int w, g;
        switch (rnd64() % 5) {
        case 0: { /* any finite double */
            uint64_t b = rnd64() & 0x7FFFFFFFFFFFFFFFull;
            if ((b >> 52) == 0x7FF)
                b &= ~(1ull << 62);
            memcpy(&v, &b, 8);
            break;
        }
        case 1: v = (double)(int64_t)(rnd64() >> 20) / (double)(1 << (rnd64() % 20)); break;
        case 2: v = (double)(int)(rnd64() % 2000000) / 1000.0; break;
        case 3: v = ldexp((double)(rnd64() % 9) + 0.5, (int)(rnd64() % 40) - 20); break; /* ties */
        default: v = (double)(rnd64() % 100) * 0.25; break;
        }
        if (rnd64() & 1)
            v = -v;
        build_spec(spec, "", conv, 1);
        w = snprintf(want, sizeof(want), spec, v);
        g = ucrt_snprintf(got, sizeof(got), spec, v);
        if (w != g || strcmp(want, got)) {
            printf("    %s of %a: want \"%s\", got \"%s\"\n", spec, v, want, got);
            MT_EXPECT(0);
            return;
        }
    }
}

static void hex_floats(void)
{
    char want[100], got[100];
    int i;
    for (i = 0; i < 60000; ++i) {
        uint64_t b = rnd64() & 0x7FFFFFFFFFFFFFFFull;
        double v;
        int prec = (int)(rnd64() % 20);
        char conv = (rnd64() & 1) ? 'a' : 'A';
        if ((b >> 52) == 0x7FF)
            continue;
        memcpy(&v, &b, 8);
        if (rnd64() & 1)
            v = -v;
        if (rnd64() % 4 == 0) {
            snprintf(want, sizeof(want), conv == 'a' ? "%.*a" : "%.*A", prec, v);
            ucrt_snprintf(got, sizeof(got), conv == 'a' ? "%.*a" : "%.*A", prec, v);
            /* glibc prints a denormal's leading digit as 0, as the runtime does; precision below 13 rounds half to even */
        } else {
            snprintf(want, sizeof(want), "%.13a", v);
            ucrt_snprintf(got, sizeof(got), "%.13a", v);
        }
        if (strcmp(want, got)) {
            printf("    hex of %a (prec %d): want \"%s\", got \"%s\"\n", v, prec, want, got);
            MT_EXPECT(0);
            return;
        }
    }
    /* Without a precision the runtime prints all thirteen hex digits. */
    ucrt_snprintf(got, sizeof(got), "%a", 1.0);
    MT_EXPECT(strcmp(got, "0x1.0000000000000p+0") == 0);
    ucrt_snprintf(got, sizeof(got), "%a", 0.0);
    MT_EXPECT(strcmp(got, "0x0.0000000000000p+0") == 0);
}

#define CHECK_FMT(expected, ...)                                                       \
    do {                                                                               \
        char b_[256];                                                                  \
        int r_ = ucrt_snprintf(b_, sizeof(b_), __VA_ARGS__);                           \
        if (strcmp(b_, expected) != 0 || r_ != (int)strlen(expected)) {                \
            printf("    %s: want \"%s\", got \"%s\" (%d)\n", #__VA_ARGS__, expected, b_, r_); \
            MT_EXPECT(0);                                                              \
        }                                                                              \
    } while (0)

static void special_values(void)
{
    CHECK_FMT("inf", "%f", INFINITY);
    CHECK_FMT("-inf", "%f", -INFINITY);
    CHECK_FMT("INF", "%F", INFINITY);
    CHECK_FMT("+inf", "%+f", INFINITY);
    CHECK_FMT("  inf", "%5f", INFINITY);
    CHECK_FMT("inf  |", "%-5f|", INFINITY);
    CHECK_FMT("  inf", "%05f", INFINITY); /* the zero flag does not apply to inf */
    CHECK_FMT("nan", "%f", NAN);
    CHECK_FMT("NAN", "%E", NAN);
    {
        double indefinite;
        uint64_t b = 0xFFF8000000000000ull;
        memcpy(&indefinite, &b, 8);
        CHECK_FMT("-nan(ind)", "%g", indefinite);
        CHECK_FMT("-NAN(IND)", "%G", indefinite);
        b = 0x7FF4000000000000ull;
        memcpy(&indefinite, &b, 8);
        CHECK_FMT("nan(snan)", "%e", indefinite);
        b = 0xFFF0000000000001ull; /* a negative signalling NaN */
        memcpy(&indefinite, &b, 8);
        CHECK_FMT("-nan(snan)", "%f", indefinite);
    }
    CHECK_FMT("-0.000000", "%f", -0.0);
    CHECK_FMT("-0", "%g", -0.0);
    CHECK_FMT("0.000000e+00", "%e", 0.0);
}

static void windows_integer_sizes(void)
{
    CHECK_FMT("9223372036854775807", "%I64d", (long long)INT64_MAX);
    CHECK_FMT("18446744073709551615", "%I64u", (unsigned long long)UINT64_MAX);
    CHECK_FMT("ffffffffffffffff", "%I64x", (unsigned long long)UINT64_MAX);
    CHECK_FMT("-1", "%I32d", -1);
    CHECK_FMT("4294967295", "%I32u", (unsigned)-1);
    CHECK_FMT("123456789012", "%Id", (long long)123456789012ll); /* pointer sized: 64 bits */
    CHECK_FMT("123456789012", "%zu", (size_t)123456789012ull);
    CHECK_FMT("-5", "%ld", -5);                                   /* long is 32 bits */
    CHECK_FMT("4294967295", "%lu", (unsigned)0xFFFFFFFFu);
    CHECK_FMT("-1", "%lld", (long long)-1);
    CHECK_FMT("65535", "%hu", 0x1FFFF);
    CHECK_FMT("-1", "%hhd", 0xFF);
    CHECK_FMT("255", "%hhu", -1);
}

static void pointers_and_n(void)
{
    char b[64];
    CHECK_FMT("0000000000000000", "%p", (void *)0);
    CHECK_FMT("00007FF612345678", "%p", (void *)(uintptr_t)0x7FF612345678ull);
    CHECK_FMT("DEADBEEFDEADBEEF", "%p", (void *)(uintptr_t)0xDEADBEEFDEADBEEFull);
    CHECK_FMT("[0000000000001234]", "[%p]", (void *)(uintptr_t)0x1234);
    CHECK_FMT("[0000000000001234    ]", "[%-20p]", (void *)(uintptr_t)0x1234);
    /* %n is refused unless enabled. */
    {
        int count = 99, r;
        memset(&g_ucrt_state.invalid_count, 0, sizeof(g_ucrt_state.invalid_count));
        r = ucrt_snprintf(b, sizeof(b), "abc%n", &count);
        MT_EXPECT_EQ(r, -1);
        MT_EXPECT_EQ(count, 99);
        MT_EXPECT_EQ(g_ucrt_state.invalid_count, 1);
        g_ucrt_state.printf_count_output = 1;
        r = ucrt_snprintf(b, sizeof(b), "abc%nxyz", &count);
        MT_EXPECT_EQ(r, 6);
        MT_EXPECT_EQ(count, 3);
        g_ucrt_state.printf_count_output = 0;
    }
}

static void strings_and_chars(void)
{
    static const ucrt_wchar wide[] = {'w', 'i', 'd', 'e', 0};
    static const ucrt_wchar wide_nonascii[] = {'a', 0xE9, 'b', 0};
    CHECK_FMT("hello", "%s", "hello");
    CHECK_FMT("[  hello]", "[%7s]", "hello");
    CHECK_FMT("[hello  ]", "[%-7s]", "hello");
    CHECK_FMT("[hel]", "[%.3s]", "hello");
    CHECK_FMT("[   hel]", "[%6.3s]", "hello");
    CHECK_FMT("[hel   ]", "[%-6.3s]", "hello");
    CHECK_FMT("(null)", "%s", (const char *)NULL);
    CHECK_FMT("[ (null)]", "[%7s]", (const char *)NULL);
    CHECK_FMT("[x]", "[%c]", 'x');
    CHECK_FMT("[  x]", "[%3c]", 'x');
    CHECK_FMT("[x  ]", "[%-3c]", 'x');
    CHECK_FMT("wide", "%ls", wide);
    CHECK_FMT("wide", "%ws", wide);
    CHECK_FMT("wide", "%S", wide);
    CHECK_FMT("[  wide]", "[%6ls]", wide);
    CHECK_FMT("a?b", "%ls", wide_nonascii); /* the "C" locale has no mapping for U+00E9 */
    CHECK_FMT("hello", "%hs", "hello");
    CHECK_FMT("W", "%lc", (int)'W');
    CHECK_FMT("W", "%C", (int)'W');
    CHECK_FMT("100%", "%d%%", 100);
    CHECK_FMT("a%b", "a%%b");
    CHECK_FMT("[ab]", "[%*s]", 2, "ab");
    CHECK_FMT("[ab   ]", "[%*s]", -5, "ab");
    CHECK_FMT("[abc]", "[%.*s]", 3, "abcdef");
    CHECK_FMT("[abcdef]", "[%.*s]", -1, "abcdef");
    {
        /* %Z takes an ANSI_STRING: {USHORT Length, MaximumLength; PCHAR Buffer} */
        struct {
            uint16_t len, max;
            uint32_t pad;
            const char *buf;
        } as = {3, 8, 0, "abcdef"};
        struct {
            uint16_t len, max;
            uint32_t pad;
            const ucrt_wchar *buf;
        } us = {4, 8, 0, wide};
        CHECK_FMT("abc", "%Z", &as);
        CHECK_FMT("wi", "%wZ", &us);
        CHECK_FMT("(null)", "%Z", (void *)0);
    }
}

static void bad_formats(void)
{
    char b[32];
    g_ucrt_state.invalid_count = 0;
    MT_EXPECT_EQ(ucrt_snprintf(b, sizeof(b), "%y", 1), -1);
    MT_EXPECT_EQ(ucrt_snprintf(b, sizeof(b), "abc%"), -1);
    MT_EXPECT_EQ(ucrt_snprintf(b, sizeof(b), "abc%5"), -1);
    MT_EXPECT(g_ucrt_state.invalid_count >= 1);
}

static void snprintf_return_rules(void)
{
    char b[8];
    /* C99 snprintf: the full length, always terminated. */
    memset(b, 'x', sizeof(b));
    MT_EXPECT_EQ(ucrt_snprintf(b, 4, "%s", "abcdefgh"), 8);
    MT_EXPECT(memcmp(b, "abc\0xxxx", 8) == 0);
    MT_EXPECT_EQ(ucrt_snprintf(b, 0, "%s", "abc"), 3);
    MT_EXPECT_EQ(ucrt_snprintf(NULL, 0, "%d", 12345), 5);
    /* _snprintf: terminated only when there is room; -1 when the result is longer than the buffer. */
    memset(b, 'x', sizeof(b));
    MT_EXPECT_EQ(ucrt_legacy_snprintf(b, 4, "%s", "abc"), 3);
    MT_EXPECT(memcmp(b, "abc\0xxxx", 8) == 0);
    memset(b, 'x', sizeof(b));
    MT_EXPECT_EQ(ucrt_legacy_snprintf(b, 4, "%s", "abcd"), 4); /* exactly fills: no terminator */
    MT_EXPECT(memcmp(b, "abcdxxxx", 8) == 0);
    memset(b, 'x', sizeof(b));
    MT_EXPECT_EQ(ucrt_legacy_snprintf(b, 4, "%s", "abcdef"), -1);
    MT_EXPECT(memcmp(b, "abcdxxxx", 8) == 0);
    /* sprintf_s */
    {
        va_list unused_ap;
        (void)unused_ap;
    }
}

static int call_vsprintf_s(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vsprintf_s(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

static void sprintf_s_rules(void)
{
    char b[6];
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(call_vsprintf_s(b, sizeof(b), "%s", "abc"), 3);
    MT_EXPECT(strcmp(b, "abc") == 0);
    MT_EXPECT_EQ(call_vsprintf_s(b, sizeof(b), "%s", "abcde"), 5);
    MT_EXPECT_EQ(call_vsprintf_s(b, sizeof(b), "%s", "abcdef"), -1);
    MT_EXPECT_EQ(b[0], 0);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    MT_EXPECT_EQ(call_vsprintf_s(NULL, 6, "x"), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
}

static void exponent_digits(void)
{
    CHECK_FMT("1.000000e+00", "%e", 1.0);
    CHECK_FMT("1.000000e+100", "%e", 1e100);
    CHECK_FMT("1.000000e-05", "%e", 1e-5);
    g_ucrt_state.printf_exp_digits = 3; /* the old msvcrt prints three */
    CHECK_FMT("1.000000e+000", "%e", 1.0);
    CHECK_FMT("1.000000e-005", "%e", 1e-5);
    CHECK_FMT("1.000000e+100", "%e", 1e100);
    CHECK_FMT("1e+100", "%g", 1e100); /* three digits are only a minimum */
    CHECK_FMT("1e-005", "%g", 1e-5);
    g_ucrt_state.printf_exp_digits = 2;
}

/* ---- The wide engine ----------------------------------------------------------------------------- */

static void widen(const char *s, ucrt_wchar *out)
{
    size_t i = 0;
    for (; s[i]; ++i)
        out[i] = (ucrt_wchar)(unsigned char)s[i];
    out[i] = 0;
}

static int wide_equals(const ucrt_wchar *got, const char *want)
{
    size_t i = 0;
    for (; want[i]; ++i)
        if (got[i] != (ucrt_wchar)(unsigned char)want[i])
            return 0;
    return got[i] == 0;
}

static void wide_engine(void)
{
    ucrt_wchar fmt[64], out[128];
    static const ucrt_wchar ws[] = {'W', 'S', 0};
    int r;
    widen("%d|%5.2f|%s|%hs|%c|%x", fmt);
    r = ucrt_swprintf(out, 128, fmt, -42, 3.14159, ws, "narrow", (int)'Z', 255);
    MT_EXPECT_EQ(r, 24);
    MT_EXPECT(wide_equals(out, "-42| 3.14|WS|narrow|Z|ff"));
    /* In the wide engine %S takes a narrow string and %c a wide character. */
    widen("%S|%lc|%C", fmt);
    r = ucrt_swprintf(out, 128, fmt, "abc", (int)'q', (int)'r');
    MT_EXPECT(wide_equals(out, "abc|q|r"));
    /* A wide string above Latin-1 passes through untouched. */
    {
        static const ucrt_wchar euro[] = {0x20AC, 0x4E2D, 0};
        widen("<%s>", fmt);
        r = ucrt_swprintf(out, 128, fmt, euro);
        MT_EXPECT_EQ(r, 4);
        MT_EXPECT(out[1] == 0x20AC && out[2] == 0x4E2D && out[3] == '>');
    }
    /* swprintf returns -1 when the result and its terminator do not fit; _snwprintf fills and does not terminate. */
    widen("%s", fmt);
    MT_EXPECT_EQ(ucrt_swprintf(out, 3, fmt, ws), 2); /* two characters and the terminator fit exactly */
    MT_EXPECT_EQ(ucrt_swprintf(out, 2, fmt, ws), -1);
    MT_EXPECT_EQ(out[0], 0); /* a failed swprintf empties the buffer */
    {
        ucrt_wchar pre[5] = {7, 7, 7, 7, 7};
        MT_EXPECT_EQ(ucrt_legacy_snwprintf(pre, 2, fmt, ws), 2);
        MT_EXPECT(pre[0] == 'W' && pre[1] == 'S' && pre[2] == 7);
    }
    /* The float engine is shared: a wide %g. */
    widen("%g|%e", fmt);
    ucrt_swprintf(out, 128, fmt, 0.0001, 12345.678);
    MT_EXPECT(wide_equals(out, "0.0001|1.234568e+04"));
}

static void rounding_modes_of_g(void)
{
    CHECK_FMT("100000", "%g", 100000.0);
    CHECK_FMT("1e+06", "%g", 1000000.0);
    CHECK_FMT("0.0001", "%g", 0.0001);
    CHECK_FMT("1e-05", "%g", 0.00001);
    CHECK_FMT("123456", "%g", 123456.5);   /* an exact tie rounds to even */
    CHECK_FMT("123458", "%g", 123457.5);
}

static const mt_case_t cases[] = {
    MT_CASE(integers_match_glibc),
    MT_CASE(floats_match_glibc),
    MT_CASE(hex_floats),
    MT_CASE(special_values),
    MT_CASE(windows_integer_sizes),
    MT_CASE(pointers_and_n),
    MT_CASE(strings_and_chars),
    MT_CASE(bad_formats),
    MT_CASE(snprintf_return_rules),
    MT_CASE(sprintf_s_rules),
    MT_CASE(exponent_digits),
    MT_CASE(wide_engine),
    MT_CASE(rounding_modes_of_g),
};

int main(void)
{
    return MT_RUN("crt_printf", cases);
}
