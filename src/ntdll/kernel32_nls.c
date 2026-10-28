/*
 * kernel32_nls.c - national language support: GetLocaleInfo(W/A/Ex), the default locale functions, CompareString(W/A/Ex/
 * Ordinal), LCMapString(W/A/Ex), GetStringType(W/A/Ex), FoldString, GetNumberFormat, GetCurrencyFormat and the locale
 * enumeration and validity functions, for the en-US locale. The rules are in k32_nls_pure.h.
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
#include "k32_nls_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif

typedef PVOID LPVOID;
typedef LONG_PTR LPARAM;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef UINT
#define UINT unsigned int
#endif

#define KN_ERROR_INVALID_PARAMETER 87u
#define KN_ERROR_INSUFFICIENT_BUFFER 122u
#define KN_ERROR_INVALID_FLAGS 1004u

void SetLastError(DWORD e);

static DWORD g_kn_thread_locale = K32N_LCID_EN_US;

static int kn_known_lcid(DWORD lcid)
{
    return k32n_lcid_name(lcid, NULL, 0) > 0;
}

/* The LCID a Win32 LCID argument stands for (user and system defaults map to en-US), or 0 for an unknown locale. */
static DWORD kn_resolve_lcid(DWORD lcid)
{
    if (lcid == K32N_LCID_USER_DEFAULT || lcid == K32N_LCID_SYSTEM_DEFAULT || lcid == 0x0C00 /* LOCALE_CUSTOM_DEFAULT */ || lcid == 0x1000)
        return K32N_LCID_EN_US;
    return kn_known_lcid(lcid) ? lcid : 0;
}

/* ---- Locale defaults ------------------------------------------------------------------------------------------- */

DWORD WINAPI GetUserDefaultLCID(void) { return K32N_LCID_EN_US; }
DWORD WINAPI GetSystemDefaultLCID(void) { return K32N_LCID_EN_US; }
WORD WINAPI GetUserDefaultLangID(void) { return 0x0409; }
WORD WINAPI GetSystemDefaultLangID(void) { return 0x0409; }
WORD WINAPI GetUserDefaultUILanguage(void) { return 0x0409; }
WORD WINAPI GetSystemDefaultUILanguage(void) { return 0x0409; }
WORD WINAPI GetThreadUILanguage(void) { return 0x0409; }
DWORD WINAPI GetThreadLocale(void) { return g_kn_thread_locale; }

BOOL WINAPI SetThreadLocale(DWORD lcid)
{
    if (!kn_resolve_lcid(lcid)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    g_kn_thread_locale = kn_resolve_lcid(lcid);
    return TRUE;
}

BOOL WINAPI IsValidLocale(DWORD lcid, DWORD flags)
{
    (void)flags;
    return kn_resolve_lcid(lcid) ? TRUE : FALSE;
}

BOOL WINAPI IsValidLocaleName(const WCHAR *name)
{
    return (name && k32n_name_to_lcid(name)) ? TRUE : FALSE;
}

int WINAPI GetUserDefaultLocaleName(WCHAR *buf, int cap)
{
    int n = k32n_lcid_name(K32N_LCID_EN_US, buf, cap);
    if (n < 0) {
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return n;
}

int WINAPI GetSystemDefaultLocaleName(WCHAR *buf, int cap)
{
    return GetUserDefaultLocaleName(buf, cap);
}

int WINAPI LCIDToLocaleName(DWORD lcid, WCHAR *buf, int cap, DWORD flags)
{
    int n;
    (void)flags;
    n = k32n_lcid_name(lcid == 0 ? K32N_LCID_EN_US : lcid, buf, cap);
    if (n == 0) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n < 0) {
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return buf ? n : n;
}

DWORD WINAPI LocaleNameToLCID(const WCHAR *name, DWORD flags)
{
    DWORD l = k32n_name_to_lcid(name);
    (void)flags;
    if (!l)
        SetLastError(KN_ERROR_INVALID_PARAMETER);
    return l;
}

BOOL WINAPI GetUserPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *buf, ULONG *cap)
{
    static const WCHAR names[] = {'e', 'n', '-', 'U', 'S', 0, 0};
    (void)flags;
    if (!count || !cap) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!buf) {
        *count = 1;
        *cap = 7;
        return TRUE;
    }
    if (*cap < 7) {
        *cap = 7;
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, names, sizeof(names));
    *count = 1;
    *cap = 7;
    return TRUE;
}

