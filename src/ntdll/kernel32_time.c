/*
 * kernel32_time.c - time shims (K32-M1): GetSystemTimeAsFileTime,
 * GetSystemTimePreciseAsFileTime, GetSystemTime, GetLocalTime, FileTimeToSystemTime,
 * SystemTimeToFileTime, FileTimeToLocalFileTime, LocalFileTimeToFileTime,
 * CompareFileTime, SystemTimeToTzSpecificLocalTime, TzSpecificLocalTimeToSystemTime,
 * GetTimeZoneInformation, GetDynamicTimeZoneInformation, GetSystemTimeAdjustment,
 * GetTickCount64, GetDateFormatW, GetTimeFormatW, GetDateFormatEx and GetTimeFormatEx.
 *
 * The clock is NtQuerySystemTime. The personality has no time zone database, so the
 * system time zone is UTC with no daylight rules: GetLocalTime equals GetSystemTime and
 * GetTimeZoneInformation reports a zero bias. A caller-supplied TIME_ZONE_INFORMATION is
 * honoured, daylight rules included. The rules are in k32_time_pure.h. Date and time
 * formats follow the en-US patterns whatever the LCID.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_time_pure.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif

#define K32M_ERROR_INVALID_PARAMETER 87u
#define K32M_ERROR_INSUFFICIENT_BUFFER 122u
#define K32M_TIME_ZONE_ID_UNKNOWN 0u

__declspec(dllimport) extern NTSTATUS NtQuerySystemTime(PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER, PLARGE_INTEGER);

void SetLastError(DWORD e);

typedef struct K32M_FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} K32M_FILETIME;

/* SYSTEMTIME has the layout of k32t_systemtime: eight WORDs. */
typedef k32t_systemtime K32M_SYSTEMTIME;

static uint64_t ft_get(const K32M_FILETIME *f)
{
    return ((uint64_t)f->dwHighDateTime << 32) | f->dwLowDateTime;
}

static void ft_put(K32M_FILETIME *f, uint64_t v)
{
    f->dwLowDateTime = (DWORD)v;
    f->dwHighDateTime = (DWORD)(v >> 32);
}

static uint64_t now_filetime(void)
{
    LARGE_INTEGER t;
    t.QuadPart = 0;
    NtQuerySystemTime(&t);
    return (uint64_t)t.QuadPart;
}

void WINAPI GetSystemTimeAsFileTime(K32M_FILETIME *out)
{
    if (out)
        ft_put(out, now_filetime());
}

void WINAPI GetSystemTimePreciseAsFileTime(K32M_FILETIME *out)
{
    GetSystemTimeAsFileTime(out);
}

void WINAPI GetSystemTime(K32M_SYSTEMTIME *out)
{
    if (out)
        k32t_filetime_to_systemtime(now_filetime(), out);
}

/* The system time zone is UTC. */
void WINAPI GetLocalTime(K32M_SYSTEMTIME *out)
{
    GetSystemTime(out);
}

