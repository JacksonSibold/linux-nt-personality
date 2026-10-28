/*
 * test_k32_time_pure.c - host tests for src/ntdll/k32_time_pure.h: FILETIME and SYSTEMTIME
 * conversion (reference values computed with Python's datetime), range rules, time zone
 * bias and daylight rules (reference values from the IANA database for Los Angeles and
 * Sydney), and the date and time format patterns.
 */
#include "minitest.h"
#include "k32_time_pure.h"

#include <string.h>

static k32t_systemtime ST(int y, int mo, int dow, int d, int h, int mi, int s, int ms)
{
    k32t_systemtime t;
    t.year = (uint16_t)y;
    t.month = (uint16_t)mo;
    t.day_of_week = (uint16_t)dow;
    t.day = (uint16_t)d;
    t.hour = (uint16_t)h;
    t.minute = (uint16_t)mi;
    t.second = (uint16_t)s;
    t.ms = (uint16_t)ms;
    return t;
}

static void filetime_reference_values(void)
{
    k32t_systemtime st;
    uint64_t ft;
    /* 1601-01-01 is FILETIME 0, a Monday. */
    MT_EXPECT(k32t_filetime_to_systemtime(0, &st));
    MT_EXPECT(st.year == 1601 && st.month == 1 && st.day == 1 && st.day_of_week == 1);
    /* The Unix epoch. */
    MT_EXPECT(k32t_filetime_to_systemtime(0x19db1ded53e8000ull, &st));
    MT_EXPECT(st.year == 1970 && st.month == 1 && st.day == 1 && st.day_of_week == 4);
    /* A leap day with milliseconds. */
    MT_EXPECT(k32t_filetime_to_systemtime(0x1bf8311163618f0ull, &st));
    MT_EXPECT(st.year == 2000 && st.month == 2 && st.day == 29 && st.day_of_week == 2);
    MT_EXPECT(st.hour == 23 && st.minute == 59 && st.second == 59 && st.ms == 999);
    MT_EXPECT(k32t_filetime_to_systemtime(0x1db5b80664dfc50ull, &st));
    MT_EXPECT(st.year == 2024 && st.month == 12 && st.day == 31 && st.hour == 12 && st.minute == 34 &&
              st.second == 56 && st.ms == 789 && st.day_of_week == 2);
    MT_EXPECT(k32t_filetime_to_systemtime(0x22f9fc03dc34000ull, &st)); /* 2100 is not a leap year */
    MT_EXPECT(st.year == 2100 && st.month == 3 && st.day == 1 && st.day_of_week == 1);
    MT_EXPECT(k32t_filetime_to_systemtime(0x24c85a5ed127a980ull, &st));
    MT_EXPECT(st.year == 9999 && st.month == 12 && st.day == 31 && st.second == 59 && st.day_of_week == 5);
    /* And back. */
    st = ST(2024, 12, 0, 31, 12, 34, 56, 789);
    MT_EXPECT(k32t_systemtime_to_filetime(&st, &ft));
    MT_EXPECT_EQ(ft, 0x1db5b80664dfc50ull);
    st = ST(1970, 1, 99, 1, 0, 0, 0, 0); /* the day of week is ignored */
    MT_EXPECT(k32t_systemtime_to_filetime(&st, &ft));
    MT_EXPECT_EQ(ft, 0x19db1ded53e8000ull);
}

