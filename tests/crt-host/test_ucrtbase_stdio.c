/*
 * test_ucrtbase_stdio.c - tests for src/ntdll/ucrtbase_stdio.inc, the __stdio_common_* entry points and the classic printf
 * and scanf names of ucrtbase.dll: how the options word selects snprintf behaviour, wide specifiers and the exponent
 * width, the _s forms, and the stream forms on the POSIX backend.
 */
#include "minitest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "posix_os.h"
#include "ucrt_runtime.h"
#include "ucrt_scanf.h"

#include "../../src/ntdll/ucrtbase_stdio.inc"

static void setup(void)
{
    g_ucrt_os = &posix_os;
    g_ucrt_malloc = malloc;
    g_ucrt_free = free;
    g_ucrt_state.invalid_handler = NULL;
    g_ucrt_state.invalid_count = 0;
    ucrt_set_errno(0);
    g_ucrt_state.printf_iso_wide = 0;
    g_ucrt_state.printf_exp_digits = 2;
}

static void wide_of(const char *s, ucrt_wchar *w)
{
    while ((*w++ = (unsigned char)*s++) != 0)
        ;
}

static void narrow_of(const ucrt_wchar *w, char *s)
{
    while ((*s++ = (char)*w++) != 0)
        ;
}

static void sprintf_unbounded(void)
{
    char b[64];
    setup();
    MT_EXPECT_EQ(w_sprintf(b, "%d-%s-%5.2f", 42, "ab", 3.14159), 11);
    MT_EXPECT(strcmp(b, "42-ab- 3.14") == 0);
}

static void snprintf_standard_returns_needed_length(void)
{
    char b[8];
    setup();
    memset(b, 'x', sizeof b);
    MT_EXPECT_EQ(w_snprintf(b, 4, "%d", 12345), 5);
    MT_EXPECT(strcmp(b, "123") == 0);
    MT_EXPECT_EQ(w_snprintf(NULL, 0, "%s", "hello"), 5);
}

static int call_vsprintf(uint64_t opt, char *b, size_t n, const char *fmt, ...);

static void legacy_snprintf_rules(void)
{
    char b[8];
    setup();
    memset(b, 'x', sizeof b);
    MT_EXPECT_EQ(w__snprintf(b, 4, "%d", 12345), -1);
    MT_EXPECT(b[3] == '4'); /* the buffer is filled and left unterminated */
    memset(b, 'x', sizeof b);
    MT_EXPECT_EQ(w__snprintf(b, 4, "%d", 1234), 4);
    MT_EXPECT(b[3] == '4' && b[4] == 'x'); /* an exact fit has no terminator either */
    MT_EXPECT_EQ(w__snprintf(b, 5, "%d", 1234), 4);
    MT_EXPECT(strcmp(b, "1234") == 0);
    /* option 1 forces a terminator on overflow */
    memset(b, 'x', sizeof b);
    MT_EXPECT_EQ(call_vsprintf(1, b, 4, "%s", "toolong"), -1);
    MT_EXPECT_EQ(b[3], 0);
}

static int call_vsprintf_s(char *b, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w___stdio_common_vsprintf_s(0, b, size, fmt, NULL, ap);
    va_end(ap);
    return r;
}

static int call_vsnprintf_s(char *b, size_t size, size_t max, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w___stdio_common_vsnprintf_s(0, b, size, max, fmt, NULL, ap);
    va_end(ap);
    return r;
}

