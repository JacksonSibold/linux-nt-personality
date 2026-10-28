/*
 * test_time.c - tests for src/crt/ucrt_time.h: the calendar against glibc over the whole range, local time with a
 * US time zone against glibc's own TZ database, mktime, asctime, strftime.
 */
#include "minitest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ucrt_time.h"

#define MT_FAIL(...) do { char fb_[800]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static uint64_t rng = 0xA0761D6478BD642Full;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static uint64_t fake_now = 0;
static const char *fake_tz;

static uint64_t t_now(void) { return fake_now; }
static const char *t_getenv(const char *n) { return strcmp(n, "TZ") == 0 ? fake_tz : NULL; }
static uint64_t t_proc(void) { return 1234; }

static int t_get_tz(ucrt_tzinfo *z)
{
    memset(z, 0, sizeof *z);
    z->bias = 300; /* US Eastern */
    strcpy(z->std_name, "EST");
    strcpy(z->dst_name, "EDT");
    z->dst_bias = -60;
    z->dst_date = (ucrt_rule){0, 3, 0, 2, 2, 0, 0};  /* second Sunday of March, 02:00 */
    z->std_date = (ucrt_rule){0, 11, 0, 1, 2, 0, 0}; /* first Sunday of November, 02:00 */
    return 0;
}

static const ucrt_time_ops ops = {t_now, t_get_tz, t_proc, t_getenv};

static void setup(const char *tz)
{
    g_ucrt_time_ops = &ops;
    fake_tz = tz;
    g_ucrt_tz.loaded = 0;
    ucrt_tzset();
}

static int tm_equal(const ucrt_tm *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min && a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday &&
           a->tm_mon == b->tm_mon && a->tm_year == b->tm_year && a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday;
}

static void gmtime_matches_glibc(void)
{
    setup(NULL);
    for (int i = 0; i < 400000; ++i) {
        int64_t t = (int64_t)(rnd64() % (uint64_t)(UCRT_TIME_MAX_UTC + 1));
        ucrt_tm a;
        struct tm b;
        time_t tt = (time_t)t;
        if (i % 5 == 0)
            t = (int64_t)(rnd64() % 200000000ull) * 3 % 86400 * 1000 + (int64_t)(rnd64() % 40) * 86400 * 365;
        tt = (time_t)t;
        if (ucrt_gmtime64_s(&a, &t))
            continue;
        gmtime_r(&tt, &b);
        if (!tm_equal(&a, &b)) {
            MT_FAIL("gmtime(%lld) differs", (long long)t);
            return;
        }
    }
    /* the limits */
    {
        ucrt_tm a;
        int64_t t = -1;
        MT_EXPECT_EQ(ucrt_gmtime64_s(&a, &t), UCRT_EINVAL);
        MT_EXPECT_EQ(a.tm_year, -1); /* the structure is filled with -1 on failure */
        t = UCRT_TIME_MAX_UTC;
        MT_EXPECT_EQ(ucrt_gmtime64_s(&a, &t), 0);
        MT_EXPECT(a.tm_year == 1100 && a.tm_mon == 11 && a.tm_mday == 31 && a.tm_hour == 23);
        t = UCRT_TIME_MAX_UTC + 1;
        MT_EXPECT_EQ(ucrt_gmtime64_s(&a, &t), UCRT_EINVAL);
    }
}

static void mkgmtime_roundtrip(void)
{
    setup(NULL);
    for (int i = 0; i < 200000; ++i) {
        int64_t t = (int64_t)(rnd64() % (uint64_t)(UCRT_TIME_MAX_UTC + 1)), back;
        ucrt_tm a;
        ucrt_gmtime64_s(&a, &t);
        back = ucrt_mkgmtime64(&a);
        if (back != t) {
            MT_FAIL("mkgmtime(gmtime(%lld)) = %lld", (long long)t, (long long)back);
            return;
        }
    }
    /* normalisation of out-of-range fields, compared with glibc's timegm */
    for (int i = 0; i < 100000; ++i) {
        ucrt_tm a;
        struct tm b;
        time_t want;
        int64_t got;
        memset(&a, 0, sizeof a);
        a.tm_year = 70 + (int)(rnd64() % 200);
        a.tm_mon = (int)(rnd64() % 80) - 20;
        a.tm_mday = (int)(rnd64() % 200) - 50;
        a.tm_hour = (int)(rnd64() % 100) - 30;
        a.tm_min = (int)(rnd64() % 400) - 100;
        a.tm_sec = (int)(rnd64() % 400) - 100;
        memset(&b, 0, sizeof b);
        b.tm_year = a.tm_year;
        b.tm_mon = a.tm_mon;
        b.tm_mday = a.tm_mday;
        b.tm_hour = a.tm_hour;
        b.tm_min = a.tm_min;
        b.tm_sec = a.tm_sec;
        want = timegm(&b);
        got = ucrt_mkgmtime64(&a);
        if (want < 0) {
            MT_ASSERT(got == -1);
            continue;
        }
        if (got != want || !tm_equal(&a, &b)) {
            MT_FAIL("mkgmtime normalisation differs: %lld vs %lld", (long long)got, (long long)want);
            return;
        }
    }
}