static void filetime_range(void)
{
    k32t_systemtime st;
    uint64_t ft;
    /* The sign bit makes a FILETIME invalid; 2^63 - 1 is the year 30828. */
    MT_EXPECT(!k32t_filetime_to_systemtime(0x8000000000000000ull, &st));
    MT_EXPECT(k32t_filetime_to_systemtime(0x7FFFFFFFFFFFFFFFull, &st));
    MT_EXPECT_EQ(st.year, 30828);
    MT_EXPECT_EQ(st.month, 9);
    MT_EXPECT_EQ(st.day, 14);
    MT_EXPECT_EQ(st.hour, 2);
    MT_EXPECT_EQ(st.minute, 48);
    MT_EXPECT_EQ(st.second, 5);
    MT_EXPECT_EQ(st.ms, 477);
    /* The year limits of SystemTimeToFileTime. */
    st = ST(1600, 12, 0, 31, 23, 59, 59, 999);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(1601, 1, 0, 1, 0, 0, 0, 0);
    MT_EXPECT(k32t_systemtime_to_filetime(&st, &ft));
    MT_EXPECT_EQ(ft, 0);
    st = ST(30827, 12, 0, 31, 23, 59, 59, 999);
    MT_EXPECT(k32t_systemtime_to_filetime(&st, &ft));
    st = ST(30828, 1, 0, 1, 0, 0, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
}

static void systemtime_field_validation(void)
{
    uint64_t ft;
    k32t_systemtime st = ST(2023, 2, 0, 29, 0, 0, 0, 0); /* not a leap year */
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 2, 0, 29, 0, 0, 0, 0);
    MT_EXPECT(k32t_systemtime_to_filetime(&st, &ft));
    st = ST(1900, 2, 0, 29, 0, 0, 0, 0); /* 1900 is not a leap year */
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2000, 2, 0, 29, 0, 0, 0, 0); /* 2000 is */
    MT_EXPECT(k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 13, 0, 1, 0, 0, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 0, 0, 1, 0, 0, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 4, 0, 31, 0, 0, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 4, 0, 0, 0, 0, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 4, 0, 30, 24, 0, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 4, 0, 30, 23, 60, 0, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 4, 0, 30, 23, 59, 60, 0);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
    st = ST(2024, 4, 0, 30, 23, 59, 59, 1000);
    MT_EXPECT(!k32t_systemtime_to_filetime(&st, &ft));
}

/* Every day from 1601 to 2400 round-trips, and the weekday advances by one each day. */
static void day_by_day_round_trip(void)
{
    int64_t day;
    int prev_wd = -1;
    for (day = 0; day < 800 * 366; ++day) {
        int y, m, d, wd;
        int64_t back;
        k32t_civil_from_days(day, &y, &m, &d);
        MT_ASSERT(m >= 1 && m <= 12 && d >= 1 && d <= k32t_days_in_month(y, m));
        back = k32t_days_from_civil(y, m, d);
        if (back != day) {
            MT_EXPECT_EQ(back, day);
            return;
        }
        wd = k32t_weekday(day);
        if (prev_wd >= 0 && wd != (prev_wd + 1) % 7) {
            MT_EXPECT_EQ(wd, (prev_wd + 1) % 7);
            return;
        }
        prev_wd = wd;
    }
}

static void compare(void)
{
    MT_EXPECT_EQ(k32t_compare(1, 2), -1);
    MT_EXPECT_EQ(k32t_compare(2, 2), 0);
    MT_EXPECT_EQ(k32t_compare(0x8000000000000000ull, 5), 1); /* unsigned */
}

static k32t_zone los_angeles(void)
{
    k32t_zone z;
    memset(&z, 0, sizeof(z));
    z.bias = 480;
    z.standard_bias = 0;
    z.daylight_bias = -60;
    z.daylight_date = ST(0, 3, 0, 2, 2, 0, 0, 0);  /* second Sunday of March, 02:00 */
    z.standard_date = ST(0, 11, 0, 1, 2, 0, 0, 0); /* first Sunday of November, 02:00 */
    return z;
}

static k32t_zone sydney(void)
{
    k32t_zone z;
    memset(&z, 0, sizeof(z));
    z.bias = -600;
    z.standard_bias = 0;
    z.daylight_bias = -60;
    z.daylight_date = ST(0, 10, 0, 1, 2, 0, 0, 0); /* first Sunday of October, 02:00 */
    z.standard_date = ST(0, 4, 0, 1, 3, 0, 0, 0);  /* first Sunday of April, 03:00 */
    return z;
}

static void expect_local(const k32t_zone *z, uint64_t utc, int y, int mo, int d, int h, int mi)
{
    uint64_t local;
    k32t_systemtime st;
    MT_EXPECT(k32t_utc_to_local(z, utc, &local));
    MT_EXPECT(k32t_filetime_to_systemtime(local, &st));
    MT_EXPECT_EQ(st.year, y);
    MT_EXPECT_EQ(st.month, mo);
    MT_EXPECT_EQ(st.day, d);
    MT_EXPECT_EQ(st.hour, h);
    MT_EXPECT_EQ(st.minute, mi);
}

