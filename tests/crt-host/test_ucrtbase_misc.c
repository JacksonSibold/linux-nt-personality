/*
 * test_ucrtbase_misc.c - tests for src/ntdll/ucrtbase_misc.inc: the number-to-text conversions (against sprintf and by hand),
 * _ecvt/_fcvt/_gcvt, the restartable multibyte conversions, error text, character class masks and the static-buffer time
 * functions.
 */
#include "minitest.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix_os.h"
#include "ucrt_runtime.h"
#include "ucrt_math.h"
#include "ucrt_time.h"
#include "ucrt_conv.h"
#include "ucrt_scanf.h"

#include "../../src/ntdll/ucrtbase_thread.h"

static uct_thread g_thread;
static uct_thread *uct_thr(void) { return &g_thread; }
static void *w_malloc(size_t n) { return malloc(n); }

#include "../../src/ntdll/ucrtbase_misc.inc"

static void setup(void)
{
    g_ucrt_os = &posix_os;
    g_ucrt_malloc = malloc;
    g_ucrt_free = free;
    g_ucrt_state.invalid_handler = NULL;
    g_ucrt_state.invalid_count = 0;
    ucrt_set_errno(0);
    ucrt_setlocale(UCRT_LC_ALL, "C");
}

static void integer_to_text_against_printf(void)
{
    static const int64_t vals[] = {0, 1, -1, 7, -7, 255, 4096, -123456, 2147483647, -2147483647 - 1, 123456789012345ll, INT64_MIN};
    char a[80], b[80];
    setup();
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        snprintf(b, sizeof b, "%" PRId64, vals[i]);
        w__i64toa(vals[i], a, 10);
        MT_EXPECT(strcmp(a, b) == 0);
        snprintf(b, sizeof b, "%" PRIx64, (uint64_t)vals[i]);
        w__ui64toa((uint64_t)vals[i], a, 16);
        MT_EXPECT(strcmp(a, b) == 0);
        snprintf(b, sizeof b, "%" PRIu64, (uint64_t)vals[i]);
        w__ui64toa((uint64_t)vals[i], a, 10);
        MT_EXPECT(strcmp(a, b) == 0);
    }
    w__itoa(-255, a, 16); /* a negative value in a radix other than 10 prints as unsigned */
    MT_EXPECT(strcmp(a, "ffffff01") == 0);
    w__itoa(-255, a, 10);
    MT_EXPECT(strcmp(a, "-255") == 0);
    w__ltoa(35, a, 36);
    MT_EXPECT(strcmp(a, "z") == 0);
    w__ultoa(4000000000ul, a, 10);
    MT_EXPECT(strcmp(a, "4000000000") == 0);
    w__i64toa(5, a, 2);
    MT_EXPECT(strcmp(a, "101") == 0);
}

static void integer_to_text_checks_its_buffer(void)
{
    char a[4];
    setup();
    MT_EXPECT_EQ(w__itoa_s(1234, a, sizeof a, 10), UCRT_ERANGE);
    MT_EXPECT_EQ(a[0], 0);
    MT_EXPECT_EQ(w__itoa_s(123, a, sizeof a, 10), 0);
    MT_EXPECT(strcmp(a, "123") == 0);
    MT_EXPECT_EQ(w__itoa_s(1, a, sizeof a, 1), UCRT_EINVAL); /* radix out of range */
    MT_EXPECT_EQ(w__itoa_s(1, NULL, 4, 10), UCRT_EINVAL);
}

static void wide_integer_to_text(void)
{
    ucrt_wchar w[40];
    char n[40];
    setup();
    w__i64tow(-9876543210ll, w, 10);
    for (size_t i = 0;; ++i) {
        n[i] = (char)w[i];
        if (!w[i])
            break;
    }
    MT_EXPECT(strcmp(n, "-9876543210") == 0);
    MT_EXPECT_EQ(w__ultow_s(255, w, 2, 16), UCRT_ERANGE);
    MT_EXPECT_EQ(w__ultow_s(255, w, 3, 16), 0);
    MT_EXPECT(w[0] == 'f' && w[1] == 'f' && w[2] == 0);
}