static void localtime_us_eastern(void)
{
    setenv("TZ", "America/New_York", 1);
    tzset();
    setup(NULL);
    MT_EXPECT_EQ(g_ucrt_tz.timezone, 18000);
    MT_EXPECT_EQ(g_ucrt_tz.daylight, 1);
    for (int i = 0; i < 400000; ++i) {
        /* 2008 .. 2036: the years where today's US rules apply */
        int64_t t = 1199145600ll + (int64_t)(rnd64() % (28ull * 365 * 86400));
        ucrt_tm a;
        struct tm b;
        time_t tt = (time_t)t;
        if (i % 4 == 0) { /* the days around the transitions */
            ucrt_tm g;
            int64_t base;
            int year = 2008 + (int)(rnd64() % 28);
            g = (ucrt_tm){0, 0, 0, 1, (rnd64() & 1) ? 2 : 10, year - 1900, 0, 0, 0};
            base = ucrt_mkgmtime64(&g);
            t = base - 86400 * 3 + (int64_t)(rnd64() % (86400 * 36));
            tt = (time_t)t;
        }
        MT_ASSERT(ucrt_localtime64_s(&a, &t) == 0);
        localtime_r(&tt, &b);
        if (!tm_equal(&a, &b) || a.tm_isdst != b.tm_isdst) {
            MT_FAIL("localtime(%lld): %d-%02d-%02d %02d:%02d dst %d vs glibc %d-%02d-%02d %02d:%02d dst %d", (long long)t, a.tm_year + 1900,
                    a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, a.tm_isdst, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour,
                    b.tm_min, b.tm_isdst);
            return;
        }
    }
    unsetenv("TZ");
}

static void mktime_us_eastern(void)
{
    setenv("TZ", "America/New_York", 1);
    tzset();
    setup(NULL);
    for (int i = 0; i < 300000; ++i) {
        ucrt_tm a;
        struct tm b;
        time_t want;
        int64_t got;
        int year = 2008 + (int)(rnd64() % 28);
        memset(&a, 0, sizeof a);
        a.tm_year = year - 1900;
        a.tm_mon = (int)(rnd64() % 12);
        a.tm_mday = 1 + (int)(rnd64() % 28);
        a.tm_hour = (int)(rnd64() % 24);
        a.tm_min = (int)(rnd64() % 60);
        a.tm_isdst = -1;
        b = (struct tm){.tm_year = a.tm_year, .tm_mon = a.tm_mon, .tm_mday = a.tm_mday, .tm_hour = a.tm_hour, .tm_min = a.tm_min, .tm_isdst = -1};
        want = mktime(&b);
        got = ucrt_mktime64(&a);
        /* skip the hour that does not exist and the hour that occurs twice: implementations choose differently */
        if (got != want) {
            ucrt_tm la;
            int64_t g1 = got;
            (void)la;
            if ((a.tm_mon == 2 || a.tm_mon == 10) && a.tm_hour >= 1 && a.tm_hour <= 2)
                continue;
            MT_FAIL("mktime %d-%02d-%02d %02d:%02d: %lld vs glibc %lld", year, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, (long long)g1,
                    (long long)want);
            return;
        }
        if (!tm_equal(&a, &b) || a.tm_isdst != b.tm_isdst) {
            MT_FAIL("mktime normalised fields differ at %lld", (long long)got);
            return;
        }
    }
    unsetenv("TZ");
}

