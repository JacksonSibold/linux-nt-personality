/*
 * ucrt_time.h - the C runtime's time functions: time, gmtime, localtime and mktime with the Windows time zone
 * model (a bias and two SYSTEMTIME-style transition rules, or a POSIX-style TZ string), asctime, ctime, strftime
 * and wcsftime in the "C" locale, clock and the _timeb functions. The operating system supplies the clock and the
 * time zone through ucrt_time_ops. Host tests: tests/crt-host/test_time.c against glibc.
 */
#ifndef UCRT_TIME_H
#define UCRT_TIME_H

#include "ucrt_conv.h"
#include "ucrt_state.h"
#include "ucrt_string.h"

typedef struct ucrt_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
} ucrt_tm;

/* A transition rule, as in SYSTEMTIME: when year is 0 the rule is relative (month, the weekday dow, the nth such weekday
 * with day 1..4, or day 5 for the last), otherwise it is that absolute date. A month of 0 means no transition. */
typedef struct ucrt_rule {
    uint16_t year, month, dow, day, hour, minute, second;
} ucrt_rule;

typedef struct ucrt_tzinfo {
    int32_t bias;     /* minutes: UTC = local + bias */
    int32_t std_bias; /* added to bias outside daylight saving time (normally 0) */
    int32_t dst_bias; /* added during daylight saving time (normally -60) */
    char std_name[64], dst_name[64];
    ucrt_rule std_date; /* when daylight saving ends, in local daylight time */
    ucrt_rule dst_date; /* when daylight saving starts, in local standard time */
} ucrt_tzinfo;

typedef struct ucrt_time_ops {
    uint64_t (*now)(void);                      /* 100 ns ticks since 1601-01-01 UTC */
    int (*get_tz)(ucrt_tzinfo *out);            /* the system time zone; 0 on success */
    uint64_t (*process_ms)(void);               /* milliseconds since the process started (for clock) */
    const char *(*getenv)(const char *name);    /* for TZ; may be NULL */
} ucrt_time_ops;

static const ucrt_time_ops *g_ucrt_time_ops;

typedef struct ucrt_tz_state {
    ucrt_tzinfo tz;
    int loaded;
    int32_t timezone; /* seconds west of UTC (bias * 60) */
    int daylight;     /* 1 when the zone has daylight saving time */
    int32_t dstbias;  /* seconds added during daylight saving time */
    char tzname[2][64];
} ucrt_tz_state;

static ucrt_tz_state g_ucrt_tz;

#define UCRT_SECS_PER_DAY 86400
#define UCRT_TIME_MAX_UTC 32535215999ll   /* 3000-12-31 23:59:59 */
#define UCRT_TIME_MAX_LOCAL 32535244799ll /* the same instant in UTC+... allowed for localtime */

/* ---- The calendar ----------------------------------------------------------------------------------- */

/* Days from 1970-01-01 to the civil date y-m-d (m 1..12), for any year. */
static inline int64_t ucrt_days_from_civil(int64_t y, int m, int d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline void ucrt_civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y += *m <= 2;
}

static inline int ucrt_is_leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static inline int ucrt_days_in_month(int64_t y, int m)
{
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ucrt_is_leap(y) ? 29 : dm[m - 1];
}