static void zone_northern_hemisphere(void)
{
    k32t_zone z = los_angeles();
    expect_local(&z, 0x1da72c954f22800ull, 2024, 3, 10, 1, 0);   /* before the change: PST */
    expect_local(&z, 0x1da72d1b6b69000ull, 2024, 3, 10, 3, 0);   /* the hour after: PDT, 02:00 skipped */
    expect_local(&z, 0x1db2dc6617e4000ull, 2024, 11, 3, 1, 0);   /* PDT, the first 01:00 */
    expect_local(&z, 0x1db2dcec342a800ull, 2024, 11, 3, 1, 0);   /* PST, the second 01:00 */
    expect_local(&z, 0x1dacbae32ed2000ull, 2024, 7, 1, 5, 0);    /* summer */
    expect_local(&z, 0x1da4745c8524000ull, 2024, 1, 14, 16, 0);  /* winter, previous day */
    MT_EXPECT_EQ(k32t_bias_at_utc(&z, 0x1dacbae32ed2000ull), 420);
    MT_EXPECT_EQ(k32t_bias_at_utc(&z, 0x1da4745c8524000ull), 480);
}

static void zone_southern_hemisphere(void)
{
    k32t_zone z = sydney();
    expect_local(&z, 0x1da883318b3d800ull, 2024, 4, 7, 2, 0);   /* daylight (+11) until 03:00 local */
    expect_local(&z, 0x1da883b7a784000ull, 2024, 4, 7, 2, 0);   /* standard (+10) */
    expect_local(&z, 0x1db17373fe25800ull, 2024, 10, 6, 1, 0);  /* standard */
    expect_local(&z, 0x1db173fa1a6c000ull, 2024, 10, 6, 3, 0);  /* daylight, 02:00 skipped */
    expect_local(&z, 0x1da3c457689c000ull, 2024, 1, 1, 11, 0);  /* new year inside daylight time */
    MT_EXPECT_EQ(k32t_bias_at_utc(&z, 0x1da3c457689c000ull), -660);
}

static void zone_without_rules_and_utc(void)
{
    k32t_zone z;
    uint64_t local, utc;
    memset(&z, 0, sizeof(z));
    MT_EXPECT(k32t_utc_to_local(&z, 12345678, &local));
    MT_EXPECT_EQ(local, 12345678);
    z.bias = -60; /* a fixed zone east of UTC */
    MT_EXPECT(k32t_utc_to_local(&z, K32T_TICKS_PER_DAY, &local));
    MT_EXPECT_EQ(local, K32T_TICKS_PER_DAY + 3600ull * K32T_TICKS_PER_SEC);
    MT_EXPECT(k32t_local_to_utc(&z, local, &utc));
    MT_EXPECT_EQ(utc, K32T_TICKS_PER_DAY);
    /* Results outside the FILETIME range fail instead of wrapping. */
    z.bias = 60;
    MT_EXPECT(!k32t_utc_to_local(&z, 0, &local));
    MT_EXPECT(!k32t_local_to_utc(&z, 0x8000000000000001ull, &utc));
}

static void local_to_utc_inverse(void)
{
    k32t_zone z = los_angeles();
    uint64_t utc, local, back;
    /* A summer instant converts to local and back. */
    utc = 0x1dacbae32ed2000ull;
    MT_EXPECT(k32t_utc_to_local(&z, utc, &local));
    MT_EXPECT(k32t_local_to_utc(&z, local, &back));
    MT_EXPECT_EQ(back, utc);
    utc = 0x1da4745c8524000ull;
    MT_EXPECT(k32t_utc_to_local(&z, utc, &local));
    MT_EXPECT(k32t_local_to_utc(&z, local, &back));
    MT_EXPECT_EQ(back, utc);
}

static int fmt_is(const char *pattern, const k32t_systemtime *st, const char *want)
{
    uint16_t buf[128];
    int fits = 0;
    size_t n = k32t_format(buf, 128, pattern, st, &fits);
    size_t i;
    if (!fits || n != strlen(want) + 1)
        return 0;
    for (i = 0; want[i]; ++i)
        if (buf[i] != (uint16_t)(unsigned char)want[i])
            return 0;
    return buf[i] == 0;
}

