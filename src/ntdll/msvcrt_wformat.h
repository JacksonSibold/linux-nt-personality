/*
 * msvcrt_wformat.h - the wide formatter behind _snwprintf (msvcrt.c).
 *
 * Header-only so tests/ntdll-host can run it on the host (test_msvcrt_policy.c).
 * Conversions: %d %i %u %x %X %c %s %S %% . Length modifiers are parsed and
 * change the argument size: h (narrow %s and %c), l and none (wide), ll, I64,
 * z, j, t, q and I (64-bit). %ls is the wide string, %hs the narrow one, %S the
 * narrow string (wide printf convention). Flags '-' and '0', a field width and
 * a string precision are supported.
 *
 * Before this module, the length modifier was not parsed, so L"%ls" was copied
 * out as the literal text "%ls". The path probes build their NT names that way
 * (tests/differential/probes/probe_path.c build_prefixes), so every such path
 * carried the text instead of the root directory.
 */
#ifndef NTDLL_MSVCRT_WFORMAT_H
#define NTDLL_MSVCRT_WFORMAT_H

#include "nt_types.h"
#include <stdarg.h>
#include <stddef.h>

static inline void ms_wput(WCHAR *out, SIZE_T cap, SIZE_T *pos, int *trunc, WCHAR c)
{
    if (*pos + 1 < cap) out[*pos] = c;
    else *trunc = 1;
    ++*pos;
}

/* Formats fmt into out[cap]. Returns the number of characters written, or -1
 * when the output and its NUL do not fit (out then holds the cut text). */
static int ms_wformat(WCHAR *out, SIZE_T cap, const WCHAR *fmt, va_list ap)
{
    SIZE_T pos = 0;
    int trunc = 0;

    for (SIZE_T i = 0; fmt[i]; ++i) {
        if (fmt[i] != '%') { ms_wput(out, cap, &pos, &trunc, fmt[i]); continue; }
        ++i;

        int left = 0, zero = 0, width = 0, prec = -1;
        int half = 0, big = 0;  /* h: narrow; big: 64-bit integer argument */

        while (fmt[i] == '-' || fmt[i] == '0') {
            if (fmt[i] == '-') left = 1; else zero = 1;
            ++i;
        }
        while (fmt[i] >= '0' && fmt[i] <= '9') { width = width * 10 + (fmt[i] - '0'); ++i; }
        if (fmt[i] == '.') {
            ++i; prec = 0;
            while (fmt[i] >= '0' && fmt[i] <= '9') { prec = prec * 10 + (fmt[i] - '0'); ++i; }
        }

        /* Length modifiers, in any order MSVC accepts. */
        for (;;) {
            WCHAR m = fmt[i];
            if (m == 'h') { half = 1; ++i; }
            else if (m == 'l') { if (fmt[i + 1] == 'l') { big = 1; i += 2; } else ++i; }
            else if (m == 'L' || m == 'q' || m == 'j' || m == 'z' || m == 'Z' || m == 't') { big = 1; ++i; }
            else if (m == 'w') { ++i; }
            else if (m == 'I') {
                if (fmt[i + 1] == '6' && fmt[i + 2] == '4') { big = 1; i += 3; }
                else if (fmt[i + 1] == '3' && fmt[i + 2] == '2') { i += 3; }
                else { big = 1; ++i; }  /* plain I: pointer size, 64-bit on x64 */
            }
            else break;
        }

        if (fmt[i] == 0) break;

        WCHAR wbuf[256];
        SIZE_T wn = 0;

        switch (fmt[i]) {
        case '%':
            ms_wput(out, cap, &pos, &trunc, '%');
            continue;
        case 'c':
            wbuf[wn++] = (WCHAR)va_arg(ap, int);
            break;
        case 's': {
            if (half) {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (s[wn] && (prec < 0 || (int)wn < prec) && wn < 255) { wbuf[wn] = (WCHAR)(unsigned char)s[wn]; ++wn; }
            } else {
                const WCHAR *s = va_arg(ap, const WCHAR *);
                static const WCHAR nullw[] = { 0x28, 0x6E, 0x75, 0x6C, 0x6C, 0x29, 0 };
                if (!s) s = nullw;
                while (s[wn] && (prec < 0 || (int)wn < prec) && wn < 255) { wbuf[wn] = s[wn]; ++wn; }
            }
            break;
        }
        case 'S': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (s[wn] && (prec < 0 || (int)wn < prec) && wn < 255) { wbuf[wn] = (WCHAR)(unsigned char)s[wn]; ++wn; }
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            unsigned long long v;
            int neg = 0;
            if (fmt[i] == 'd' || fmt[i] == 'i') {
                long long sv = big ? va_arg(ap, long long) : (long long)va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (unsigned long long)(-(sv + 1)) + 1u; }
                else v = (unsigned long long)sv;
            } else {
                v = big ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            }
            unsigned base = (fmt[i] == 'x' || fmt[i] == 'X') ? 16u : 10u;
            const char *digits = (fmt[i] == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[32];
            SIZE_T k = sizeof(tmp);
            do { tmp[--k] = digits[v % base]; v /= base; } while (v && k > 0);
            if (neg && k > 0) tmp[--k] = '-';
            for (; k < sizeof(tmp); ++k) wbuf[wn++] = (WCHAR)tmp[k];
            break;
        }
        default:
            ms_wput(out, cap, &pos, &trunc, '%');
            ms_wput(out, cap, &pos, &trunc, fmt[i]);
            continue;
        }

        SIZE_T padn = ((SIZE_T)width > wn) ? (SIZE_T)width - wn : 0;
        if (!left) for (SIZE_T j = 0; j < padn; ++j) ms_wput(out, cap, &pos, &trunc, zero ? '0' : ' ');
        for (SIZE_T j = 0; j < wn; ++j) ms_wput(out, cap, &pos, &trunc, wbuf[j]);
        if (left) for (SIZE_T j = 0; j < padn; ++j) ms_wput(out, cap, &pos, &trunc, ' ');
    }

    if (cap > 0) out[pos < cap ? pos : cap - 1] = 0;
    if (trunc || cap == 0) return -1;
    return (int)pos;
}

#endif /* NTDLL_MSVCRT_WFORMAT_H */