BOOL WINAPI SetThreadPreferredUILanguages(DWORD flags, const WCHAR *langs, ULONG *count)
{
    (void)flags;
    (void)langs;
    if (count)
        *count = 1;
    return TRUE;
}

typedef BOOL(WINAPI *KN_ENUM_LOCALES_EX)(WCHAR *, DWORD, LPARAM);
typedef BOOL(WINAPI *KN_ENUM_LOCALES)(WCHAR *);

BOOL WINAPI EnumSystemLocalesEx(KN_ENUM_LOCALES_EX proc, DWORD flags, LPARAM lparam, void *reserved)
{
    static WCHAR name[] = {'e', 'n', '-', 'U', 'S', 0};
    (void)flags;
    (void)reserved;
    if (!proc) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    proc(name, 0, lparam);
    return TRUE;
}

BOOL WINAPI EnumSystemLocalesW(KN_ENUM_LOCALES proc, DWORD flags)
{
    static WCHAR id[] = {'0', '0', '0', '0', '0', '4', '0', '9', 0};
    (void)flags;
    if (!proc) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    proc(id);
    return TRUE;
}

typedef struct {
    DWORD dwNLSVersionInfoSize;
    DWORD dwNLSVersion;
    DWORD dwDefinedVersion;
} KN_NLSVERSIONINFO;

BOOL WINAPI GetNLSVersion(DWORD func, DWORD lcid, KN_NLSVERSIONINFO *v)
{
    (void)func;
    (void)lcid;
    if (!v) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    v->dwNLSVersion = 0x00060101;
    v->dwDefinedVersion = 0x00060101;
    return TRUE;
}

BOOL WINAPI GetNLSVersionEx(DWORD func, const WCHAR *name, KN_NLSVERSIONINFO *v)
{
    (void)name;
    return GetNLSVersion(func, 0, v);
}

/* ---- GetLocaleInfo ---------------------------------------------------------------------------------------------- */

