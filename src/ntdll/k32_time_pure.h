/*
 * k32_time_pure.h - pure rules for the kernel32 time shims (kernel32_time.c):
 * FILETIME and SYSTEMTIME conversion, time zone bias and daylight rules,
 * CompareFileTime, and the date and time format patterns of GetDateFormatW
 * and GetTimeFormatW.
 *
 * FILETIME counts 100 ns ticks since 1601-01-01 00:00:00 UTC. SYSTEMTIME is
 * valid for the years 1601 to 30827, as on Windows. The personality has no
 * time zone database: the system time zone is UTC (see kernel32_time.c).
 */
#ifndef NTDLL_K32_TIME_PURE_H
#define NTDLL_K32_TIME_PURE_H

#include <stddef.h>
#include <stdint.h>

#define K32T_TICKS_PER_MS 10000ull
#define K32T_TICKS_PER_SEC 10000000ull
#define K32T_TICKS_PER_DAY 864000000000ull
#define K32T_MIN_YEAR 1601
#define K32T_MAX_YEAR 30827
/* FileTimeToSystemTime accepts every FILETIME up to 2^63 - 1 (the year 30828). Values
 * with the sign bit set are out of range. SystemTimeToFileTime stops at the year 30827. */
#define K32T_FILETIME_MAX 0x7FFFFFFFFFFFFFFFull

/* SYSTEMTIME as eight 16-bit fields. */
typedef struct k32t_systemtime {
    uint16_t year, month, day_of_week, day, hour, minute, second, ms;
} k32t_systemtime;

static inline int k32t_is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static inline int k32t_days_in_month(int y, int m)
{
    static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12)
        return 0;
    return m == 2 && k32t_is_leap(y) ? 29 : d[m - 1];
}

/* Days from 1601-01-01 to y-m-d (proleptic Gregorian), for y >= 1601. */
static inline int64_t k32t_days_from_civil(int y, int m, int d)
{
    /* Howard Hinnant's algorithm, shifted so day 0 is 1601-01-01. */
    int64_t yy = (int64_t)y - (m <= 2 ? 1 : 0);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days_from_0000_03_01 = era * 146097 + doe;
    /* 1601-01-01 is 584694 days after 0000-03-01 in this count (day 0 = 0000-03-01). */
    return days_from_0000_03_01 - 584694;
}

/* The inverse: civil date from days since 1601-01-01 (days >= 0). */
static inline void k32t_civil_from_days(int64_t days, int *y, int *m, int *d)
{
    int64_t z = days + 584694;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2 ? 1 : 0));
}

/* 1601-01-01 was a Monday; 0 = Sunday. */
static inline int k32t_weekday(int64_t days)
{
    return (int)((days + 1) % 7);
}

/* FileTimeToSystemTime. Returns 0 when ft is above K32T_FILETIME_MAX (this includes every value
 * with the sign bit set). */
static inline int k32t_filetime_to_systemtime(uint64_t ft, k32t_systemtime *st)
{
    uint64_t days, rem;
    int y, m, d;
    if (ft > K32T_FILETIME_MAX)
        return 0;
    days = ft / K32T_TICKS_PER_DAY;
    rem = ft % K32T_TICKS_PER_DAY;
    k32t_civil_from_days((int64_t)days, &y, &m, &d);
    st->year = (uint16_t)y;
    st->month = (uint16_t)m;
    st->day = (uint16_t)d;
    st->day_of_week = (uint16_t)k32t_weekday((int64_t)days);
    st->hour = (uint16_t)(rem / (3600ull * K32T_TICKS_PER_SEC));
    rem %= 3600ull * K32T_TICKS_PER_SEC;
    st->minute = (uint16_t)(rem / (60ull * K32T_TICKS_PER_SEC));
    rem %= 60ull * K32T_TICKS_PER_SEC;
    st->second = (uint16_t)(rem / K32T_TICKS_PER_SEC);
    rem %= K32T_TICKS_PER_SEC;
    st->ms = (uint16_t)(rem / K32T_TICKS_PER_MS);
    return 1;
}