BOOL WINAPI FileTimeToSystemTime(const K32M_FILETIME *ft, K32M_SYSTEMTIME *st)
{
    if (!ft || !st || !k32t_filetime_to_systemtime(ft_get(ft), st)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

BOOL WINAPI SystemTimeToFileTime(const K32M_SYSTEMTIME *st, K32M_FILETIME *ft)
{
    uint64_t v;
    if (!st || !ft || !k32t_systemtime_to_filetime(st, &v)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ft_put(ft, v);
    return 1;
}

BOOL WINAPI FileTimeToLocalFileTime(const K32M_FILETIME *in, K32M_FILETIME *out)
{
    if (!in || !out || ft_get(in) > K32T_FILETIME_MAX) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ft_put(out, ft_get(in)); /* local time is UTC */
    return 1;
}

BOOL WINAPI LocalFileTimeToFileTime(const K32M_FILETIME *in, K32M_FILETIME *out)
{
    return FileTimeToLocalFileTime(in, out);
}

LONG WINAPI CompareFileTime(const K32M_FILETIME *a, const K32M_FILETIME *b)
{
    return k32t_compare(ft_get(a), ft_get(b));
}

/* ---- Time zones --------------------------------------------------------------------- */

/* TIME_ZONE_INFORMATION (172 bytes): Bias, StandardName[32], StandardDate, StandardBias,
 * DaylightName[32], DaylightDate, DaylightBias. */
#define K32M_TZI_BIAS 0
#define K32M_TZI_STDNAME 4
#define K32M_TZI_STDDATE 68
#define K32M_TZI_STDBIAS 84
#define K32M_TZI_DLNAME 88
#define K32M_TZI_DLDATE 152
#define K32M_TZI_DLBIAS 168
#define K32M_TZI_SIZE 172

static int32_t tzi_i32(const uint8_t *p, size_t off)
{
    return (int32_t)((uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
                     ((uint32_t)p[off + 3] << 24));
}

static void tzi_get_date(const uint8_t *p, size_t off, k32t_systemtime *st)
{
    uint16_t *f = (uint16_t *)st;
    for (int i = 0; i < 8; ++i)
        f[i] = (uint16_t)(p[off + 2 * i] | (p[off + 2 * i + 1] << 8));
}

static void tzi_to_zone(const uint8_t *tzi, k32t_zone *z)
{
    z->bias = tzi_i32(tzi, K32M_TZI_BIAS);
    z->standard_bias = tzi_i32(tzi, K32M_TZI_STDBIAS);
    z->daylight_bias = tzi_i32(tzi, K32M_TZI_DLBIAS);
    tzi_get_date(tzi, K32M_TZI_STDDATE, &z->standard_date);
    tzi_get_date(tzi, K32M_TZI_DLDATE, &z->daylight_date);
}

static void put_wide(uint8_t *p, size_t off, const char *s, size_t cap)
{
    size_t i = 0;
    for (; s[i] && i + 1 < cap; ++i) {
        p[off + 2 * i] = (uint8_t)s[i];
        p[off + 2 * i + 1] = 0;
    }
    for (; i < cap; ++i) {
        p[off + 2 * i] = 0;
        p[off + 2 * i + 1] = 0;
    }
}

static void fill_utc_tzi(uint8_t *tzi)
{
    for (size_t i = 0; i < K32M_TZI_SIZE; ++i)
        tzi[i] = 0;
    put_wide(tzi, K32M_TZI_STDNAME, "Coordinated Universal Time", 32);
    put_wide(tzi, K32M_TZI_DLNAME, "Coordinated Universal Time", 32);
}

DWORD WINAPI GetTimeZoneInformation(void *tzi)
{
    if (!tzi) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFFu;
    }
    fill_utc_tzi((uint8_t *)tzi);
    return K32M_TIME_ZONE_ID_UNKNOWN;
}

/* DYNAMIC_TIME_ZONE_INFORMATION: a TIME_ZONE_INFORMATION, then TimeZoneKeyName[128] and a flag. */
DWORD WINAPI GetDynamicTimeZoneInformation(void *dtzi)
{
    uint8_t *p = (uint8_t *)dtzi;
    if (!p) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFFu;
    }
    fill_utc_tzi(p);
    put_wide(p, K32M_TZI_SIZE, "UTC", 128);
    p[K32M_TZI_SIZE + 256] = 1; /* DynamicDaylightTimeDisabled */
    for (size_t i = K32M_TZI_SIZE + 257; i < 432; ++i)
        p[i] = 0;
    return K32M_TIME_ZONE_ID_UNKNOWN;
}

BOOL WINAPI SystemTimeToTzSpecificLocalTime(const void *tzi, const K32M_SYSTEMTIME *utc, K32M_SYSTEMTIME *local)
{
    k32t_zone z = {0};
    uint64_t u, l;
    if (!utc || !local || !k32t_systemtime_to_filetime(utc, &u)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (tzi)
        tzi_to_zone((const uint8_t *)tzi, &z);
    if (!k32t_utc_to_local(&z, u, &l) || !k32t_filetime_to_systemtime(l, local)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

BOOL WINAPI TzSpecificLocalTimeToSystemTime(const void *tzi, const K32M_SYSTEMTIME *local, K32M_SYSTEMTIME *utc)
{
    k32t_zone z = {0};
    uint64_t l, u;
    if (!local || !utc || !k32t_systemtime_to_filetime(local, &l)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (tzi)
        tzi_to_zone((const uint8_t *)tzi, &z);
    if (!k32t_local_to_utc(&z, l, &u) || !k32t_filetime_to_systemtime(u, utc)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

/* The clock advances 15.625 ms (156250 ticks) per interrupt, and nothing adjusts it. */
BOOL WINAPI GetSystemTimeAdjustment(DWORD *adjustment, DWORD *increment, BOOL *disabled)
{
    if (!adjustment || !increment || !disabled) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *adjustment = 156250;
    *increment = 156250;
    *disabled = 1;
    return 1;
}

ULONGLONG WINAPI GetTickCount64(void)
{
    LARGE_INTEGER count, freq;
    uint64_t c, f;
    count.QuadPart = 0;
    freq.QuadPart = 0;
    if (!NT_SUCCESS(NtQueryPerformanceCounter(&count, &freq)) || freq.QuadPart <= 0)
        return 0;
    c = (uint64_t)count.QuadPart;
    f = (uint64_t)freq.QuadPart;
    /* Split the division so count * 1000 cannot overflow. */
    return (c / f) * 1000u + ((c % f) * 1000u) / f;
}

/* ---- Date and time text ----------------------------------------------------------------- */

/* A format picture as narrow text; characters above 0x7F become '?'. Returns 0 when it does not fit. */
static int narrow_pattern(const WCHAR *w, char *out, size_t cap)
{
    size_t i = 0;
    for (; w[i]; ++i) {
        if (i + 1 >= cap)
            return 0;
        out[i] = w[i] < 0x80 ? (char)w[i] : '?';
    }
    out[i] = 0;
    return 1;
}

static int pick_time(const K32M_SYSTEMTIME *in, K32M_SYSTEMTIME *out)
{
    if (!in) {
        GetLocalTime(out);
        return 1;
    }
    {
        uint64_t ignored;
        if (!k32t_systemtime_to_filetime(in, &ignored))
            return 0;
    }
    *out = *in;
    /* The day of week is recomputed: callers often leave it zero. */
    {
        uint64_t ft;
        k32t_systemtime tmp = {0};
        k32t_systemtime_to_filetime(in, &ft);
        k32t_filetime_to_systemtime(ft, &tmp);
        out->day_of_week = tmp.day_of_week;
    }
    return 1;
}

static int format_common(const K32M_SYSTEMTIME *when, const WCHAR *picture, const char *fallback, WCHAR *buf, int cch)
{
    K32M_SYSTEMTIME t = {0};
    char pat[128];
    int fits = 1;
    size_t n;
    if (cch < 0 || (cch > 0 && !buf) || !pick_time(when, &t)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (picture) {
        if (!narrow_pattern(picture, pat, sizeof(pat))) {
            SetLastError(K32M_ERROR_INVALID_PARAMETER);
            return 0;
        }
    } else {
        size_t i = 0;
        for (; fallback[i]; ++i)
            pat[i] = fallback[i];
        pat[i] = 0;
    }
    n = k32t_format(cch ? (uint16_t *)buf : NULL, (size_t)cch, pat, &t, &fits);
    if (!fits || n == 0) {
        SetLastError(K32M_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return (int)n;
}

int WINAPI GetDateFormatW(DWORD lcid, DWORD flags, const K32M_SYSTEMTIME *when, const WCHAR *picture, WCHAR *buf, int cch)
{
    (void)lcid;
    if (picture && flags != 0) { /* a picture cannot be combined with date flags */
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return format_common(when, picture, k32t_default_date_pattern(flags), buf, cch);
}

int WINAPI GetTimeFormatW(DWORD lcid, DWORD flags, const K32M_SYSTEMTIME *when, const WCHAR *picture, WCHAR *buf, int cch)
{
    (void)lcid;
    return format_common(when, picture, k32t_default_time_pattern(picture ? 0 : flags), buf, cch);
}

int WINAPI GetDateFormatEx(const WCHAR *locale, DWORD flags, const K32M_SYSTEMTIME *when, const WCHAR *picture, WCHAR *buf,
                           int cch, const WCHAR *calendar)
{
    (void)locale;
    (void)calendar;
    return GetDateFormatW(0, flags, when, picture, buf, cch);
}

int WINAPI GetTimeFormatEx(const WCHAR *locale, DWORD flags, const K32M_SYSTEMTIME *when, const WCHAR *picture, WCHAR *buf,
                           int cch)
{
    (void)locale;
    return GetTimeFormatW(0, flags, when, picture, buf, cch);
}
#endif