static int kn_locale_info(DWORD lcid, DWORD type, WCHAR *buf, int cap)
{
    DWORD flags = type & K32N_LOCALE_RETURN_NUMBER, t = type & ~(K32N_LOCALE_RETURN_NUMBER | K32N_LOCALE_NOUSEROVERRIDE | 0x08000000u);
    const char *s;
    if (!kn_resolve_lcid(lcid)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = k32n_locale_string(t);
    if (!s) {
        SetLastError(KN_ERROR_INVALID_FLAGS);
        return 0;
    }
    if (flags) {
        if (!k32n_locale_is_number(t)) {
            SetLastError(KN_ERROR_INVALID_FLAGS);
            return 0;
        }
        if (cap == 0)
            return 2; /* a DWORD is two WCHARs */
        if (cap < 2) {
            SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        *(DWORD *)buf = k32n_locale_number(t, s);
        return 2;
    }
    {
        int n = 0;
        while (s[n])
            ++n;
        if (cap == 0)
            return n + 1;
        if (n + 1 > cap) {
            SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (int i = 0; i <= n; ++i)
            buf[i] = (WCHAR)(unsigned char)s[i];
        return n + 1;
    }
}

int WINAPI GetLocaleInfoW(DWORD lcid, DWORD type, WCHAR *buf, int cap)
{
    return kn_locale_info(lcid, type, buf, cap);
}

int WINAPI GetLocaleInfoEx(const WCHAR *name, DWORD type, WCHAR *buf, int cap)
{
    DWORD lcid = k32n_name_to_lcid(name);
    if (!lcid) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return kn_locale_info(lcid, type, buf, cap);
}

int WINAPI GetLocaleInfoA(DWORD lcid, DWORD type, char *buf, int cap)
{
    WCHAR tmp[128];
    int n = kn_locale_info(lcid, type, tmp, 128);
    if (n == 0)
        return 0;
    if (type & K32N_LOCALE_RETURN_NUMBER) {
        if (cap == 0)
            return 4;
        if (cap < 4) {
            SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memcpy(buf, tmp, 4);
        return 4;
    }
    if (cap == 0)
        return n;
    if (n > cap) {
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i < n; ++i)
        buf[i] = (char)tmp[i];
    return n;
}

/* ---- Comparison ---------------------------------------------------------------------------------------------------- */

int WINAPI CompareStringW(DWORD lcid, DWORD flags, const WCHAR *s1, int n1, const WCHAR *s2, int n2)
{
    if (!s1 || !s2 || !kn_resolve_lcid(lcid)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & ~(K32N_NORM_IGNORECASE | K32N_NORM_IGNORENONSPACE | K32N_NORM_IGNORESYMBOLS | K32N_LINGUISTIC_IGNORECASE | K32N_SORT_STRINGSORT | 0x10000u | 0x20000u | 0x20u | 0x40u | 0x80000000u)) {
        SetLastError(KN_ERROR_INVALID_FLAGS);
        return 0;
    }
    return k32n_compare(flags, s1, k32n_len(s1, n1), s2, k32n_len(s2, n2));
}

int WINAPI CompareStringEx(const WCHAR *name, DWORD flags, const WCHAR *s1, int n1, const WCHAR *s2, int n2, void *version, void *reserved, LPARAM lparam)
{
    DWORD lcid = k32n_name_to_lcid(name);
    (void)version;
    (void)reserved;
    (void)lparam;
    if (!lcid) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CompareStringW(lcid, flags, s1, n1, s2, n2);
}

int WINAPI CompareStringOrdinal(const WCHAR *s1, int n1, const WCHAR *s2, int n2, BOOL ignore_case)
{
    if (!s1 || !s2) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return k32n_compare_ordinal(s1, k32n_len(s1, n1), s2, k32n_len(s2, n2), ignore_case ? 1 : 0);
}

int WINAPI CompareStringA(DWORD lcid, DWORD flags, const char *s1, int n1, const char *s2, int n2)
{
    WCHAR a[512], b[512];
    int la = 0, lb = 0;
    if (!s1 || !s2) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n1 < 0) {
        n1 = 0;
        while (s1[n1])
            ++n1;
    }
    if (n2 < 0) {
        n2 = 0;
        while (s2[n2])
            ++n2;
    }
    for (; la < n1 && la < 511; ++la)
        a[la] = (WCHAR)(unsigned char)s1[la];
    for (; lb < n2 && lb < 511; ++lb)
        b[lb] = (WCHAR)(unsigned char)s2[lb];
    return CompareStringW(lcid, flags, a, la, b, lb);
}

/* ---- Case mapping and character types --------------------------------------------------------------------------- */

int WINAPI LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap)
{
    int len, r;
    if (!src || cap < 0 || !kn_resolve_lcid(lcid) || (!dst && cap)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    len = k32n_len(src, n);
    if (n < 0)
        ++len; /* the terminating NUL is part of the string when the length was -1 */
    r = k32n_lcmap(flags, src, len, dst, cap);
    if (r == -1) {
        SetLastError(KN_ERROR_INVALID_FLAGS);
        return 0;
    }
    if (r == -2) {
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return r;
}

int WINAPI LCMapStringEx(const WCHAR *name, DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap, void *version, void *reserved, LPARAM lparam)
{
    DWORD lcid = k32n_name_to_lcid(name);
    (void)version;
    (void)reserved;
    (void)lparam;
    if (!lcid) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return LCMapStringW(lcid, flags, src, n, dst, cap);
}

int WINAPI LCMapStringA(DWORD lcid, DWORD flags, const char *src, int n, char *dst, int cap)
{
    WCHAR w[512], o[512];
    int len = 0, r;
    if (!src) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n < 0) {
        n = 0;
        while (src[n])
            ++n;
        ++n;
    }
    for (; len < n && len < 512; ++len)
        w[len] = (WCHAR)(unsigned char)src[len];
    r = LCMapStringW(lcid, flags, w, len, cap ? o : NULL, cap > 512 ? 512 : cap);
    if (r == 0)
        return 0;
    if (cap) {
        if (r > cap) {
            SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (int i = 0; i < r; ++i)
            dst[i] = o[i] <= 0xFF ? (char)o[i] : '?';
    }
    return r;
}

BOOL WINAPI GetStringTypeW(DWORD type, const WCHAR *src, int n, WORD *out)
{
    int len;
    if (!src || !out || (type != K32N_CT_CTYPE1 && type != K32N_CT_CTYPE2 && type != K32N_CT_CTYPE3)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    len = k32n_len(src, n);
    if (n < 0)
        ++len;
    for (int i = 0; i < len; ++i)
        out[i] = type == K32N_CT_CTYPE1 ? k32n_ctype1(src[i]) : type == K32N_CT_CTYPE2 ? k32n_ctype2(src[i]) : k32n_ctype3(src[i]);
    return TRUE;
}

BOOL WINAPI GetStringTypeExW(DWORD lcid, DWORD type, const WCHAR *src, int n, WORD *out)
{
    (void)lcid;
    return GetStringTypeW(type, src, n, out);
}

BOOL WINAPI GetStringTypeA(DWORD lcid, DWORD type, const char *src, int n, WORD *out)
{
    WCHAR w[512];
    int len = 0;
    (void)lcid;
    if (!src || !out) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (n < 0) {
        n = 0;
        while (src[n])
            ++n;
        ++n;
    }
    for (; len < n && len < 512; ++len)
        w[len] = (WCHAR)(unsigned char)src[len];
    return GetStringTypeW(type, w, len, out);
}

BOOL WINAPI GetStringTypeExA(DWORD lcid, DWORD type, const char *src, int n, WORD *out)
{
    return GetStringTypeA(lcid, type, src, n, out);
}

/* FoldString: only the flags that need no tables (MAP_FOLDCZONE 0x10 and MAP_PRECOMPOSED 0x20 leave ASCII and Latin-1 as
 * they are; MAP_EXPAND_LIGATURES 0x2 is not applied). */
int WINAPI FoldStringW(DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap)
{
    int len;
    if (!src || (flags & ~0x72u)) {
        SetLastError(KN_ERROR_INVALID_FLAGS);
        return 0;
    }
    len = k32n_len(src, n);
    if (n < 0)
        ++len;
    if (cap == 0)
        return len;
    if (cap < len) {
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i < len; ++i)
        dst[i] = src[i];
    return len;
}

/* ---- Numbers and money ------------------------------------------------------------------------------------------ */

/* NUMBERFMTW and CURRENCYFMTW: the overrides a caller can pass. */
typedef struct {
    UINT NumDigits;
    UINT LeadingZero;
    UINT Grouping;
    WCHAR *lpDecimalSep;
    WCHAR *lpThousandSep;
    UINT NegativeOrder;
} KN_NUMBERFMTW;

typedef struct {
    UINT NumDigits;
    UINT LeadingZero;
    UINT Grouping;
    WCHAR *lpDecimalSep;
    WCHAR *lpThousandSep;
    UINT NegativeOrder;
    UINT PositiveOrder;
    WCHAR *lpCurrencySymbol;
} KN_CURRENCYFMTW;

static void kn_narrow(const WCHAR *w, char *out, size_t cap)
{
    size_t i = 0;
    for (; w[i] && i + 1 < cap; ++i)
        out[i] = w[i] < 0x80 ? (char)w[i] : '?';
    out[i] = 0;
}

static int kn_emit(const char *s, WCHAR *buf, int cap)
{
    int n = 0;
    while (s[n])
        ++n;
    if (cap == 0)
        return n + 1;
    if (n + 1 > cap) {
        SetLastError(KN_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i <= n; ++i)
        buf[i] = (WCHAR)(unsigned char)s[i];
    return n + 1;
}

int WINAPI GetNumberFormatW(DWORD lcid, DWORD flags, const WCHAR *value, const KN_NUMBERFMTW *fmt, WCHAR *buf, int cap)
{
    char num[128], dec[8] = ".", tho[8] = ",", out[256];
    int digits = 2, grouping = 3, r;
    (void)flags;
    if (!value || !kn_resolve_lcid(lcid) || cap < 0 || (!buf && cap)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    kn_narrow(value, num, sizeof(num));
    if (fmt) {
        digits = (int)fmt->NumDigits;
        grouping = fmt->Grouping > 9 ? 3 : (int)fmt->Grouping; /* the Grouping field is the digits in the first group */
        if (fmt->lpDecimalSep)
            kn_narrow(fmt->lpDecimalSep, dec, sizeof(dec));
        if (fmt->lpThousandSep)
            kn_narrow(fmt->lpThousandSep, tho, sizeof(tho));
    }
    r = k32n_format_number(num, digits, grouping, dec, tho, out, sizeof(out));
    if (r < 0) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return kn_emit(out, buf, cap);
}

int WINAPI GetNumberFormatEx(const WCHAR *name, DWORD flags, const WCHAR *value, const KN_NUMBERFMTW *fmt, WCHAR *buf, int cap)
{
    DWORD lcid = k32n_name_to_lcid(name);
    if (!lcid) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return GetNumberFormatW(lcid, flags, value, fmt, buf, cap);
}

int WINAPI GetCurrencyFormatW(DWORD lcid, DWORD flags, const WCHAR *value, const KN_CURRENCYFMTW *fmt, WCHAR *buf, int cap)
{
    char num[128], dec[8] = ".", tho[8] = ",", sym[16] = "$", out[256], full[300];
    int digits = 2, grouping = 3, r, neg;
    (void)flags;
    if (!value || !kn_resolve_lcid(lcid) || cap < 0 || (!buf && cap)) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    kn_narrow(value, num, sizeof(num));
    if (fmt) {
        digits = (int)fmt->NumDigits;
        grouping = fmt->Grouping > 9 ? 3 : (int)fmt->Grouping;
        if (fmt->lpDecimalSep)
            kn_narrow(fmt->lpDecimalSep, dec, sizeof(dec));
        if (fmt->lpThousandSep)
            kn_narrow(fmt->lpThousandSep, tho, sizeof(tho));
        if (fmt->lpCurrencySymbol)
            kn_narrow(fmt->lpCurrencySymbol, sym, sizeof(sym));
    }
    r = k32n_format_number(num, digits, grouping, dec, tho, out, sizeof(out));
    if (r < 0) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    neg = out[0] == '-';
    /* the US form: $1,234.50 and ($1,234.50) for a negative amount */
    {
        size_t o = 0;
        if (neg)
            full[o++] = '(';
        for (const char *s = sym; *s && o < sizeof(full) - 2; ++s)
            full[o++] = *s;
        for (const char *s = out + (neg ? 1 : 0); *s && o < sizeof(full) - 2; ++s)
            full[o++] = *s;
        if (neg)
            full[o++] = ')';
        full[o] = 0;
    }
    return kn_emit(full, buf, cap);
}

int WINAPI GetCurrencyFormatEx(const WCHAR *name, DWORD flags, const WCHAR *value, const KN_CURRENCYFMTW *fmt, WCHAR *buf, int cap)
{
    DWORD lcid = k32n_name_to_lcid(name);
    if (!lcid) {
        SetLastError(KN_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return GetCurrencyFormatW(lcid, flags, value, fmt, buf, cap);
}

#endif /* __MINGW32__ */
