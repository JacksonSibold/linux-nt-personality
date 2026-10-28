/*
 * k32_nls_pure.h - the national language support rules for kernel32_nls.c, for the en-US locale (and the invariant one):
 * locale information strings, string comparison with the default sort, case mapping, character types, and number and
 * currency formatting. Host tests: tests/ntdll-host/test_k32_nls_pure.c.
 *
 * Only one locale is modelled. Every locale name that resolves (en-US, en, the empty invariant name, the user default) gets its
 * data; others are unknown to the caller.
 */
#ifndef NTDLL_K32_NLS_PURE_H
#define NTDLL_K32_NLS_PURE_H

#include "nt_types.h"
#include <stddef.h>
#include <stdint.h>

#include "../crt/ucrt_ctype.h"

#define K32N_LCID_EN_US 0x0409u
#define K32N_LCID_INVARIANT 0x007Fu
#define K32N_LCID_USER_DEFAULT 0x0400u
#define K32N_LCID_SYSTEM_DEFAULT 0x0800u

#define K32N_LOCALE_RETURN_NUMBER 0x20000000u
#define K32N_LOCALE_NOUSEROVERRIDE 0x80000000u

#define K32N_CSTR_LESS_THAN 1
#define K32N_CSTR_EQUAL 2
#define K32N_CSTR_GREATER_THAN 3

#define K32N_NORM_IGNORECASE 0x00000001u
#define K32N_NORM_IGNORENONSPACE 0x00000002u
#define K32N_NORM_IGNORESYMBOLS 0x00000004u
#define K32N_LINGUISTIC_IGNORECASE 0x00000010u
#define K32N_SORT_STRINGSORT 0x00001000u

#define K32N_LCMAP_LOWERCASE 0x00000100u
#define K32N_LCMAP_UPPERCASE 0x00000200u
#define K32N_LCMAP_SORTKEY 0x00000400u

static inline WCHAR k32n_upper(WCHAR c) { return (WCHAR)ucrt_towupper((ucrt_wchar)c); }
static inline WCHAR k32n_lower(WCHAR c) { return (WCHAR)ucrt_towlower((ucrt_wchar)c); }

/* ---- Locale names --------------------------------------------------------------------------------------------- */