/* 1 when every field is in range; the day of week is ignored, as on Windows. */
static inline int k32t_systemtime_valid(const k32t_systemtime *st)
{
    if (st->year < K32T_MIN_YEAR || st->year > K32T_MAX_YEAR)
        return 0;
    if (st->month < 1 || st->month > 12)
        return 0;
    if (st->day < 1 || st->day > k32t_days_in_month(st->year, st->month))
        return 0;
    return st->hour < 24 && st->minute < 60 && st->second < 60 && st->ms < 1000;
}

/* SystemTimeToFileTime. Returns 0 for an invalid SYSTEMTIME. */
static inline int k32t_systemtime_to_filetime(const k32t_systemtime *st, uint64_t *ft)
{
    int64_t days;
    if (!k32t_systemtime_valid(st))
        return 0;
    days = k32t_days_from_civil(st->year, st->month, st->day);
    *ft = (uint64_t)days * K32T_TICKS_PER_DAY +
          ((uint64_t)st->hour * 3600ull + (uint64_t)st->minute * 60ull + st->second) * K32T_TICKS_PER_SEC +
          (uint64_t)st->ms * K32T_TICKS_PER_MS;
    return 1;
}

/* CompareFileTime: -1, 0 or 1. FILETIMEs compare as unsigned. */
static inline int k32t_compare(uint64_t a, uint64_t b)
{
    return a < b ? -1 : a > b ? 1 : 0;
}

/* ---- Time zones ------------------------------------------------------------------ */

/* The part of TIME_ZONE_INFORMATION that decides the offset. Dates are SYSTEMTIME: with a
 * zero month the zone has no transition; otherwise day is the week of the month (1 to 5,
 * 5 = the last) and day_of_week the weekday, as in the Windows structure. Biases are in
 * minutes: UTC = local + bias. */
typedef struct k32t_zone {
    int32_t bias;
    int32_t standard_bias;
    int32_t daylight_bias;
    k32t_systemtime standard_date;  /* when daylight time ends */
    k32t_systemtime daylight_date;  /* when daylight time begins */
} k32t_zone;

/* The Nth (week 1..4) or last (week 5) weekday of a month, as days since 1601-01-01. */
static inline int64_t k32t_nth_weekday(int year, int month, int week, int weekday)
{
    int64_t first = k32t_days_from_civil(year, month, 1);
    int first_wd = k32t_weekday(first);
    int64_t day = first + ((weekday - first_wd + 7) % 7) + (int64_t)(week - 1) * 7;
    int dim = k32t_days_in_month(year, month);
    while (day - first >= dim)
        day -= 7; /* week 5 means the last such weekday */
    return day;
}

/* FILETIME ticks, in local wall time, of a transition rule in the given year. */
static inline uint64_t k32t_transition_local(int year, const k32t_systemtime *rule)
{
    int64_t day;
    if (rule->year != 0) { /* an absolute date */
        day = k32t_days_from_civil(rule->year, rule->month, rule->day);
    } else {
        day = k32t_nth_weekday(year, rule->month, rule->day, rule->day_of_week);
    }
    return (uint64_t)day * K32T_TICKS_PER_DAY +
           ((uint64_t)rule->hour * 3600ull + (uint64_t)rule->minute * 60ull + rule->second) * K32T_TICKS_PER_SEC +
           (uint64_t)rule->ms * K32T_TICKS_PER_MS;
}

/* 1 when daylight time is in effect at the local standard time `local` (FILETIME ticks).
 * Handles both hemispheres: when daylight begins after it ends in the calendar year,
 * daylight time spans the new year. */
static inline int k32t_is_daylight(const k32t_zone *z, uint64_t local_standard, int year)
{
    uint64_t start, end;
    if (z->standard_date.month == 0 || z->daylight_date.month == 0)
        return 0;
    start = k32t_transition_local(year, &z->daylight_date);
    /* The start rule is written in standard time. The end rule is written in daylight time, so
     * its standard-time equivalent is earlier by the difference of the two biases. */
    end = k32t_transition_local(year, &z->standard_date) +
          (uint64_t)(int64_t)(z->daylight_bias - z->standard_bias) * 60ull * K32T_TICKS_PER_SEC;
    if (start < end)
        return local_standard >= start && local_standard < end;
    return local_standard >= start || local_standard < end;
}

