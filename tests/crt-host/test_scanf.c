/*
 * test_scanf.c - tests for src/crt/ucrt_scanf.h: a differential test of the sscanf engine against glibc on random
 * inputs for many formats, then the Microsoft-specific parts (_s functions, wide strings, fscanf on a stream).
 */
#include "minitest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix_os.h"
#include "ucrt_scanf.h"

#define MT_FAIL(...) do { char fb_[400]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static uint64_t rng = 0x94D049BB133111EBull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const char *const tokens[] = {
    "12", "-7", "+3", "0", "0x1f", "0X7F", "0755", "08", "3.14", "-2.5e3", ".5", "5.", "1e5", "1E-3", "42", "100",
    "abc", "hello", "w0rld", "x", "a,b", ",", ";", "%", " ", "  ", "\t", "\n", "-", "+", "(", ")", "z", "007",
    "123456", "99", "1", "2", "ff", "0.000123", "6.02e23", "-0", "q", "e", "E", "1e+",
};

static void gen_input(char *out, size_t cap)
{
    size_t n = 0, parts = 1 + rnd64() % 6;
    out[0] = 0;
    for (size_t i = 0; i < parts; ++i) {
        const char *t = tokens[rnd64() % (sizeof tokens / sizeof tokens[0])];
        size_t l = strlen(t);
        if (n + l + 2 >= cap)
            break;
        memcpy(out + n, t, l);
        n += l;
        if (rnd64() % 3 == 0)
            out[n++] = ' ';
        out[n] = 0;
    }
}

static const char *const formats[] = {
    "%d", "%d %d", "%i", "%i %i", "%u", "%o", "%x", "%X %x", "%hd", "%hhd", "%lld", "%5d", "%3x", "%2d%2d",
    "%d,%d", "%d %*d %d", "%*d", "%d%%", "%d%n", "%f", "%lf", "%e", "%g", "%lf %lf", "%5f", "%3lf", "%s", "%s %s",
    "%5s", "%2s%2s", "%c", "%3c", "%c%c", "%[a-z]", "%[^ ]", "%[0-9]%[a-z]", "%[^,]", "%[abc]x", "%s%d", "%d%s",
    "%d %s %lf", "abc%d", "a b %d", " %d", "%d %c", "%x%x", "%i%s", "%*s %d", "%[a-z] %d", "%*[a-z]%d", "%1d%1d%1d",
    "%d-%d", "%lf%s", "%f%f", "%4[0-9]", "%[^\n]", "%n%d", "%d%n%d", "%hu", "%hx", "%llx", "%o%o",
};

/* glibc consumes an exponent mark that has no digits after it ("1e" is read whole); the Microsoft runtime leaves it
 * unread, so inputs like that are not compared. */