static inline int k32n_name_eq(const WCHAR *a, const char *b)
{
    for (;; ++a, ++b) {
        WCHAR x = *a, y = (WCHAR)(unsigned char)*b;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* The LCID a locale name stands for, or 0 when the locale is not known. NULL (LOCALE_NAME_USER_DEFAULT) is the user's
 * locale; the empty string is the invariant one. */
static inline DWORD k32n_name_to_lcid(const WCHAR *name)
{
    if (!name)
        return K32N_LCID_EN_US;
    if (!*name)
        return K32N_LCID_INVARIANT;
    if (k32n_name_eq(name, "en-US") || k32n_name_eq(name, "en"))
        return K32N_LCID_EN_US;
    return 0;
}

/* The name of an LCID; 0 if unknown. Writes at most cap characters (with the NUL) and returns the length including it. */
static inline int k32n_lcid_name(DWORD lcid, WCHAR *out, int cap)
{
    const char *n = NULL;
    int len;
    if (lcid == K32N_LCID_EN_US || lcid == K32N_LCID_USER_DEFAULT || lcid == K32N_LCID_SYSTEM_DEFAULT || lcid == 0x0009)
        n = "en-US";
    else if (lcid == K32N_LCID_INVARIANT)
        n = "";
    else
        return 0;
    len = 0;
    while (n[len])
        ++len;
    if (out) {
        if (len + 1 > cap)
            return -1;
        for (int i = 0; i <= len; ++i)
            out[i] = (WCHAR)(unsigned char)n[i];
    }
    return len + 1;
}

/* ---- Locale information ------------------------------------------------------------------------------------- */

static const char *const k32n_days[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char *const k32n_days_abbr[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char *const k32n_months[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static const char *const k32n_months_abbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* The string for a LOCALE_ type (the number flag already removed), NULL when the type is not known. */
static inline const char *k32n_locale_string(DWORD type)
{
    if (type >= 0x2A && type <= 0x30)
        return k32n_days[type - 0x2A];
    if (type >= 0x31 && type <= 0x37)
        return k32n_days_abbr[type - 0x31];
    if (type >= 0x38 && type <= 0x43)
        return k32n_months[type - 0x38];
    if (type >= 0x44 && type <= 0x4F)
        return k32n_months_abbr[type - 0x44];
    switch (type) {
    case 0x01: return "0409";                       /* ILANGUAGE */
    case 0x02: return "English (United States)";    /* SLANGUAGE */
    case 0x03: return "ENU";                        /* SABBREVLANGNAME */
    case 0x04: return "English (United States)";    /* SNATIVELANGNAME */
    case 0x05: return "1";                          /* ICOUNTRY */
    case 0x06: return "United States";              /* SCOUNTRY */
    case 0x07: return "USA";                        /* SABBREVCTRYNAME */
    case 0x08: return "United States";              /* SNATIVECTRYNAME */
    case 0x09: return "0409";                       /* IDEFAULTLANGUAGE */
    case 0x0A: return "1";                          /* IDEFAULTCOUNTRY */
    case 0x0B: return "437";                        /* IDEFAULTCODEPAGE */
    case 0x0C: return ",";                          /* SLIST */
    case 0x0D: return "1";                          /* IMEASURE */
    case 0x0E: return ".";                          /* SDECIMAL */
    case 0x0F: return ",";                          /* STHOUSAND */
    case 0x10: return "3;0";                        /* SGROUPING */
    case 0x11: return "2";                          /* IDIGITS */
    case 0x12: return "1";                          /* ILZERO */
    case 0x13: return "0123456789";                 /* SNATIVEDIGITS */
    case 0x14: return "$";                          /* SCURRENCY */
    case 0x15: return "USD";                        /* SINTLSYMBOL */
    case 0x16: return ".";                          /* SMONDECIMALSEP */
    case 0x17: return ",";                          /* SMONTHOUSANDSEP */
    case 0x18: return "3;0";                        /* SMONGROUPING */
    case 0x19: return "2";                          /* ICURRDIGITS */
    case 0x1A: return "2";                          /* IINTLCURRDIGITS */
    case 0x1B: return "0";                          /* ICURRENCY */
    case 0x1C: return "0";                          /* INEGCURR */
    case 0x1D: return "/";                          /* SDATE */
    case 0x1E: return ":";                          /* STIME */
    case 0x1F: return "M/d/yyyy";                   /* SSHORTDATE */
    case 0x20: return "dddd, MMMM d, yyyy";         /* SLONGDATE */
    case 0x21: return "0";                          /* IDATE */
    case 0x22: return "2";                          /* ILDATE */
    case 0x23: return "0";                          /* ITIME */
    case 0x24: return "1";                          /* ICENTURY */
    case 0x25: return "0";                          /* ITLZERO */
    case 0x26: return "0";                          /* IDAYLZERO */
    case 0x27: return "0";                          /* IMONLZERO */
    case 0x28: return "AM";                         /* S1159 */
    case 0x29: return "PM";                         /* S2359 */
    case 0x50: return "";                           /* SPOSITIVESIGN */
    case 0x51: return "-";                          /* SNEGATIVESIGN */
    case 0x52: return "3";                          /* IPOSSIGNPOSN */
    case 0x53: return "0";                          /* INEGSIGNPOSN */
    case 0x54: return "1";                          /* IPOSSYMPRECEDES */
    case 0x55: return "0";                          /* IPOSSEPBYSPACE */
    case 0x56: return "1";                          /* INEGSYMPRECEDES */
    case 0x57: return "0";                          /* INEGSEPBYSPACE */
    case 0x59: return "en";                         /* SISO639LANGNAME */
    case 0x5A: return "US";                         /* SISO3166CTRYNAME */
    case 0x5C: return "en-US";                      /* SNAME */
    case 0x1001: return "English";                  /* SENGLANGUAGE */
    case 0x1002: return "United States";            /* SENGCOUNTRY */
    case 0x1003: return "h:mm:ss tt";               /* STIMEFORMAT */
    case 0x1004: return "1252";                     /* IDEFAULTANSICODEPAGE */
    case 0x1005: return "0";                        /* ITIMEMARKPOSN */
    case 0x1006: return "1";                        /* ICALENDARTYPE */
    case 0x1007: return "US Dollar";                /* SENGCURRNAME */
    case 0x1008: return "US Dollar";                /* SNATIVECURRNAME */
    case 0x100A: return "1";                        /* IPAPERSIZE */
    case 0x100B: return "0";                        /* IOPTIONALCALENDAR */
    case 0x100C: return "6";                        /* IFIRSTDAYOFWEEK */
    case 0x100D: return "0";                        /* IFIRSTWEEKOFYEAR */
    case 0x100E: return "";                         /* SMONTHNAME13 */
    case 0x100F: return "";                         /* SABBREVMONTHNAME13 */
    case 0x1010: return "1";                        /* INEGNUMBER */
    case 0x1011: return "10000";                    /* IDEFAULTMACCODEPAGE */
    case 0x1012: return "437";                      /* IDEFAULTEBCDICCODEPAGE */
    case 0x1013: return "Default";                  /* SSORTNAME */
    case 0x1014: return "0";                        /* IDIGITSUBSTITUTION */
    case 0x0022 + 0x7000: return NULL;
    default: return NULL;
    }
}

/* LOCALE_I... types return numbers with LOCALE_RETURN_NUMBER; this says whether a type is numeric. */
static inline int k32n_locale_is_number(DWORD type)
{
    switch (type) {
    case 0x01: case 0x05: case 0x09: case 0x0A: case 0x0B: case 0x0D: case 0x11: case 0x12: case 0x19: case 0x1A: case 0x1B: case 0x1C:
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56:
    case 0x57: case 0x1004: case 0x1005: case 0x1006: case 0x100A: case 0x100B: case 0x100C: case 0x100D: case 0x1010: case 0x1011:
    case 0x1012: case 0x1014:
        return 1;
    default:
        return 0;
    }
}

/* The numeric value of a numeric locale string ("0409" is a hexadecimal LCID, the rest are decimal). */
static inline DWORD k32n_locale_number(DWORD type, const char *s)
{
    DWORD v = 0;
    int hex = (type == 0x01 || type == 0x09);
    for (; *s; ++s) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0)
            break;
        v = v * (hex ? 16u : 10u) + (DWORD)d;
    }
    return v;
}

/* ---- Comparison ------------------------------------------------------------------------------------------------ */

/* The primary sort rank of a character: white space and symbols sort before digits, digits before letters. Letters compare
 * by their upper case form. */
static inline unsigned k32n_primary(WCHAR c)
{
    WCHAR u = k32n_upper(c);
    if ((c >= '0' && c <= '9'))
        return 0x10000u | c;
    if (ucrt_iswalpha((ucrt_wchar)c))
        return 0x20000u | u;
    return c; /* everything else, in code point order, below the digits */
}

static inline int k32n_ignorable(WCHAR c, DWORD flags)
{
    if (flags & K32N_NORM_IGNORESYMBOLS)
        return !(ucrt_iswalpha((ucrt_wchar)c) || (c >= '0' && c <= '9'));
    return 0;
}

/* Compares n1 characters of s1 with n2 of s2 (the counts are exact: the caller resolved -1). Returns CSTR_*. */
static inline int k32n_compare(DWORD flags, const WCHAR *s1, int n1, const WCHAR *s2, int n2)
{
    int i = 0, j = 0, tie = 0;
    int ignore_case = (flags & (K32N_NORM_IGNORECASE | K32N_LINGUISTIC_IGNORECASE)) != 0;
    for (;;) {
        while (i < n1 && k32n_ignorable(s1[i], flags))
            ++i;
        while (j < n2 && k32n_ignorable(s2[j], flags))
            ++j;
        if (i >= n1 || j >= n2)
            break;
        {
            unsigned a = k32n_primary(s1[i]), b = k32n_primary(s2[j]);
            if (a != b)
                return a < b ? K32N_CSTR_LESS_THAN : K32N_CSTR_GREATER_THAN;
            if (!ignore_case && !tie && s1[i] != s2[j]) {
                /* equal letters differing in case: lower case sorts first; remembered if nothing else differs */
                tie = (s1[i] == k32n_lower(s1[i]) && s1[i] != k32n_upper(s1[i])) ? K32N_CSTR_LESS_THAN : K32N_CSTR_GREATER_THAN;
            }
        }
        ++i;
        ++j;
    }
    if (i < n1)
        return K32N_CSTR_GREATER_THAN;
    if (j < n2)
        return K32N_CSTR_LESS_THAN;
    return tie ? tie : K32N_CSTR_EQUAL;
}

/* CompareStringOrdinal: code unit order after optional upper-casing. */
static inline int k32n_compare_ordinal(const WCHAR *s1, int n1, const WCHAR *s2, int n2, int ignore_case)
{
    int n = n1 < n2 ? n1 : n2;
    for (int i = 0; i < n; ++i) {
        WCHAR a = ignore_case ? k32n_upper(s1[i]) : s1[i], b = ignore_case ? k32n_upper(s2[i]) : s2[i];
        if (a != b)
            return a < b ? K32N_CSTR_LESS_THAN : K32N_CSTR_GREATER_THAN;
    }
    return n1 == n2 ? K32N_CSTR_EQUAL : (n1 < n2 ? K32N_CSTR_LESS_THAN : K32N_CSTR_GREATER_THAN);
}

/* The length of a counted string argument: cch, or up to the NUL when it is -1. */
static inline int k32n_len(const WCHAR *s, int cch)
{
    int n = 0;
    if (cch >= 0)
        return cch;
    while (s[n])
        ++n;
    return n;
}

/* ---- Case mapping -------------------------------------------------------------------------------------------------- */

/* LCMapString for the case flags (and a plain copy when only normalisation flags are given). Writes into dst when cap
 * allows; returns the characters produced, or the needed count when cap is 0. -1: the flags are not supported here. */
static inline int k32n_lcmap(DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap)
{
    DWORD op = flags & (K32N_LCMAP_LOWERCASE | K32N_LCMAP_UPPERCASE);
    if (flags & K32N_LCMAP_SORTKEY)
        return -1;
    if (op == (K32N_LCMAP_LOWERCASE | K32N_LCMAP_UPPERCASE))
        return -1;
    if (cap == 0)
        return n;
    if (cap < n)
        return -2; /* insufficient buffer */
    for (int i = 0; i < n; ++i)
        dst[i] = op == K32N_LCMAP_LOWERCASE ? k32n_lower(src[i]) : op == K32N_LCMAP_UPPERCASE ? k32n_upper(src[i]) : src[i];
    return n;
}

/* ---- Character types ------------------------------------------------------------------------------------------------ */

#define K32N_CT_CTYPE1 1u
#define K32N_CT_CTYPE2 2u
#define K32N_CT_CTYPE3 4u

static inline WORD k32n_ctype1(WCHAR c)
{
    WORD t = 0;
    ucrt_wchar w = (ucrt_wchar)c;
    if (c >= 0xD800 && c <= 0xDFFF)
        return 0; /* a surrogate half has no type of its own */
    if (c == 0xFFFE || c == 0xFFFF)
        return 0;
    if (ucrt_iswupper(w))
        t |= 0x0001;
    if (ucrt_iswlower(w))
        t |= 0x0002;
    if (c >= '0' && c <= '9')
        t |= 0x0004;
    if (ucrt_iswspace(w))
        t |= 0x0008;
    if (ucrt_iswpunct(w))
        t |= 0x0010;
    if (ucrt_iswcntrl(w))
        t |= 0x0020;
    if (ucrt_iswblank(w))
        t |= 0x0040;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
        t |= 0x0080;
    if (ucrt_iswalpha(w))
        t |= 0x0100;
    t |= 0x0200; /* C1_DEFINED */
    return t;
}

static inline WORD k32n_ctype2(WCHAR c)
{
    if (c >= '0' && c <= '9')
        return 3; /* C2_EUROPENUMBER */
    if (c == ' ' || c == '\t' || c == 0x3000)
        return 10; /* C2_WHITESPACE */
    if (c == '\n' || c == '\r' || c == 0x2029)
        return 8; /* C2_BLOCKSEPARATOR */
    if (c == '+' || c == '-')
        return 4; /* C2_EUROPESEPARATOR */
    if (c == '#' || c == '$' || c == '%' || c == 0xA2 || c == 0xA3 || c == 0xA5)
        return 5; /* C2_EUROPETERMINATOR */
    if (c == ',' || c == '.' || c == '/' || c == ':' || c == 0xA0)
        return 7; /* C2_COMMONSEPARATOR */
    if (ucrt_iswalpha((ucrt_wchar)c))
        return (c >= 0x0590 && c <= 0x08FF) ? 2 : 1; /* right to left for Hebrew and Arabic, otherwise left to right */
    return 11; /* C2_OTHERNEUTRAL */
}

static inline WORD k32n_ctype3(WCHAR c)
{
    WORD t = 0;
    if (ucrt_iswalpha((ucrt_wchar)c))
        t |= 0x8000; /* C3_ALPHA */
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF))
        t |= 0x0001 | 0x0002; /* C3_NONSPACING | C3_DIACRITIC */
    if ((c >= 0x3041 && c <= 0x3096))
        t |= 0x0020; /* C3_HIRAGANA */
    if ((c >= 0x30A1 && c <= 0x30FA))
        t |= 0x0010; /* C3_KATAKANA */
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF))
        t |= 0x0100; /* C3_IDEOGRAPH */
    if (c >= 0xFF01 && c <= 0xFF5E)
        t |= 0x0080; /* C3_FULLWIDTH */
    if (c >= 0xFF61 && c <= 0xFF9F)
        t |= 0x0040; /* C3_HALFWIDTH */
    if (!(t & 0x8000) && !(c >= '0' && c <= '9') && c > 0x20 && c < 0x7F)
        t |= 0x0008; /* C3_SYMBOL for ASCII punctuation and symbols */
    if (c == 0xA9 || c == 0xAE || c == 0xB0 || c == 0xB1 || (c >= 0x2190 && c <= 0x2BFF))
        t |= 0x0008;
    if (c > 0xFF || (c >= 0xC0 && !ucrt_iswspace((ucrt_wchar)c)))
        t |= 0x0400 * ucrt_iswalpha((ucrt_wchar)c); /* C3_LEXICAL for letters beyond ASCII */
    return t;
}