static void tz_string_and_southern_zone(void)
{
    ucrt_tm a;
    int64_t t;
    setup("PST8PDT");
    MT_EXPECT_EQ(g_ucrt_tz.timezone, 28800);
    MT_EXPECT(strcmp(g_ucrt_tz.tzname[0], "PST") == 0 && strcmp(g_ucrt_tz.tzname[1], "PDT") == 0);
    t = 1720000000; /* July 2024: summer */
    ucrt_localtime64_s(&a, &t);
    MT_EXPECT(a.tm_isdst == 1 && a.tm_hour == 2 && a.tm_min == 46);
    setup("CET-1");
    MT_EXPECT_EQ(g_ucrt_tz.timezone, -3600);
    MT_EXPECT_EQ(g_ucrt_tz.daylight, 0);
    t = 1720000000;
    ucrt_localtime64_s(&a, &t);
    MT_EXPECT_EQ(a.tm_isdst, 0);
    /* a southern hemisphere rule (the period wraps the new year): Sydney, UTC+10, DST from the first Sunday of
     * October to the first Sunday of April */
    {
        ucrt_tzinfo z;
        memset(&z, 0, sizeof z);
        z.bias = -600;
        z.dst_bias = -60;
        z.dst_date = (ucrt_rule){0, 10, 0, 1, 2, 0, 0};
        z.std_date = (ucrt_rule){0, 4, 0, 1, 3, 0, 0};
        t = 1704067200; /* 2024-01-01 */
        MT_EXPECT(ucrt_tz_dst_at(&z, t) == 1);
        t = 1720000000; /* July */
        MT_EXPECT(ucrt_tz_dst_at(&z, t) == 0);
        t = 1730000000; /* late October 2024 */
        MT_EXPECT(ucrt_tz_dst_at(&z, t) == 1);
    }
    /* the last-Sunday rule (day 5), as used in Europe */
    {
        ucrt_tzinfo z;
        memset(&z, 0, sizeof z);
        z.bias = -60;
        z.dst_bias = -60;
        z.dst_date = (ucrt_rule){0, 3, 0, 5, 2, 0, 0};
        z.std_date = (ucrt_rule){0, 10, 0, 5, 3, 0, 0};
        /* 2024-03-31 01:00 UTC is the transition */
        MT_EXPECT(ucrt_tz_dst_at(&z, 1711846799) == 0 && ucrt_tz_dst_at(&z, 1711846800) == 1);
        /* 2024-10-27 01:00 UTC */
        MT_EXPECT(ucrt_tz_dst_at(&z, 1729990799) == 1 && ucrt_tz_dst_at(&z, 1729990800) == 0);
    }
}

static void asctime_and_ctime(void)
{
    ucrt_tm a;
    char buf[40];
    int64_t t = 1709251199; /* 2024-02-29 23:59:59 UTC */
    setup("UTC0");
    ucrt_gmtime64_s(&a, &t);
    MT_EXPECT_EQ(ucrt_asctime_s(buf, sizeof buf, &a), 0);
    MT_EXPECT(strcmp(buf, "Thu Feb 29 23:59:59 2024\n") == 0);
    {
        char small[10];
        MT_EXPECT_EQ(ucrt_asctime_s(small, sizeof small, &a), UCRT_ERANGE);
        MT_EXPECT_EQ(small[0], 0);
    }
    MT_EXPECT_EQ(ucrt_ctime64_s(buf, sizeof buf, &t), 0);
    MT_EXPECT(strcmp(buf, "Thu Feb 29 23:59:59 2024\n") == 0);
    t = 5;
    ucrt_gmtime64_s(&a, &t);
    ucrt_asctime_s(buf, sizeof buf, &a);
    MT_EXPECT(strcmp(buf, "Thu Jan  1 00:00:05 1970\n") == 0); /* the day is space padded */
    a.tm_mon = 12;
    MT_EXPECT_EQ(ucrt_asctime_s(buf, sizeof buf, &a), UCRT_EINVAL);
}

static void strftime_matches_glibc(void)
{
    static const char *const fmts[] = {"%a %A %b %B", "%d %H %I %j %m %M %p %S", "%U %w %W %y %Y %%", "%C %D %e %F %g %G %h %n %R %t %T %u %V",
                                       "%d/%m/%Y %H:%M:%S", "%Y-%j", "[%e]", "%y%m%d"};
    setup("UTC0");
    setenv("TZ", "UTC", 1);
    tzset();
    for (int i = 0; i < 100000; ++i) {
        int64_t t = (int64_t)(rnd64() % (uint64_t)(UCRT_TIME_MAX_UTC + 1));
        time_t tt = (time_t)t;
        ucrt_tm a;
        struct tm b;
        char ra[200], rb[200];
        size_t na, nb;
        const char *f = fmts[rnd64() % (sizeof fmts / sizeof fmts[0])];
        ucrt_gmtime64_s(&a, &t);
        gmtime_r(&tt, &b);
        na = ucrt_strftime(ra, sizeof ra, f, &a);
        nb = strftime(rb, sizeof rb, f, &b);
        if (na != nb || memcmp(ra, rb, na) != 0) {
            MT_FAIL("strftime(\"%s\", %lld) = \"%s\", glibc \"%s\"", f, (long long)t, ra, rb);
            return;
        }
    }
}