/* The bias in minutes for a UTC FILETIME: bias + standard or daylight bias. */
static inline int32_t k32t_bias_at_utc(const k32t_zone *z, uint64_t utc)
{
    uint64_t local_std = utc - (uint64_t)(int64_t)z->bias * 60 * K32T_TICKS_PER_SEC;
    k32t_systemtime st;
    if (!k32t_filetime_to_systemtime(local_std, &st))
        return z->bias;
    return z->bias + (k32t_is_daylight(z, local_std, st.year) ? z->daylight_bias : z->standard_bias);
}

/* SystemTimeToTzSpecificLocalTime on FILETIME ticks: 0 when the result is out of range. */
static inline int k32t_utc_to_local(const k32t_zone *z, uint64_t utc, uint64_t *local)
{
    int64_t bias = k32t_bias_at_utc(z, utc);
    int64_t r = (int64_t)utc - bias * 60 * (int64_t)K32T_TICKS_PER_SEC;
    if (r < 0 || (uint64_t)r > K32T_FILETIME_MAX)
        return 0;
    *local = (uint64_t)r;
    return 1;
}

/* TzSpecificLocalTimeToSystemTime: the local time is taken as standard time unless the
 * daylight rule applies to it. An hour that happens twice or not at all resolves as standard time. */
static inline int k32t_local_to_utc(const k32t_zone *z, uint64_t local, uint64_t *utc)
{
    k32t_systemtime st;
    int64_t bias;
    int64_t r;
    if (!k32t_filetime_to_systemtime(local, &st))
        return 0;
    bias = z->bias + (k32t_is_daylight(z, local, st.year) ? z->daylight_bias : z->standard_bias);
    r = (int64_t)local + bias * 60 * (int64_t)K32T_TICKS_PER_SEC;
    if (r < 0 || (uint64_t)r > K32T_FILETIME_MAX)
        return 0;
    *utc = (uint64_t)r;
    return 1;
}

/* ---- GetDateFormatW and GetTimeFormatW -------------------------------------------- */

static const char *const k32t_day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const k32t_month_names[12] = {"January", "February", "March", "April", "May", "June",
                                                 "July", "August", "September", "October", "November", "December"};

/* Appends s (ASCII) to out at *n, never writing at or past cap. *total counts every character
 * that would be written, so the caller can report the size needed. */
static inline void k32t_put(uint16_t *out, size_t cap, size_t *n, size_t *total, const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (out && *n < cap)
            out[*n] = (uint16_t)(unsigned char)s[i];
        ++*n;
        ++*total;
    }
}