static void cvt_functions(void)
{
    int dec, sign;
    char *s;
    setup();
    s = w__ecvt(123.456, 5, &dec, &sign);
    MT_EXPECT(strcmp(s, "12346") == 0 && dec == 3 && sign == 0);
    s = w__ecvt(-0.00123, 3, &dec, &sign);
    MT_EXPECT(strcmp(s, "123") == 0 && dec == -2 && sign == 1);
    s = w__ecvt(5.0, 4, &dec, &sign);
    MT_EXPECT(strcmp(s, "5000") == 0 && dec == 1);
    s = w__fcvt(123.456, 2, &dec, &sign);
    MT_EXPECT(strcmp(s, "12346") == 0 && dec == 3);
    s = w__fcvt(1234.5, 0, &dec, &sign);
    MT_EXPECT(strcmp(s, "1235") == 0 || strcmp(s, "1234") == 0);
    MT_EXPECT_EQ(dec, 4);
    s = w__ecvt(0.0, 3, &dec, &sign);
    MT_EXPECT(strcmp(s, "000") == 0 && dec == 0);
}

static void gcvt_matches_the_microsoft_forms(void)
{
    char b[64];
    setup();
    MT_EXPECT(strcmp(w__gcvt(1.0, 5, b), "1.") == 0);
    MT_EXPECT(strcmp(w__gcvt(0.5, 5, b), "0.5") == 0);
    MT_EXPECT(strcmp(w__gcvt(123456.0, 3, b), "1.23e+05") == 0);
    MT_EXPECT(strcmp(w__gcvt(-2.25, 6, b), "-2.25") == 0);
    MT_EXPECT_EQ(w__gcvt_s(b, 3, 12345.0, 8), UCRT_ERANGE);
}

static void error_text(void)
{
    char b[64];
    setup();
    MT_EXPECT(strcmp(w_strerror(2), "No such file or directory") == 0);
    ucrt_set_errno(UCRT_EINVAL);
    MT_EXPECT(strcmp(w__strerror("open"), "open: Invalid argument\n") == 0);
    MT_EXPECT(strcmp(w__strerror(NULL), "Invalid argument\n") == 0);
    MT_EXPECT_EQ(w_strerror_s(b, sizeof b, 2), 0);
    MT_EXPECT(strcmp(b, "No such file or directory") == 0);
    MT_EXPECT_EQ(w_strerror_s(b, 5, 2), UCRT_ERANGE);
    MT_EXPECT(strcmp(b, "No s") == 0);
    {
        ucrt_wchar *w = w__wcserror(2);
        MT_EXPECT(w[0] == 'N' && w[1] == 'o');
    }
}

static void tokens_are_per_thread_state(void)
{
    char s[] = "a,b;;c";
    setup();
    MT_EXPECT(strcmp(w_strtok(s, ",;"), "a") == 0);
    MT_EXPECT(strcmp(w_strtok(NULL, ",;"), "b") == 0);
    MT_EXPECT(strcmp(w_strtok(NULL, ",;"), "c") == 0);
    MT_EXPECT(w_strtok(NULL, ",;") == NULL);
}

static void wide_memory_functions(void)
{
    ucrt_wchar a[5] = {1, 2, 3, 4, 5}, b[5];
    setup();
    w_wmemcpy(b, a, 5);
    MT_EXPECT_EQ(w_wmemcmp(a, b, 5), 0);
    b[3] = 9;
    MT_EXPECT_EQ(w_wmemcmp(a, b, 5), -1);
    MT_EXPECT(w_wmemchr(a, 3, 5) == a + 2 && w_wmemchr(a, 8, 5) == NULL);
    w_wmemset(b, 7, 2);
    MT_EXPECT(b[0] == 7 && b[1] == 7 && b[2] == 3);
    w_wmemmove(a + 1, a, 4);
    MT_EXPECT(a[0] == 1 && a[1] == 1 && a[4] == 4);
    MT_EXPECT_EQ(w_wmemcpy_s(b, 2, a, 3), UCRT_ERANGE);
}

static void character_class_masks(void)
{
    const unsigned short *t;
    setup();
    MT_EXPECT(w__isctype('a', 0x103) != 0 && w__isctype('1', 0x103) == 0);
    MT_EXPECT(w__isctype('F', UCRT_HEX) != 0 && w__isctype('G', UCRT_HEX) == 0);
    MT_EXPECT(w_iswctype('Z', UCRT_UPPER) != 0 && w_iswctype('z', UCRT_UPPER) == 0);
    MT_EXPECT(w_iswctype(0x4E2D, 0x100) != 0); /* CJK is alphabetic */
    MT_EXPECT(w_iswctype(' ', UCRT_SPACE | UCRT_BLANK) == (UCRT_SPACE | UCRT_BLANK));
    t = w___pwctype_func();
    MT_EXPECT((t['A'] & UCRT_UPPER) && (t['5'] & UCRT_DIGIT) && t[-1] == 0);
    t = w___pctype_func();
    MT_EXPECT((t['a'] & UCRT_LOWER) && (t[' '] & UCRT_SPACE) && t[-1] == 0);
}