/* ---- Numbers ------------------------------------------------------------------------------------------------------ */

/* Formats a decimal number string "[-]digits[.digits]" with the US conventions: the fraction is rounded or padded to
 * `digits` places, the integer part grouped in threes with `thousand`, and a leading zero kept. Output is ASCII into out. Returns
 * the length, or -1 on a bad number or a short buffer. negative_fmt 1 is "-1.1" (the only form modelled). */
static inline int k32n_format_number(const char *num, int digits, int grouping, const char *decimal, const char *thousand, char *out, int cap)
{
    char ip[64], fp[64];
    int ni = 0, nf = 0, neg = 0, o = 0;
    const char *p = num;
    if (*p == '-') {
        neg = 1;
        ++p;
    }
    if (!(*p >= '0' && *p <= '9') && *p != '.')
        return -1;
    while (*p >= '0' && *p <= '9') {
        if (ni >= 60)
            return -1;
        ip[ni++] = *p++;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (nf >= 60)
                return -1;
            fp[nf++] = *p++;
        }
    }
    if (*p)
        return -1;
    /* round half up to the requested digits */
    if (digits >= 0 && nf > digits) {
        int up = fp[digits] >= '5';
        nf = digits;
        if (up) {
            int k = nf - 1;
            while (k >= 0) {
                if (fp[k] == '9')
                    fp[k--] = '0';
                else {
                    ++fp[k];
                    break;
                }
            }
            if (k < 0) { /* the carry goes into the integer part */
                int m = ni - 1;
                while (m >= 0) {
                    if (ip[m] == '9')
                        ip[m--] = '0';
                    else {
                        ++ip[m];
                        break;
                    }
                }
                if (m < 0) {
                    for (int q = ni; q > 0; --q)
                        ip[q] = ip[q - 1];
                    ip[0] = '1';
                    ++ni;
                }
            }
        }
    }
    while (digits >= 0 && nf < digits && nf < 60)
        fp[nf++] = '0';
    {
        int lead = 0;
        while (lead < ni - 1 && ip[lead] == '0')
            ++lead; /* drop leading zeros but keep one digit */
        if (ni == 0) {
            ip[0] = '0';
            ni = 1;
        }
        {
            int is_zero = 1;
            for (int q = 0; q < ni; ++q)
                if (ip[q] != '0')
                    is_zero = 0;
            for (int q = 0; q < nf; ++q)
                if (fp[q] != '0')
                    is_zero = 0;
            if (is_zero)
                neg = 0;
        }
        if (neg) {
            if (o + 1 >= cap)
                return -1;
            out[o++] = '-';
        }
        for (int q = lead; q < ni; ++q) {
            int remaining = ni - q;
            if (o + 2 >= cap)
                return -1;
            out[o++] = ip[q];
            if (grouping > 0 && remaining > 1 && (remaining - 1) % grouping == 0) {
                for (const char *t = thousand; *t; ++t) {
                    if (o + 2 >= cap)
                        return -1;
                    out[o++] = *t;
                }
            }
        }
    }
    if (nf > 0) {
        for (const char *t = decimal; *t; ++t) {
            if (o + 2 >= cap)
                return -1;
            out[o++] = *t;
        }
        for (int q = 0; q < nf; ++q) {
            if (o + 2 >= cap)
                return -1;
            out[o++] = fp[q];
        }
    }
    out[o] = 0;
    return o;
}

#endif /* NTDLL_K32_NLS_PURE_H */