static inline size_t k32t_cstr_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static inline void k32t_put_num(uint16_t *out, size_t cap, size_t *n, size_t *total, unsigned v, int min_digits)
{
    char buf[12];
    int len = 0;
    do {
        buf[len++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (len < min_digits)
        buf[len++] = '0';
    for (int i = len - 1; i >= 0; --i)
        k32t_put(out, cap, n, total, &buf[i], 1);
}

/* Formats a date or time with a pattern of the picture characters Windows defines: d dd ddd dddd
 * M MM MMM MMMM y yy yyyy for dates and h hh H HH m mm s ss t tt for times. A single-quoted
 * run is copied literally and '' is one quote. Returns the number of characters the result
 * needs including the NUL, and writes at most cap of them (when out is not NULL). Returns 0
 * when cap is nonzero, out is not NULL and the result does not fit; sets *fits to match. */
static inline size_t k32t_format(uint16_t *out, size_t cap, const char *pattern, const k32t_systemtime *st,
                                 int *fits)
{
    size_t n = 0, total = 0;
    size_t i = 0;
    while (pattern[i]) {
        char c = pattern[i];
        size_t run = 1;
        if (c == '\'') {
            ++i;
            while (pattern[i]) {
                if (pattern[i] == '\'') {
                    if (pattern[i + 1] == '\'') { /* '' inside quotes is one quote */
                        k32t_put(out, cap, &n, &total, "'", 1);
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                k32t_put(out, cap, &n, &total, &pattern[i], 1);
                ++i;
            }
            continue;
        }
        while (pattern[i + run] == c)
            ++run;
        switch (c) {
        case 'd':
            if (run == 1)
                k32t_put_num(out, cap, &n, &total, st->day, 1);
            else if (run == 2)
                k32t_put_num(out, cap, &n, &total, st->day, 2);
            else if (run == 3)
                k32t_put(out, cap, &n, &total, k32t_day_names[st->day_of_week % 7], 3);
            else
                k32t_put(out, cap, &n, &total, k32t_day_names[st->day_of_week % 7],
                         k32t_cstr_len(k32t_day_names[st->day_of_week % 7]));
            break;
        case 'M':
            if (run <= 2)
                k32t_put_num(out, cap, &n, &total, st->month, (int)run);
            else if (run == 3)
                k32t_put(out, cap, &n, &total, k32t_month_names[(st->month + 11) % 12], 3);
            else
                k32t_put(out, cap, &n, &total, k32t_month_names[(st->month + 11) % 12],
                         k32t_cstr_len(k32t_month_names[(st->month + 11) % 12]));
            break;
        case 'y':
            if (run <= 2)
                k32t_put_num(out, cap, &n, &total, st->year % 100, (int)run);
            else
                k32t_put_num(out, cap, &n, &total, st->year, 4);
            break;
        case 'h': {
            unsigned h12 = st->hour % 12 == 0 ? 12 : st->hour % 12;
            k32t_put_num(out, cap, &n, &total, h12, (int)(run > 1 ? 2 : 1));
            break;
        }
        case 'H':
            k32t_put_num(out, cap, &n, &total, st->hour, (int)(run > 1 ? 2 : 1));
            break;
        case 'm':
            k32t_put_num(out, cap, &n, &total, st->minute, (int)(run > 1 ? 2 : 1));
            break;
        case 's':
            k32t_put_num(out, cap, &n, &total, st->second, (int)(run > 1 ? 2 : 1));
            break;
        case 't': {
            const char *ap = st->hour < 12 ? "AM" : "PM";
            k32t_put(out, cap, &n, &total, ap, run == 1 ? 1 : 2);
            break;
        }
        default:
            for (size_t k = 0; k < run; ++k)
                k32t_put(out, cap, &n, &total, &c, 1);
            break;
        }
        i += run;
    }
    ++total; /* the NUL */
    if (out && cap) {
        if (total > cap) {
            if (fits)
                *fits = 0;
            return 0;
        }
        out[n] = 0;
    }
    if (fits)
        *fits = 1;
    return total;
}

/* The pattern GetDateFormat uses when the caller passes none: DATE_SHORTDATE (1) or no flag give
 * the en-US short form, DATE_LONGDATE (2) the long form, DATE_YEARMONTH (8) and DATE_MONTHDAY (0x80)
 * the partial forms. */
static inline const char *k32t_default_date_pattern(uint32_t flags)
{
    if (flags & 0x2u)
        return "dddd, MMMM d, yyyy";
    if (flags & 0x8u)
        return "MMMM yyyy";
    if (flags & 0x80u)
        return "MMMM d";
    return "M/d/yyyy";
}

/* The pattern GetTimeFormat uses when the caller passes none, shaped by TIME_NOMINUTESORSECONDS (1),
 * TIME_NOSECONDS (2), TIME_NOTIMEMARKER (4) and TIME_FORCE24HOURFORMAT (8). */
static inline const char *k32t_default_time_pattern(uint32_t flags)
{
    int h24 = (flags & 0x8u) != 0, marker = !h24 && !(flags & 0x4u);
    if (flags & 0x1u)
        return h24 ? "HH" : (marker ? "h tt" : "h");
    if (flags & 0x2u)
        return h24 ? "HH:mm" : (marker ? "h:mm tt" : "h:mm");
    return h24 ? "HH:mm:ss" : (marker ? "h:mm:ss tt" : "h:mm:ss");
}

#endif /* NTDLL_K32_TIME_PURE_H */