static void restartable_multibyte(void)
{
    const char *s = "héllo"; /* UTF-8 */
    ucrt_wchar w[16];
    char out[32];
    const ucrt_wchar *ws = w;
    const char *src;
    setup();
    ucrt_setlocale(UCRT_LC_ALL, ".UTF8");
    src = s;
    MT_EXPECT_EQ((long)w_mbsrtowcs(NULL, &src, 0, NULL), 5);
    MT_EXPECT(src == s); /* counting leaves the source alone */
    MT_EXPECT_EQ((long)w_mbsrtowcs(w, &src, 16, NULL), 5);
    MT_EXPECT(src == NULL && w[1] == 0xE9 && w[4] == 'o' && w[5] == 0);
    src = s;
    MT_EXPECT_EQ((long)w_mbsrtowcs(w, &src, 2, NULL), 2); /* stops when the buffer is full, with src after the last one used */
    MT_EXPECT(src == s + 3);
    ws = w;
    w[0] = 'h';
    w[1] = 0xE9;
    w[2] = 0;
    MT_EXPECT_EQ((long)w_wcsrtombs(NULL, &ws, 0, NULL), 3);
    MT_EXPECT_EQ((long)w_wcsrtombs(out, &ws, 32, NULL), 3);
    MT_EXPECT(ws == NULL && strcmp(out, "h\xC3\xA9") == 0);
    ws = w;
    MT_EXPECT_EQ((long)w_wcsrtombs(out, &ws, 2, NULL), 1); /* the 2-byte character does not fit */
    MT_EXPECT(ws == w + 1);
    ucrt_setlocale(UCRT_LC_ALL, "C");
}

static uint64_t t_now(void) { return 116444736000000000ull + 5ull * 10000000ull; }
static int t_get_tz(ucrt_tzinfo *z)
{
    memset(z, 0, sizeof *z);
    strcpy(z->std_name, "UTC");
    strcpy(z->dst_name, "UTD");
    return 0;
}
static uint64_t t_proc(void) { return 0; }
static const char *t_getenv(const char *n) { (void)n; return NULL; }
static const ucrt_time_ops tops = {t_now, t_get_tz, t_proc, t_getenv};

static void time_functions_use_the_thread_buffer(void)
{
    int64_t t = 86400ll * 365; /* 1971-01-01 */
    ucrt_tm *tm;
    char *c;
    long tz = -1;
    ucrt_timeb tb;
    setup();
    g_ucrt_time_ops = &tops;
    g_ucrt_tz.loaded = 0;
    tm = w__gmtime64(&t);
    MT_ASSERT(tm != NULL);
    MT_EXPECT(tm->tm_year == 71 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_wday == 5);
    c = w__ctime64(&t);
    MT_ASSERT(c != NULL);
    MT_EXPECT(strcmp(c, "Fri Jan  1 00:00:00 1971\n") == 0);
    MT_EXPECT(strcmp(w_asctime(tm), "Fri Jan  1 00:00:00 1971\n") == 0);
    t = -1;
    MT_EXPECT(w__gmtime64(&t) == NULL);
    MT_EXPECT_EQ(w__get_timezone(&tz), 0);
    MT_EXPECT_EQ(tz, 0);
    MT_EXPECT_EQ(w__ftime64_s(&tb), 0);
    MT_EXPECT(tb.time == 5 && tb.millitm == 0);
    {
        int dst = -1;
        MT_EXPECT_EQ(w__get_daylight(&dst), 0);
        MT_EXPECT_EQ(dst, 0);
    }
    {
        size_t len = 0;
        char name[8];
        MT_EXPECT_EQ(w__get_tzname(&len, name, sizeof name, 0), 0);
        MT_EXPECT_EQ(w__get_tzname(&len, name, 1, 0), UCRT_ERANGE);
        MT_EXPECT_EQ(w__get_tzname(&len, name, sizeof name, 2), UCRT_EINVAL);
    }
    g_ucrt_time_ops = NULL;
}

static const mt_case_t cases[] = {
    MT_CASE(integer_to_text_against_printf), MT_CASE(integer_to_text_checks_its_buffer), MT_CASE(wide_integer_to_text),
    MT_CASE(cvt_functions), MT_CASE(gcvt_matches_the_microsoft_forms), MT_CASE(error_text), MT_CASE(tokens_are_per_thread_state),
    MT_CASE(wide_memory_functions), MT_CASE(character_class_masks), MT_CASE(restartable_multibyte),
    MT_CASE(time_functions_use_the_thread_buffer),
};

int main(void)
{
    return MT_RUN("ucrtbase_misc", cases);
}