static void secure_sprintf(void)
{
    char b[8];
    setup();
    MT_EXPECT_EQ(call_vsprintf_s(b, sizeof b, "%d", 1234567), 7);
    MT_EXPECT_EQ(call_vsprintf_s(b, sizeof b, "%d", 12345678), -1); /* does not fit: empties the buffer */
    MT_EXPECT_EQ(b[0], 0);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    MT_EXPECT_EQ(call_vsprintf_s(NULL, 8, "x"), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    MT_EXPECT_EQ((long)g_ucrt_state.invalid_count, 2);
}

static void secure_snprintf_count_rules(void)
{
    char b[8];
    setup();
    memset(b, 'x', sizeof b);
    MT_EXPECT_EQ(call_vsnprintf_s(b, sizeof b, 3, "%s", "hello"), -1); /* more than count: cut at count */
    MT_EXPECT(strcmp(b, "hel") == 0);
    MT_EXPECT_EQ(call_vsnprintf_s(b, sizeof b, 5, "%s", "hello"), 5);
    MT_EXPECT(strcmp(b, "hello") == 0);
    MT_EXPECT_EQ(call_vsnprintf_s(b, sizeof b, (size_t)-1, "%s", "truncated!"), -1); /* _TRUNCATE */
    MT_EXPECT(strcmp(b, "truncat") == 0);
    MT_EXPECT_EQ(call_vsnprintf_s(b, sizeof b, (size_t)-1, "%s", "fits"), 4);
    MT_EXPECT_EQ((long)g_ucrt_state.invalid_count, 0);
    MT_EXPECT_EQ(call_vsnprintf_s(b, sizeof b, 20, "%s", "toolongforbuf"), -1); /* count allows it, the buffer does not */
    MT_EXPECT_EQ(b[0], 0);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
}

static int call_vswprintf(uint64_t opt, ucrt_wchar *b, size_t n, const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w___stdio_common_vswprintf(opt, b, n, fmt, NULL, ap);
    va_end(ap);
    return r;
}

static void wide_specifiers_follow_the_option(void)
{
    ucrt_wchar fmt[8], out[32], wide_arg[8];
    char narrow[32];
    setup();
    wide_of("%s", fmt);
    wide_of("wide", wide_arg);
    /* the Microsoft default (option 4): %s in a wide function takes a wide string */
    MT_EXPECT_EQ(call_vswprintf(UCT_PF_MS_WIDE, out, 32, fmt, wide_arg), 4);
    narrow_of(out, narrow);
    MT_EXPECT(strcmp(narrow, "wide") == 0);
    /* without it (ISO): %s is a narrow string */
    MT_EXPECT_EQ(call_vswprintf(0, out, 32, fmt, "narrow"), 6);
    narrow_of(out, narrow);
    MT_EXPECT(strcmp(narrow, "narrow") == 0);
    /* the state is put back after each call */
    MT_EXPECT_EQ(g_ucrt_state.printf_iso_wide, 0);
    MT_EXPECT_EQ(g_ucrt_state.printf_exp_digits, 2);
}

static int call_vsprintf(uint64_t opt, char *b, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w___stdio_common_vsprintf(opt, b, n, fmt, NULL, ap);
    va_end(ap);
    return r;
}

static void exponent_width_option(void)
{
    char b[32];
    setup();
    call_vsprintf(UCT_PF_STANDARD_SNPRINTF, b, sizeof b, "%e", 1.0);
    MT_EXPECT(strcmp(b, "1.000000e+00") == 0);
    call_vsprintf(UCT_PF_STANDARD_SNPRINTF | UCT_PF_THREE_DIGIT_EXP, b, sizeof b, "%e", 1.0);
    MT_EXPECT(strcmp(b, "1.000000e+000") == 0);
    MT_EXPECT_EQ(g_ucrt_state.printf_exp_digits, 2);
}

static void wide_snprintf_modes(void)
{
    ucrt_wchar fmt[8], out[8];
    setup();
    wide_of("%d", fmt);
    MT_EXPECT_EQ(call_vswprintf(UCT_PF_STANDARD_SNPRINTF, out, 8, fmt, 123), 3);
    MT_EXPECT_EQ(call_vswprintf(UCT_PF_STANDARD_SNPRINTF, out, 3, fmt, 123), -1); /* swprintf: the terminator must fit */
    MT_EXPECT_EQ(call_vswprintf(0, out, 3, fmt, 123), 3);                          /* _snwprintf: an exact fit is fine */
    MT_EXPECT_EQ(call_vswprintf(0, out, 2, fmt, 123), -1);
}

static void count_functions(void)
{
    ucrt_wchar wfmt[8];
    setup();
    MT_EXPECT_EQ(w__scprintf("%s-%d", "ab", 7), 4);
    wide_of("%d%d", wfmt);
    MT_EXPECT_EQ(w__scwprintf(wfmt, 12, 34), 4);
}

static void scanf_family(void)
{
    int a = 0, n = 0;
    char word[8], two[3];
    setup();
    MT_EXPECT_EQ(w_sscanf("12 hello", "%d %s", &a, word), 2);
    MT_EXPECT(a == 12 && strcmp(word, "hello") == 0);
    MT_EXPECT_EQ(w_sscanf_s("7 ab", "%d %s", &a, two, (unsigned)sizeof two), 2);
    MT_EXPECT(a == 7 && strcmp(two, "ab") == 0);
    /* the secure form refuses a string that does not fit its size */
    MT_EXPECT_EQ(w_sscanf_s("7 abcdef", "%d %s", &a, two, (unsigned)sizeof two), 1);
    MT_EXPECT_EQ(w_sscanf("5", "%d%n", &a, &n), 1);
    MT_EXPECT_EQ(n, 1);
}

static void stream_forms(void)
{
    char dir[64], path[96], line[64];
    ucrt_file *f;
    int a = 0;
    double d = 0;
    setup();
    snprintf(dir, sizeof dir, "/tmp/crt_ucb_stdio_%d", (int)getpid());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/a.txt", dir);
    f = ucrt_fopen(path, "w");
    MT_ASSERT(f != NULL);
    MT_EXPECT_EQ(w_fprintf(f, "%d %g\n", 9, 2.5), 6);
    MT_EXPECT_EQ(ucrt_fclose(f), 0);
    f = ucrt_fopen(path, "r");
    MT_ASSERT(f != NULL);
    MT_EXPECT_EQ(w_fscanf(f, "%d %lf", &a, &d), 2);
    MT_EXPECT(a == 9 && d == 2.5);
    ucrt_fclose(f);
    f = ucrt_fopen(path, "r");
    MT_ASSERT(f != NULL);
    MT_ASSERT(ucrt_fgets(line, sizeof line, f) != NULL);
    MT_EXPECT(strcmp(line, "9 2.5\n") == 0);
    ucrt_fclose(f);
    unlink(path);
    rmdir(dir);
}

static void count_output_switch(void)
{
    int n = -1;
    char b[16];
    setup();
    MT_EXPECT_EQ(w__get_printf_count_output(), 0);
    MT_EXPECT_EQ(w__set_printf_count_output(1), 0);
    MT_EXPECT_EQ(w_sprintf(b, "ab%n", &n), 2);
    MT_EXPECT_EQ(n, 2);
    w__set_printf_count_output(0);
}

static const mt_case_t cases[] = {
    MT_CASE(sprintf_unbounded), MT_CASE(snprintf_standard_returns_needed_length), MT_CASE(legacy_snprintf_rules),
    MT_CASE(secure_sprintf), MT_CASE(secure_snprintf_count_rules), MT_CASE(wide_specifiers_follow_the_option),
    MT_CASE(exponent_width_option), MT_CASE(wide_snprintf_modes), MT_CASE(count_functions), MT_CASE(scanf_family),
    MT_CASE(stream_forms), MT_CASE(count_output_switch),
};

int main(void)
{
    return MT_RUN("ucrtbase_stdio", cases);
}