/* Breaks seconds since the epoch (as if UTC) into a tm; no range check. */
static inline void ucrt_break_down(int64_t t, ucrt_tm *tm)
{
    int64_t days = t >= 0 ? t / UCRT_SECS_PER_DAY : -((-t + UCRT_SECS_PER_DAY - 1) / UCRT_SECS_PER_DAY), y;
    int64_t rem = t - days * UCRT_SECS_PER_DAY;
    int m, d;
    ucrt_civil_from_days(days, &y, &m, &d);
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday */
    tm->tm_yday = (int)(days - ucrt_days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
}

/* ---- Time zone ---------------------------------------------------------------------------------------- */

/* The instant of a rule in the year, as seconds since the epoch in the rule's own (local) time. */
static inline int64_t ucrt_rule_local_epoch(int64_t year, const ucrt_rule *r)
{
    int64_t days;
    if (r->year) {
        days = ucrt_days_from_civil(r->year, r->month, r->day);
    } else {
        int64_t first = ucrt_days_from_civil(year, r->month, 1);
        int wd1 = (int)(((first % 7) + 11) % 7);
        int day = 1 + ((int)r->dow - wd1 + 7) % 7 + ((int)r->day - 1) * 7;
        if (day > ucrt_days_in_month(year, r->month))
            day -= 7;
        days = first + day - 1;
    }
    return days * UCRT_SECS_PER_DAY + r->hour * 3600 + r->minute * 60 + r->second;
}

static inline int ucrt_tz_has_dst(const ucrt_tzinfo *z)
{
    return z->dst_date.month != 0 && z->std_date.month != 0;
}

/* Is daylight saving time in effect at the UTC instant t? */
static inline int ucrt_tz_dst_at(const ucrt_tzinfo *z, int64_t t)
{
    int64_t local_std, year, start, end;
    int m, d;
    if (!ucrt_tz_has_dst(z))
        return 0;
    local_std = t - (int64_t)(z->bias + z->std_bias) * 60;
    ucrt_civil_from_days(local_std >= 0 ? local_std / UCRT_SECS_PER_DAY : -((-local_std + UCRT_SECS_PER_DAY - 1) / UCRT_SECS_PER_DAY), &year, &m, &d);
    start = ucrt_rule_local_epoch(year, &z->dst_date) + (int64_t)(z->bias + z->std_bias) * 60;
    end = ucrt_rule_local_epoch(year, &z->std_date) + (int64_t)(z->bias + z->dst_bias) * 60;
    if (start < end)
        return t >= start && t < end;
    return t >= start || t < end; /* the southern hemisphere: the period wraps over new year */
}

/* Seconds to add to UTC to get local time at instant t. */
static inline int64_t ucrt_tz_offset_at(const ucrt_tzinfo *z, int64_t t, int *isdst)
{
    int dst = ucrt_tz_dst_at(z, t);
    if (isdst)
        *isdst = dst;
    return -(int64_t)(z->bias + (dst ? z->dst_bias : z->std_bias)) * 60;
}

static inline void ucrt_tz_fill_state(void)
{
    ucrt_tz_state *s = &g_ucrt_tz;
    s->timezone = s->tz.bias * 60 + s->tz.std_bias * 60;
    s->daylight = ucrt_tz_has_dst(&s->tz);
    s->dstbias = (s->tz.dst_bias - s->tz.std_bias) * 60;
    ucrt_strncpy(s->tzname[0], s->tz.std_name, 63);
    s->tzname[0][63] = 0;
    ucrt_strncpy(s->tzname[1], s->tz.dst_name, 63);
    s->tzname[1][63] = 0;
}

/* Parses a TZ string of the form "PST8PDT", "EST5", "CET-1CEST": a name of three or more letters, an offset in
 * hours (west of UTC positive), optionally minutes and seconds, and an optional daylight saving name. Daylight saving
 * time follows the current US rules (second Sunday of March to first Sunday of November, 2:00). */
static inline int ucrt_parse_tz(const char *tz, ucrt_tzinfo *z)
{
    int n = 0, sign = 1, hours = 0, mins = 0, secs = 0;
    __builtin_memset(z, 0, sizeof(*z));
    while ((tz[n] >= 'A' && tz[n] <= 'Z') || (tz[n] >= 'a' && tz[n] <= 'z')) {
        if (n < 63)
            z->std_name[n] = tz[n];
        ++n;
    }
    if (n < 3)
        return -1;
    tz += n;
    if (*tz == '-' || *tz == '+') {
        sign = *tz == '-' ? -1 : 1;
        ++tz;
    }
    if (!(*tz >= '0' && *tz <= '9'))
        return -1;
    while (*tz >= '0' && *tz <= '9')
        hours = hours * 10 + (*tz++ - '0');
    if (*tz == ':') {
        ++tz;
        while (*tz >= '0' && *tz <= '9')
            mins = mins * 10 + (*tz++ - '0');
        if (*tz == ':') {
            ++tz;
            while (*tz >= '0' && *tz <= '9')
                secs = secs * 10 + (*tz++ - '0');
        }
    }
    z->bias = sign * (hours * 60 + mins) + (secs >= 30 ? sign : 0);
    z->std_bias = 0;
    n = 0;
    while ((tz[n] >= 'A' && tz[n] <= 'Z') || (tz[n] >= 'a' && tz[n] <= 'z')) {
        if (n < 63)
            z->dst_name[n] = tz[n];
        ++n;
    }
    if (n >= 3) {
        z->dst_bias = -60;
        z->dst_date.month = 3;
        z->dst_date.dow = 0;
        z->dst_date.day = 2;
        z->dst_date.hour = 2;
        z->std_date.month = 11;
        z->std_date.dow = 0;
        z->std_date.day = 1;
        z->std_date.hour = 2;
    } else {
        ucrt_strcpy(z->dst_name, z->std_name);
    }
    return 0;
}

/* _tzset: reads the TZ environment variable, else the system's time zone. */
static inline void ucrt_tzset(void)
{
    const char *tz = g_ucrt_time_ops && g_ucrt_time_ops->getenv ? g_ucrt_time_ops->getenv("TZ") : NULL;
    ucrt_tz_state *s = &g_ucrt_tz;
    __builtin_memset(&s->tz, 0, sizeof(s->tz));
    if (tz && *tz && ucrt_parse_tz(tz, &s->tz) == 0) {
        /* a TZ string in effect */
    } else if (g_ucrt_time_ops && g_ucrt_time_ops->get_tz && g_ucrt_time_ops->get_tz(&s->tz) == 0) {
        /* the system's zone */
    } else {
        ucrt_strcpy(s->tz.std_name, "UTC");
        ucrt_strcpy(s->tz.dst_name, "UTC");
    }
    s->loaded = 1;
    ucrt_tz_fill_state();
}

static inline void ucrt_tz_ensure(void)
{
    if (!g_ucrt_tz.loaded)
        ucrt_tzset();
}

/* ---- Conversions ---------------------------------------------------------------------------------------- */

/* gmtime and localtime reject what the Microsoft runtime rejects: before 1970 and after the year 3000. */
static inline ucrt_errno_t ucrt_gmtime64_s(ucrt_tm *tm, const int64_t *t)
{
    if (!tm)
        return ucrt_invalid(UCRT_EINVAL);
    __builtin_memset(tm, 0xFF, sizeof(*tm));
    if (!t)
        return ucrt_invalid(UCRT_EINVAL);
    if (*t < 0 || *t > UCRT_TIME_MAX_UTC) {
        ucrt_set_errno(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    ucrt_break_down(*t, tm);
    return 0;
}

static inline ucrt_errno_t ucrt_localtime64_s(ucrt_tm *tm, const int64_t *t)
{
    int dst;
    int64_t off;
    if (!tm)
        return ucrt_invalid(UCRT_EINVAL);
    __builtin_memset(tm, 0xFF, sizeof(*tm));
    if (!t)
        return ucrt_invalid(UCRT_EINVAL);
    if (*t < -43200 || *t > UCRT_TIME_MAX_LOCAL) { /* a day either side of the epoch is allowed for local time */
        ucrt_set_errno(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    ucrt_tz_ensure();
    off = ucrt_tz_offset_at(&g_ucrt_tz.tz, *t, &dst);
    if (*t + off < 0) {
        ucrt_set_errno(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    ucrt_break_down(*t + off, tm);
    tm->tm_isdst = dst;
    return 0;
}

/* Brings a tm with out-of-range fields into range and returns its calendar seconds (as if UTC). */
static inline int64_t ucrt_tm_to_seconds(ucrt_tm *tm)
{
    int64_t year = (int64_t)tm->tm_year + 1900, mon = tm->tm_mon, days, secs;
    year += mon >= 0 ? mon / 12 : -((-mon + 11) / 12);
    mon = mon >= 0 ? mon % 12 : (12 - (-mon) % 12) % 12;
    days = ucrt_days_from_civil(year, (int)mon + 1, 1) + (int64_t)tm->tm_mday - 1;
    secs = days * UCRT_SECS_PER_DAY + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    return secs;
}

static inline int64_t ucrt_mkgmtime64(ucrt_tm *tm)
{
    int64_t t;
    if (!tm) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    t = ucrt_tm_to_seconds(tm);
    if (t < 0 || t > UCRT_TIME_MAX_UTC) {
        ucrt_set_errno(UCRT_EINVAL);
        return -1;
    }
    ucrt_break_down(t, tm);
    return t;
}

static inline int64_t ucrt_mktime64(ucrt_tm *tm)
{
    int64_t local, t;
    int want = tm ? tm->tm_isdst : 0, dst;
    const ucrt_tzinfo *z;
    if (!tm) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_tz_ensure();
    z = &g_ucrt_tz.tz;
    local = ucrt_tm_to_seconds(tm);
    /* first guess: standard time; then see whether daylight saving time is in effect at that instant */
    t = local + (int64_t)(z->bias + z->std_bias) * 60;
    dst = ucrt_tz_dst_at(z, t);
    if (want < 0) {
        if (dst)
            t = local + (int64_t)(z->bias + z->dst_bias) * 60;
        /* a time in the hour skipped at the start of daylight saving time: keep the standard-time reading */
        if (dst && !ucrt_tz_dst_at(z, t))
            t = local + (int64_t)(z->bias + z->std_bias) * 60;
    } else if (want > 0 && ucrt_tz_has_dst(z)) {
        t = local + (int64_t)(z->bias + z->dst_bias) * 60;
    }
    if (t < 0 || t > UCRT_TIME_MAX_LOCAL) {
        ucrt_set_errno(UCRT_EINVAL);
        return -1;
    }
    {
        int isdst;
        int64_t off = ucrt_tz_offset_at(z, t, &isdst);
        ucrt_break_down(t + off, tm);
        tm->tm_isdst = isdst;
    }
    return t;
}

/* ---- Text forms --------------------------------------------------------------------------------------- */

static const char *const ucrt_day_abbr[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const ucrt_day_full[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const ucrt_month_abbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const ucrt_month_full[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static inline int ucrt_tm_valid(const ucrt_tm *tm)
{
    return tm && tm->tm_sec >= 0 && tm->tm_sec <= 60 && tm->tm_min >= 0 && tm->tm_min <= 59 && tm->tm_hour >= 0 && tm->tm_hour <= 23 &&
           tm->tm_mday >= 1 && tm->tm_mday <= 31 && tm->tm_mon >= 0 && tm->tm_mon <= 11 && tm->tm_year >= 0 && tm->tm_year <= 8099 &&
           tm->tm_wday >= 0 && tm->tm_wday <= 6 && tm->tm_yday >= 0 && tm->tm_yday <= 365;
}

/* "Www Mmm dd hh:mm:ss yyyy\n" and a NUL: 26 bytes. */
static inline ucrt_errno_t ucrt_asctime_s(char *buf, size_t size, const ucrt_tm *tm)
{
    static const char digits[] = "0123456789";
    int y;
    if (!buf || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    buf[0] = 0;
    if (size < 26)
        return ucrt_invalid(UCRT_ERANGE);
    if (!ucrt_tm_valid(tm))
        return ucrt_invalid(UCRT_EINVAL);
    y = tm->tm_year + 1900;
    ucrt_memcpy(buf, ucrt_day_abbr[tm->tm_wday], 3);
    buf[3] = ' ';
    ucrt_memcpy(buf + 4, ucrt_month_abbr[tm->tm_mon], 3);
    buf[7] = ' ';
    buf[8] = tm->tm_mday >= 10 ? digits[tm->tm_mday / 10] : ' ';
    buf[9] = digits[tm->tm_mday % 10];
    buf[10] = ' ';
    buf[11] = digits[tm->tm_hour / 10];
    buf[12] = digits[tm->tm_hour % 10];
    buf[13] = ':';
    buf[14] = digits[tm->tm_min / 10];
    buf[15] = digits[tm->tm_min % 10];
    buf[16] = ':';
    buf[17] = digits[tm->tm_sec / 10];
    buf[18] = digits[tm->tm_sec % 10];
    buf[19] = ' ';
    buf[20] = digits[y / 1000 % 10];
    buf[21] = digits[y / 100 % 10];
    buf[22] = digits[y / 10 % 10];
    buf[23] = digits[y % 10];
    buf[24] = '\n';
    buf[25] = 0;
    return 0;
}

static inline char *ucrt_asctime(const ucrt_tm *tm)
{
    static char buf[26];
    return ucrt_asctime_s(buf, sizeof buf, tm) ? NULL : buf;
}

static inline ucrt_errno_t ucrt_ctime64_s(char *buf, size_t size, const int64_t *t)
{
    ucrt_tm tm;
    if (!buf || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    buf[0] = 0;
    if (size < 26)
        return ucrt_invalid(UCRT_ERANGE);
    if (!t)
        return ucrt_invalid(UCRT_EINVAL);
    if (ucrt_localtime64_s(&tm, t))
        return UCRT_EINVAL;
    return ucrt_asctime_s(buf, size, &tm);
}

/* ---- strftime ------------------------------------------------------------------------------------------ */

static inline int ucrt_iso_week(const ucrt_tm *tm, int *iso_year)
{
    int year = tm->tm_year + 1900, wday = (tm->tm_wday + 6) % 7; /* Monday = 0 */
    int week = (tm->tm_yday - wday + 10) / 7;
    if (week < 1) {
        --year;
        week = (tm->tm_yday + 365 + ucrt_is_leap(year) - wday + 10) / 7;
        /* the last week of the previous year: 52 or 53 */
        {
            int pw_days = 365 + ucrt_is_leap(year);
            week = (tm->tm_yday + pw_days - wday + 10) / 7;
        }
    } else if (week == 53) {
        int days = 365 + ucrt_is_leap(year);
        if (tm->tm_yday - wday + 3 >= days) { /* Thursday of this week is in next year */
            week = 1;
            ++year;
        }
    }
    *iso_year = year;
    return week;
}

#define UCRT_CT char
#define UCRT_P ucrt_ft_
#include "ucrt_strftime_t.h"
#undef UCRT_CT
#undef UCRT_P

#define UCRT_CT ucrt_wchar
#define UCRT_P ucrt_fw_
#include "ucrt_strftime_t.h"
#undef UCRT_CT
#undef UCRT_P

static inline size_t ucrt_strftime(char *buf, size_t max, const char *fmt, const ucrt_tm *tm)
{
    return ucrt_ft_strftime(buf, max, fmt, tm);
}

static inline size_t ucrt_wcsftime(ucrt_wchar *buf, size_t max, const ucrt_wchar *fmt, const ucrt_tm *tm)
{
    return ucrt_fw_strftime(buf, max, fmt, tm);
}

/* ---- The clock --------------------------------------------------------------------------------------- */

#define UCRT_EPOCH_DIFF_100NS 116444736000000000ull /* 1601 to 1970 */

static inline int64_t ucrt_time64(int64_t *out)
{
    int64_t t = 0;
    if (g_ucrt_time_ops && g_ucrt_time_ops->now)
        t = (int64_t)((g_ucrt_time_ops->now() - UCRT_EPOCH_DIFF_100NS) / 10000000ull);
    if (out)
        *out = t;
    return t;
}

static inline int32_t ucrt_time32(int32_t *out)
{
    int32_t t = (int32_t)ucrt_time64(NULL);
    if (out)
        *out = t;
    return t;
}

typedef struct ucrt_timeb {
    int64_t time;
    unsigned short millitm;
    short timezone;
    short dstflag;
} ucrt_timeb;

static inline void ucrt_ftime64(ucrt_timeb *tb)
{
    uint64_t ft = g_ucrt_time_ops && g_ucrt_time_ops->now ? g_ucrt_time_ops->now() : UCRT_EPOCH_DIFF_100NS;
    int dst = 0;
    ucrt_tz_ensure();
    tb->time = (int64_t)((ft - UCRT_EPOCH_DIFF_100NS) / 10000000ull);
    tb->millitm = (unsigned short)(((ft - UCRT_EPOCH_DIFF_100NS) / 10000ull) % 1000);
    tb->timezone = (short)(g_ucrt_tz.tz.bias + g_ucrt_tz.tz.std_bias);
    ucrt_tz_offset_at(&g_ucrt_tz.tz, tb->time, &dst);
    tb->dstflag = (short)dst;
}

static inline int32_t ucrt_clock(void)
{
    if (g_ucrt_time_ops && g_ucrt_time_ops->process_ms)
        return (int32_t)g_ucrt_time_ops->process_ms();
    return -1;
}

static inline double ucrt_difftime64(int64_t a, int64_t b)
{
    return (double)(a - b);
}

#endif /* UCRT_TIME_H */