static void format_patterns(void)
{
    k32t_systemtime t = ST(2024, 12, 2, 31, 12, 34, 56, 789);
    k32t_systemtime am = ST(2024, 3, 5, 7, 0, 5, 9, 0);
    MT_EXPECT(fmt_is("dddd, MMMM dd, yyyy", &t, "Tuesday, December 31, 2024"));
    MT_EXPECT(fmt_is("ddd MMM d yy", &t, "Tue Dec 31 24"));
    MT_EXPECT(fmt_is("M/d/yyyy", &t, "12/31/2024"));
    MT_EXPECT(fmt_is("M/d/yyyy", &am, "3/7/2024"));
    MT_EXPECT(fmt_is("MM/dd/yy", &am, "03/07/24"));
    MT_EXPECT(fmt_is("h:mm:ss tt", &t, "12:34:56 PM"));
    MT_EXPECT(fmt_is("hh:mm t", &am, "12:05 A"));
    MT_EXPECT(fmt_is("H:m:s", &am, "0:5:9"));
    MT_EXPECT(fmt_is("HH:mm:ss", &am, "00:05:09"));
    MT_EXPECT(fmt_is("'Today is' d", &t, "Today is 31"));
    MT_EXPECT(fmt_is("'it''s' yyyy", &t, "it's 2024"));
    MT_EXPECT(fmt_is("", &t, ""));
    MT_EXPECT(fmt_is("d.M", &t, "31.12"));
}

static void format_sizing(void)
{
    k32t_systemtime t = ST(2024, 12, 2, 31, 12, 34, 56, 789);
    uint16_t buf[8];
    int fits = 0;
    /* A NULL buffer reports the size including the NUL. */
    MT_EXPECT_EQ(k32t_format(NULL, 0, "M/d/yyyy", &t, &fits), 11);
    MT_EXPECT(fits);
    /* A buffer that is too small writes nothing the caller may rely on and reports no fit. */
    MT_EXPECT_EQ(k32t_format(buf, 8, "M/d/yyyy", &t, &fits), 0);
    MT_EXPECT(!fits);
    MT_EXPECT_EQ(k32t_format(buf, 8, "d/M", &t, &fits), 6);
    MT_EXPECT(fits);
    MT_EXPECT(buf[0] == '3' && buf[4] == '2' && buf[5] == 0);
    /* Exactly full: "31/12/24" is 8 characters and needs 9 units. */
    MT_EXPECT_EQ(k32t_format(buf, 8, "d/M/yy", &t, &fits), 0);
    MT_EXPECT(!fits);
}

static void default_patterns(void)
{
    MT_EXPECT(strcmp(k32t_default_date_pattern(0), "M/d/yyyy") == 0);
    MT_EXPECT(strcmp(k32t_default_date_pattern(1), "M/d/yyyy") == 0);
    MT_EXPECT(strcmp(k32t_default_date_pattern(2), "dddd, MMMM d, yyyy") == 0);
    MT_EXPECT(strcmp(k32t_default_date_pattern(8), "MMMM yyyy") == 0);
    MT_EXPECT(strcmp(k32t_default_date_pattern(0x80), "MMMM d") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(0), "h:mm:ss tt") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(2), "h:mm tt") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(4), "h:mm:ss") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(8), "HH:mm:ss") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(8 | 2), "HH:mm") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(1), "h tt") == 0);
    MT_EXPECT(strcmp(k32t_default_time_pattern(1 | 8), "HH") == 0);
    {
        k32t_systemtime t = ST(2024, 12, 2, 31, 15, 4, 9, 0);
        uint16_t buf[32];
        int fits;
        MT_EXPECT(k32t_format(buf, 32, k32t_default_time_pattern(0), &t, &fits) == 10 && fits); /* "3:04:09 PM" */
        MT_EXPECT(buf[0] == '3' && buf[8] == 'P' && buf[9] == 'M');
    }
}

static const mt_case_t cases[] = {
    MT_CASE(filetime_reference_values),
    MT_CASE(filetime_range),
    MT_CASE(systemtime_field_validation),
    MT_CASE(day_by_day_round_trip),
    MT_CASE(compare),
    MT_CASE(zone_northern_hemisphere),
    MT_CASE(zone_southern_hemisphere),
    MT_CASE(zone_without_rules_and_utc),
    MT_CASE(local_to_utc_inverse),
    MT_CASE(format_patterns),
    MT_CASE(format_sizing),
    MT_CASE(default_patterns),
};

int main(void)
{
    return MT_RUN("k32_time_pure", cases);
}