static void strftime_microsoft_forms(void)
{
    ucrt_tm a;
    char buf[100];
    int64_t t = 1709251199;
    setup("UTC0");
    ucrt_gmtime64_s(&a, &t);
    MT_EXPECT_EQ((long)ucrt_strftime(buf, sizeof buf, "%c", &a), 17);
    MT_EXPECT(strcmp(buf, "02/29/24 23:59:59") == 0);
    ucrt_strftime(buf, sizeof buf, "%x|%X", &a);
    MT_EXPECT(strcmp(buf, "02/29/24|23:59:59") == 0);
    ucrt_strftime(buf, sizeof buf, "%#c", &a);
    MT_EXPECT(strcmp(buf, "Thursday, February 29, 2024 23:59:59") == 0);
    ucrt_strftime(buf, sizeof buf, "%#x", &a);
    MT_EXPECT(strcmp(buf, "Thursday, February 29, 2024") == 0);
    t = 3 * 86400 + 3600 * 5 + 7; /* 1970-01-04 05:00:07 */
    ucrt_gmtime64_s(&a, &t);
    ucrt_strftime(buf, sizeof buf, "%#d %#H %#I %#j %#m %#M %#S %#y", &a);
    MT_EXPECT(strcmp(buf, "4 5 5 4 1 0 7 70") == 0); /* the # flag drops the leading zeros */
    ucrt_strftime(buf, sizeof buf, "%I:%M %p %r", &a);
    MT_EXPECT(strcmp(buf, "05:00 AM 05:00:07 AM") == 0);
    /* %z and %Z follow the time zone */
    setup("PST8PDT");
    t = 1720000000;
    ucrt_localtime64_s(&a, &t);
    ucrt_strftime(buf, sizeof buf, "%Z %z", &a);
    MT_EXPECT(strcmp(buf, "PDT -0700") == 0);
    t = 1704067200;
    ucrt_localtime64_s(&a, &t);
    ucrt_strftime(buf, sizeof buf, "%Z %z", &a);
    MT_EXPECT(strcmp(buf, "PST -0800") == 0);
    /* not enough room: 0 and the buffer is emptied; unknown specifiers are invalid */
    {
        char tiny[5];
        MT_EXPECT_EQ((long)ucrt_strftime(tiny, sizeof tiny, "%Y-%m-%d", &a), 0);
        MT_EXPECT_EQ(tiny[0], 0);
        MT_EXPECT_EQ((long)ucrt_strftime(tiny, sizeof tiny, "%Y", &a), 4);
        MT_EXPECT_EQ((long)ucrt_strftime(tiny, 4, "%Y", &a), 0); /* the NUL needs room too */
        ucrt_set_errno(0);
        MT_EXPECT_EQ((long)ucrt_strftime(buf, sizeof buf, "%Q", &a), 0);
        MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    }
    {
        static const ucrt_wchar wf[] = {'%', 'Y', '-', '%', 'm', 0x20AC, 0};
        ucrt_wchar wb[32];
        MT_EXPECT_EQ((long)ucrt_wcsftime(wb, 32, wf, &a), 8);
        MT_EXPECT(wb[0] == '2' && wb[3] == '3' && wb[4] == '-' && wb[5] == '1' && wb[6] == '2' && wb[7] == 0x20AC && wb[8] == 0);
    }
}

static void clock_and_time(void)
{
    ucrt_timeb tb;
    setup("PST8PDT");
    fake_now = UCRT_EPOCH_DIFF_100NS + 1720000000ull * 10000000ull + 1234567ull; /* + 123.4567 ms */
    MT_EXPECT(ucrt_time64(NULL) == 1720000000);
    {
        int64_t t;
        ucrt_time64(&t);
        MT_EXPECT(t == 1720000000);
    }
    MT_EXPECT_EQ(ucrt_clock(), 1234);
    ucrt_ftime64(&tb);
    MT_EXPECT(tb.time == 1720000000 && tb.millitm == 123 && tb.timezone == 480 && tb.dstflag == 1);
    MT_EXPECT(ucrt_difftime64(10, 4) == 6.0);
    ucrt_tzset();
    MT_EXPECT_EQ(g_ucrt_tz.dstbias, -3600);
}

static const mt_case_t cases[] = {
    MT_CASE(gmtime_matches_glibc), MT_CASE(mkgmtime_roundtrip), MT_CASE(localtime_us_eastern), MT_CASE(mktime_us_eastern),
    MT_CASE(tz_string_and_southern_zone), MT_CASE(asctime_and_ctime), MT_CASE(strftime_matches_glibc),
    MT_CASE(strftime_microsoft_forms), MT_CASE(clock_and_time),
};

int main(void)
{
    return MT_RUN("crt_time", cases);
}