static int has_dangling_exponent(const char *s)
{
    for (const char *p = s; *p; ++p) {
        if ((*p == 'e' || *p == 'E') && p > s && ((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.')) {
            const char *q = p + 1;
            if (*q == '+' || *q == '-')
                ++q;
            if (!(*q >= '0' && *q <= '9'))
                return 1;
        }
    }
    return 0;
}

/* An integer too big for its type wraps here and clamps in glibc; "0x" with no digits is the number 0 followed by an
 * x here, and a failure in glibc. Neither is compared. */
static int has_long_digit_run(const char *s)
{
    int run = 0;
    for (const char *p = s; *p; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
            ++run;
        else
            run = 0;
        if (run >= 9)
            return 1;
    }
    return 0;
}

static void bit_compare(const char *fmt, const char *input, int round)
{
    unsigned char ou[4][64], og[4][64];
    int ru, rg;
    memset(ou, 0xAA, sizeof ou);
    memset(og, 0xAA, sizeof og);
    ru = ucrt_sscanf(input, fmt, ou[0], ou[1], ou[2], ou[3]);
    rg = sscanf(input, fmt, og[0], og[1], og[2], og[3]);
    if (ru != rg || memcmp(ou, og, sizeof ou) != 0) {
        char esc[200];
        size_t k = 0;
        for (const char *p = input; *p && k + 4 < sizeof esc; ++p)
            k += (size_t)snprintf(esc + k, sizeof esc - k, *p == '\n' ? "\\n" : *p == '\t' ? "\\t" : "%c", *p);
        MT_FAIL("round %d: sscanf(\"%s\", \"%s\") = %d, glibc %d (outputs %s)", round, esc, fmt, ru, rg,
                memcmp(ou, og, sizeof ou) ? "differ" : "same");
    }
}

static void differential_sscanf(void)
{
    int fails_before = mt_case_failures;
    for (int round = 0; round < 200000 && mt_case_failures - fails_before < 12; ++round) {
        char input[160];
        const char *fmt = formats[rnd64() % (sizeof formats / sizeof formats[0])];
        gen_input(input, sizeof input);
        if (strpbrk(fmt, "feg") && (has_dangling_exponent(input) || strstr(input, "0x") || strstr(input, "0X")) &&
            !strstr(fmt, "%x") && !strstr(fmt, "%X") && !strstr(fmt, "%i"))
            continue;
        if (strpbrk(fmt, "dioxXu") && has_long_digit_run(input))
            continue;
        bit_compare(fmt, input, round);
    }
}

static void int_conversions(void)
{
    int a = 0, b = 0, n = 0;
    unsigned u = 0;
    long long ll = 0;
    short sh = 0;
    signed char ch = 0;
    unsigned long long q = 0;
    MT_EXPECT_EQ(ucrt_sscanf("  42 -17", "%d %d", &a, &b), 2);
    MT_EXPECT(a == 42 && b == -17);
    MT_EXPECT_EQ(ucrt_sscanf("0x1F 017 99", "%i %i %i", &a, &b, &n), 3);
    MT_EXPECT(a == 31 && b == 15 && n == 99);
    MT_EXPECT_EQ(ucrt_sscanf("ff", "%x", &u), 1);
    MT_EXPECT_EQ(u, 255);
    MT_EXPECT_EQ(ucrt_sscanf("12345678901234", "%lld", &ll), 1);
    MT_EXPECT(ll == 12345678901234ll);
    MT_EXPECT_EQ(ucrt_sscanf("123456789012", "%I64d", &ll), 1);
    MT_EXPECT(ll == 123456789012ll);
    MT_EXPECT_EQ(ucrt_sscanf("70000", "%hd", &sh), 1);
    MT_EXPECT_EQ(sh, (short)70000);
    MT_EXPECT_EQ(ucrt_sscanf("300", "%hhd", &ch), 1);
    MT_EXPECT_EQ(ch, (signed char)300);
    MT_EXPECT_EQ(ucrt_sscanf("ffffffffffffffff", "%llx", &q), 1);
    MT_EXPECT(q == 0xFFFFFFFFFFFFFFFFull);
    MT_EXPECT_EQ(ucrt_sscanf("abc", "%d", &a), 0);
    MT_EXPECT_EQ(ucrt_sscanf("", "%d", &a), UCRT_EOF);
    MT_EXPECT_EQ(ucrt_sscanf("   ", "%d", &a), UCRT_EOF);
    MT_EXPECT_EQ(ucrt_sscanf("7 x", "%d %d", &a, &b), 1);
    MT_EXPECT_EQ(ucrt_sscanf("5 6", "%d%n", &a, &n), 1);
    MT_EXPECT_EQ(n, 1);
    MT_EXPECT_EQ(ucrt_sscanf("12:34", "%d:%d", &a, &b), 2);
    MT_EXPECT(a == 12 && b == 34);
    MT_EXPECT_EQ(ucrt_sscanf("1234", "%2d%2d", &a, &b), 2);
    MT_EXPECT(a == 12 && b == 34);
    /* an overflowing value wraps, as it does in the Microsoft runtime */
    MT_EXPECT_EQ(ucrt_sscanf("4294967297", "%u", &u), 1);
    MT_EXPECT_EQ(u, 1);
}

static void float_conversions(void)
{
    double d = 0, e = 0;
    float f = 0;
    MT_EXPECT_EQ(ucrt_sscanf("3.25 -1e2", "%lf %lf", &d, &e), 2);
    MT_EXPECT(d == 3.25 && e == -100.0);
    MT_EXPECT_EQ(ucrt_sscanf("0.1", "%f", &f), 1);
    MT_EXPECT(f == 0.1f); /* rounded straight to float, not through double */
    MT_EXPECT_EQ(ucrt_sscanf("1e", "%lf", &d), 1);
    MT_EXPECT(d == 1.0);
    MT_EXPECT_EQ(ucrt_sscanf("inf nan", "%lf %lf", &d, &e), 2);
    MT_EXPECT(d > 1e308 && e != e);
    MT_EXPECT_EQ(ucrt_sscanf("0x1.8p1", "%lf", &d), 1);
    MT_EXPECT(d == 3.0);
    MT_EXPECT_EQ(ucrt_sscanf("2.5", "%Lf", &d), 1); /* a long double is a double here */
    MT_EXPECT(d == 2.5);
    MT_EXPECT_EQ(ucrt_sscanf("1.5abc", "%lf%s", &d, (char[8]){0}), 2);
    MT_EXPECT_EQ(ucrt_sscanf("123456789012345678901234567890", "%lf", &d), 1);
    MT_EXPECT(d == 123456789012345678901234567890.0);
    MT_EXPECT_EQ(ucrt_sscanf(".", "%lf", &d), 0);
    MT_EXPECT_EQ(ucrt_sscanf("-", "%lf", &d), 0);
}

static void string_conversions(void)
{
    char s[16], t[16];
    int n = 0;
    MT_EXPECT_EQ(ucrt_sscanf("hello world", "%s %s", s, t), 2);
    MT_EXPECT(strcmp(s, "hello") == 0 && strcmp(t, "world") == 0);
    MT_EXPECT_EQ(ucrt_sscanf("abcdef", "%3s%s", s, t), 2);
    MT_EXPECT(strcmp(s, "abc") == 0 && strcmp(t, "def") == 0);
    MT_EXPECT_EQ(ucrt_sscanf("a,b,c", "%[^,],%[^,]", s, t), 2);
    MT_EXPECT(strcmp(s, "a") == 0 && strcmp(t, "b") == 0);
    MT_EXPECT_EQ(ucrt_sscanf("12abc", "%[0-9]%[a-z]", s, t), 2);
    MT_EXPECT(strcmp(s, "12") == 0 && strcmp(t, "abc") == 0);
    MT_EXPECT_EQ(ucrt_sscanf("]a]", "%[]a]", s), 1); /* a ] first is a member */
    MT_EXPECT(strcmp(s, "]a]") == 0);
    memset(s, 'x', sizeof s);
    MT_EXPECT_EQ(ucrt_sscanf("abcd", "%2c", s), 1); /* %c does not add a terminator */
    MT_EXPECT(s[0] == 'a' && s[1] == 'b' && s[2] == 'x');
    MT_EXPECT_EQ(ucrt_sscanf("  q", "%c", s), 1); /* and does not skip white space */
    MT_EXPECT(s[0] == ' ');
    MT_EXPECT_EQ(ucrt_sscanf("x", "%2c", s), 1); /* fewer characters than the width: the ones there are */
    MT_EXPECT(s[0] == 'x');
    MT_EXPECT_EQ(ucrt_sscanf("name=Bob 7", "name=%s %n", s, &n), 1);
    MT_EXPECT_EQ(n, 9);
}

static void secure_functions(void)
{
    char s[8] = "zzzzzzz", t[4];
    int n = 0;
    MT_EXPECT_EQ(ucrt_sscanf_s("hello", "%s", s, (unsigned)sizeof s), 1);
    MT_EXPECT(strcmp(s, "hello") == 0);
    ucrt_set_errno(0);
    MT_EXPECT_EQ(ucrt_sscanf_s("toolongstring", "%s", t, (unsigned)sizeof t), 0); /* does not fit: nothing stored */
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    MT_EXPECT_EQ(t[0], 0);
    MT_EXPECT_EQ(ucrt_sscanf_s("ab 5", "%2c %d", t, 2u, &n), 2);
    MT_EXPECT(t[0] == 'a' && t[1] == 'b' && n == 5);
    MT_EXPECT_EQ(ucrt_sscanf_s("abc", "%[a-z]", t, 3u), 0);
}

static void wide_engine(void)
{
    static const ucrt_wchar in[] = {'1', '2', ' ', 'h', 'i', ' ', '3', '.', '5', 0};
    static const ucrt_wchar fmt[] = {'%', 'd', ' ', '%', 's', ' ', '%', 'l', 'f', 0};
    static const ucrt_wchar fmt2[] = {'%', 'd', ' ', '%', 'S', 0};
    int n = 0;
    ucrt_wchar w[8];
    char c[8];
    double d = 0;
    MT_EXPECT_EQ(ucrt_swscanf(in, fmt, &n, w, &d), 3); /* %s is a wide string here */
    MT_EXPECT(n == 12 && w[0] == 'h' && w[1] == 'i' && w[2] == 0 && d == 3.5);
    MT_EXPECT_EQ(ucrt_swscanf(in, fmt2, &n, c), 2); /* %S is a narrow string */
    MT_EXPECT(n == 12 && strcmp(c, "hi") == 0);
    {
        static const ucrt_wchar fmt3[] = {'%', 's', 0};
        MT_EXPECT_EQ(ucrt_sscanf("abc", "%ls", w), 1); /* a wide string from a narrow scan */
        MT_EXPECT(w[0] == 'a' && w[2] == 'c' && w[3] == 0);
        (void)fmt3;
    }
}

static void stream_scanning(void)
{
    ucrt_file *f;
    FILE *raw;
    int a = 0, b = 0, n;
    char s[16];
    double d;
    g_ucrt_os = &posix_os;
    g_ucrt_malloc = malloc;
    g_ucrt_free = free;
    g_ucrt_io.initialised = 0;
    g_ucrt_stdio.initialised = 0;
    ucrt_stdio_init();
    raw = fopen("/tmp/crt_scanf_stream.txt", "wb");
    fputs("10 20\r\nword 2.5\r\nlast", raw);
    fclose(raw);
    f = ucrt_fopen("/tmp/crt_scanf_stream.txt", "r");
    MT_ASSERT(f != NULL);
    MT_EXPECT_EQ(ucrt_fscanf(f, "%d %d", &a, &b), 2);
    MT_EXPECT(a == 10 && b == 20);
    MT_EXPECT_EQ(ucrt_fscanf(f, " %s %lf", s, &d), 2);
    MT_EXPECT(strcmp(s, "word") == 0 && d == 2.5);
    MT_EXPECT_EQ(ucrt_fscanf(f, "%s", s), 1);
    MT_EXPECT(strcmp(s, "last") == 0);
    MT_EXPECT_EQ(ucrt_fscanf(f, "%d", &a), UCRT_EOF);
    MT_EXPECT_EQ(ucrt_feof(f), 1);
    ucrt_rewind(f);
    /* a failed match leaves the offending character in the stream */
    MT_EXPECT_EQ(ucrt_fscanf(f, "%d %d x", &a, &b), 2);
    MT_EXPECT_EQ(ucrt_fgetc(f), 'w'); /* the space in the format ate the line end; the x did not match 'w', which stays */
    (void)n;
    ucrt_fclose(f);
    remove("/tmp/crt_scanf_stream.txt");
}

static const mt_case_t cases[] = {
    MT_CASE(int_conversions), MT_CASE(float_conversions), MT_CASE(string_conversions), MT_CASE(secure_functions),
    MT_CASE(wide_engine), MT_CASE(stream_scanning), MT_CASE(differential_sscanf),
};

int main(void)
{
    return MT_RUN("crt_scanf", cases);
}
