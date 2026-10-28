/*
 * ucrt_scanf_t.h - the scanf engine, written once for char and 16-bit wide formats.
 *
 * Included twice by ucrt_scanf.h with UCRT_CT (the format character type), UCRT_P (the name prefix) and
 * UCRT_WIDE (1 for the wide engine) set. The engine reads characters through a ucrt_sc_src, so strings and
 * streams share it. Characters read ahead and not used are pushed back in order, several if need be (the
 * string source can take any number, a stream as many as its buffer holds).
 *
 * Differences from ISO C, matching the Microsoft runtime: %c %s %[ with the "_s" functions take a buffer size;
 * a long double is a double; %p reads a hexadecimal number; in the wide engine %s is a wide string and %S a
 * narrow one, the other way round in the narrow engine; integer overflow wraps rather than clamping.
 */
#define UCRT_CAT_(a, b) a##b
#define UCRT_CAT(a, b) UCRT_CAT_(a, b)
#define UCRT_FN(name) UCRT_CAT(UCRT_P, name)

static inline int UCRT_FN(is_space)(int c)
{
#if UCRT_WIDE
    return c >= 0 && ucrt_iswspace((ucrt_wchar)c);
#else
    return c >= 0 && ucrt_isspace(c);
#endif
}

/* Is c in the scan set that starts at s (just after "[" and an optional "^") and ends at the closing bracket? */
static inline int UCRT_FN(in_scanset)(int c, const UCRT_CT *s, const UCRT_CT *end, int negate)
{
    int hit = 0;
    for (const UCRT_CT *p = s; p < end; ++p) {
        if (p + 2 < end && p[1] == '-') { /* a range a-z; a trailing or leading '-' is a literal */
            int lo = (int)(unsigned)p[0], hi = (int)(unsigned)p[2];
            if (c >= lo && c <= hi)
                hit = 1;
            p += 2;
        } else if ((int)(unsigned)*p == c) {
            hit = 1;
        }
    }
    return negate ? !hit : hit;
}

/* Finds the end of a scan set. p points just after the "[" (and "^"); the first "]" is a member. */
static inline const UCRT_CT *UCRT_FN(scanset_end)(const UCRT_CT *p)
{
    if (*p == ']')
        ++p;
    while (*p && *p != ']')
        ++p;
    return p;
}

/* Stores a character read as an int into a narrow or wide destination as the conversion asks. */
static inline int UCRT_FN(digit)(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Reads an integer into *out (as an unsigned 64-bit value with the sign applied by wrapping). Returns 1 for a
 * number, 0 for a matching failure, -1 for end of input before any character. */
static inline int UCRT_FN(scan_int)(ucrt_sc_src *src, int width, int base, uint64_t *out)
{
    int c, neg = 0, any = 0, w = width;
    uint64_t v = 0;
    c = ucrt_sc_get(src);
    if (c < 0)
        return -1;
    if (c == '+' || c == '-') {
        neg = c == '-';
        if (--w <= 0) {
            return 0;
        }
        c = ucrt_sc_get(src);
    }
    if ((base == 0 || base == 16) && c == '0' && w > 0) {
        any = 1; /* the zero is a digit in its own right */
        if (--w > 0) {
            int c2 = ucrt_sc_get(src);
            if (c2 == 'x' || c2 == 'X') {
                int c3;
                if (--w > 0 && (c3 = ucrt_sc_get(src)) >= 0 && UCRT_FN(digit)(c3) >= 0 && UCRT_FN(digit)(c3) < 16) {
                    base = 16;
                    c = c3;
                    any = 0; /* the digits follow */
                } else {
                    /* "0x" with no hex digit after it: the number is the 0, and the x is not part of it */
                    if (c3 >= 0 && w > 0)
                        ucrt_sc_unget(src, c3);
                    ucrt_sc_unget(src, c2);
                    *out = 0;
                    return 1;
                }
            } else {
                if (base == 0)
                    base = 8;
                c = c2;
                /* c is the character after the leading zero; fall through with any = 1 */
            }
        } else {
            *out = 0;
            return 1;
        }
    } else if (base == 0) {
        base = 10;
    }
    while (w > 0 || (w == 0 && 0)) {
        int d = c >= 0 ? UCRT_FN(digit)(c) : -1;
        if (d < 0 || d >= base)
            break;
        v = v * (uint64_t)base + (uint64_t)d;
        any = 1;
        if (--w == 0) {
            c = -2; /* width exhausted: nothing to push back */
            break;
        }
        c = ucrt_sc_get(src);
    }
    if (c >= 0)
        ucrt_sc_unget(src, c);
    if (!any)
        return 0;
    *out = neg ? (uint64_t)0 - v : v;
    return 1;
}

/* Gathers the text of a floating-point number (decimal, hexadecimal, inf, nan) into out. Returns its length, 0 when
 * there is none, -1 at end of input. */
static inline int UCRT_FN(scan_float_text)(ucrt_sc_src *src, int width, char *out, int cap)
{
    int c, n = 0, w = width;
#define UCRT_TAKE(ch) do { if (n + 1 < cap) out[n++] = (char)(ch); --w; } while (0)
    c = ucrt_sc_get(src);
    if (c < 0)
        return -1;
    if (c == '+' || c == '-') {
        UCRT_TAKE(c);
        if (w <= 0)
            goto fail;
        c = ucrt_sc_get(src);
    }
    if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
        /* inf, infinity, nan, nan(...): read letters while they continue the word */
        static const char inf[] = "infinity", nan[] = "nan";
        const char *word = (c == 'i' || c == 'I') ? inf : nan;
        int len = (c == 'i' || c == 'I') ? 8 : 3, k = 0, minlen = 3;
        int pushed[8], np = 0;
        for (;;) {
            int lc = c >= 'A' && c <= 'Z' ? c + 32 : c;
            if (k < len && lc == word[k] && w > 0) {
                pushed[np++] = c;
                ++k;
                --w;
                c = w > 0 ? ucrt_sc_get(src) : -2;
                continue;
            }
            break;
        }
        if (c >= 0)
            ucrt_sc_unget(src, c);
        if (k >= minlen && (word == nan || k == 3 || k == 8)) {
            for (int i = 0; i < k; ++i)
                out[n++] = (char)pushed[i];
            if (word == nan && c == '(' && w > 0) { /* nan(payload) */
                int depth_chars = 0;
                char tmp[64];
                int tn = 0, c2;
                tmp[tn++] = '(';
                (void)ucrt_sc_get(src);
                --w;
                while (w > 0 && (c2 = ucrt_sc_get(src)) >= 0) {
                    --w;
                    if (c2 == ')') {
                        tmp[tn++] = ')';
                        for (int i = 0; i < tn && n + 1 < cap; ++i)
                            out[n++] = tmp[i];
                        depth_chars = 1;
                        break;
                    }
                    if (tn < 60 && ((c2 >= '0' && c2 <= '9') || (c2 >= 'a' && c2 <= 'z') || (c2 >= 'A' && c2 <= 'Z') || c2 == '_'))
                        tmp[tn++] = (char)c2;
                    else {
                        ucrt_sc_unget(src, c2);
                        break;
                    }
                }
                (void)depth_chars;
            }
            out[n] = 0;
            return n;
        }
        /* "in" without an f: give the letters back, leaving a matching failure */
        for (int i = np - 1; i >= 0; --i)
            ucrt_sc_unget(src, pushed[i]);
        goto fail;
    }
    {
        int digits = 0, hex = 0;
        if (c == '0' && w > 0) {
            int c2;
            UCRT_TAKE('0');
            digits = 1;
            c = w > 0 ? ucrt_sc_get(src) : -2;
            if ((c == 'x' || c == 'X') && w > 0) {
                c2 = w > 1 ? ucrt_sc_get(src) : -2;
                if (c2 >= 0 && (UCRT_FN(digit)(c2) >= 0 || (c2 == '.'))) {
                    UCRT_TAKE(c);
                    hex = 1;
                    digits = 0;
                    c = c2;
                } else {
                    if (c2 >= 0)
                        ucrt_sc_unget(src, c2);
                    /* "0x" alone is the number 0 followed by x */
                }
            }
        }
        while (w > 0 && c >= 0 && (hex ? (UCRT_FN(digit)(c) >= 0) : (c >= '0' && c <= '9'))) {
            UCRT_TAKE(c);
            digits = 1;
            c = w > 0 ? ucrt_sc_get(src) : -2;
        }
        if (w > 0 && c == '.') {
            UCRT_TAKE(c);
            c = w > 0 ? ucrt_sc_get(src) : -2;
            while (w > 0 && c >= 0 && (hex ? (UCRT_FN(digit)(c) >= 0) : (c >= '0' && c <= '9'))) {
                UCRT_TAKE(c);
                digits = 1;
                c = w > 0 ? ucrt_sc_get(src) : -2;
            }
        }
        if (!digits) {
            if (c >= 0)
                ucrt_sc_unget(src, c);
            goto fail;
        }
        if (w > 0 && (hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E'))) {
            int e1 = c, sign = -1, d1 = -1;
            c = ucrt_sc_get(src);
            if (c == '+' || c == '-') {
                sign = c;
                c = w > 1 ? ucrt_sc_get(src) : -2;
            }
            d1 = c;
            if (w > (sign >= 0 ? 2 : 1) - 1 && d1 >= '0' && d1 <= '9' && w >= (sign >= 0 ? 3 : 2)) {
                UCRT_TAKE(e1);
                if (sign >= 0)
                    UCRT_TAKE(sign);
                while (w > 0 && c >= '0' && c <= '9') {
                    UCRT_TAKE(c);
                    c = w > 0 ? ucrt_sc_get(src) : -2;
                }
            } else {
                /* an exponent mark with no digits: not part of the number */
                if (c >= 0)
                    ucrt_sc_unget(src, c);
                if (sign >= 0)
                    ucrt_sc_unget(src, sign);
                ucrt_sc_unget(src, e1);
                c = -2;
            }
        }
        if (c >= 0)
            ucrt_sc_unget(src, c);
        out[n] = 0;
        return n;
    }
fail:
    if (n > 0) {
        /* a sign with nothing after it: give it back */
        out[n] = 0;
    }
    return 0;
#undef UCRT_TAKE
}

/* The scanf engine. secure: the _s functions, where %c, %s and %[ take a destination size. Returns the number of
 * fields assigned, or EOF (-1) if input ended before the first conversion completed. */
static inline int UCRT_FN(vscan)(ucrt_sc_src *src, const UCRT_CT *fmt, va_list ap, int secure)
{
    int assigned = 0, conversions = 0, eof_before_first = 0;
    const UCRT_CT *p = fmt;
    int failed_input = 0;
    while (*p) {
        int c;
        if (UCRT_FN(is_space)((int)(unsigned)*p)) {
            while (UCRT_FN(is_space)((int)(unsigned)*p))
                ++p;
            for (;;) {
                c = ucrt_sc_get(src);
                if (c < 0 || !UCRT_FN(is_space)(c)) {
                    if (c >= 0)
                        ucrt_sc_unget(src, c);
                    break;
                }
            }
            continue;
        }
        if (*p != '%') {
            c = ucrt_sc_get(src);
            if (c < 0) {
                failed_input = 1;
                break;
            }
            if (c != (int)(unsigned)*p) {
                ucrt_sc_unget(src, c);
                break;
            }
            ++p;
            continue;
        }
        {
            int suppress = 0, width = 0, size = UCRT_SZ_DEFAULT, conv;
            int has_width = 0;
            ++p;
            if (*p == '%') {
                do {
                    c = ucrt_sc_get(src);
                } while (c >= 0 && UCRT_FN(is_space)(c));
                if (c != '%') {
                    if (c < 0)
                        failed_input = 1;
                    else
                        ucrt_sc_unget(src, c);
                    break;
                }
                ++p;
                continue;
            }
            if (*p == '*') {
                suppress = 1;
                ++p;
            }
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                has_width = 1;
                ++p;
            }
            if (has_width && width == 0)
                break; /* %0d is invalid */
            switch (*p) {
            case 'h':
                ++p;
                if (*p == 'h') {
                    size = UCRT_SZ_HH;
                    ++p;
                } else {
                    size = UCRT_SZ_H;
                }
                break;
            case 'l':
                ++p;
                if (*p == 'l') {
                    size = UCRT_SZ_LL;
                    ++p;
                } else {
                    size = UCRT_SZ_L;
                }
                break;
            case 'L': size = UCRT_SZ_BIGL; ++p; break;
            case 'j': case 'z': case 't': size = UCRT_SZ_LL; ++p; break;
            case 'w': size = UCRT_SZ_W; ++p; break;
            case 'I':
                ++p;
                if (p[0] == '3' && p[1] == '2') {
                    size = UCRT_SZ_I32;
                    p += 2;
                } else if (p[0] == '6' && p[1] == '4') {
                    size = UCRT_SZ_LL;
                    p += 2;
                } else {
                    size = UCRT_SZ_LL; /* I alone is a pointer-sized integer */
                }
                break;
            default: break;
            }
            conv = (int)(unsigned)*p;
            if (!conv)
                break;
            ++p;
            if (!width)
                width = 0x7FFFFFFF;
            /* conversions other than %c, %[ and %n skip leading white space */
            if (conv != 'c' && conv != 'C' && conv != '[' && conv != 'n') {
                do {
                    c = ucrt_sc_get(src);
                } while (c >= 0 && UCRT_FN(is_space)(c));
                if (c < 0) {
                    failed_input = 1;
                    break;
                }
                ucrt_sc_unget(src, c);
            }
            switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                uint64_t v = 0;
                int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
                int r = UCRT_FN(scan_int)(src, width, base, &v);
                if (r < 0) {
                    failed_input = 1;
                    goto done;
                }
                if (r == 0)
                    goto done;
                if (!suppress) {
                    if (conv == 'p') {
                        *va_arg(ap, void **) = (void *)(uintptr_t)v;
                    } else {
                        switch (size) {
                        case UCRT_SZ_HH: *va_arg(ap, unsigned char *) = (unsigned char)v; break;
                        case UCRT_SZ_H: *va_arg(ap, unsigned short *) = (unsigned short)v; break;
                        case UCRT_SZ_LL: *va_arg(ap, uint64_t *) = v; break;
                        default: *va_arg(ap, unsigned int *) = (unsigned int)v; break;
                        }
                    }
                    ++assigned;
                }
                ++conversions;
                break;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                char tok[1200];
                int len = UCRT_FN(scan_float_text)(src, width, tok, (int)sizeof(tok));
                if (len < 0) {
                    failed_input = 1;
                    goto done;
                }
                if (len == 0)
                    goto done;
                if (!suppress) {
                    if (size == UCRT_SZ_L || size == UCRT_SZ_BIGL)
                        *va_arg(ap, double *) = ucrt_strtod(tok, NULL);
                    else
                        *va_arg(ap, float *) = ucrt_strtof(tok, NULL);
                    ++assigned;
                }
                ++conversions;
                break;
            }
            case 'n': {
                long long n = src->consumed;
                if (!suppress) {
                    switch (size) {
                    case UCRT_SZ_HH: *va_arg(ap, signed char *) = (signed char)n; break;
                    case UCRT_SZ_H: *va_arg(ap, short *) = (short)n; break;
                    case UCRT_SZ_LL: *va_arg(ap, long long *) = n; break;
                    default: *va_arg(ap, int *) = (int)n; break;
                    }
                }
                break;
            }
            case 'c': case 'C': case 's': case 'S': case '[': {
                /* which character type is stored: wide when the conversion and size say so */
                int store_wide;
                const UCRT_CT *set = NULL, *set_end = NULL;
                int negate = 0, is_char = (conv == 'c' || conv == 'C'), count = 0;
                void *dst = NULL;
                unsigned dst_size = 0x7FFFFFFF;
                if (conv == '[') {
                    if (*p == '^') {
                        negate = 1;
                        ++p;
                    }
                    set = p;
                    set_end = UCRT_FN(scanset_end)(p);
                    if (*set_end != ']')
                        goto done;
                    p = set_end + 1;
                }
#if UCRT_WIDE
                store_wide = !(conv == 'C' || conv == 'S' || size == UCRT_SZ_H);
                if (size == UCRT_SZ_L || size == UCRT_SZ_W)
                    store_wide = 1;
#else
                store_wide = (conv == 'C' || conv == 'S' || size == UCRT_SZ_L || size == UCRT_SZ_W);
                if (size == UCRT_SZ_H)
                    store_wide = 0;
#endif
                if (!suppress) {
                    dst = va_arg(ap, void *);
                    if (secure)
                        dst_size = va_arg(ap, unsigned);
                }
                if (is_char && !has_width)
                    width = 1;
                for (;;) {
                    c = ucrt_sc_get(src);
                    if (c < 0) {
                        if (count == 0)
                            failed_input = 1;
                        break;
                    }
                    if (!is_char && (conv == 's' || conv == 'S') && UCRT_FN(is_space)(c)) {
                        ucrt_sc_unget(src, c);
                        break;
                    }
                    if (conv == '[' && !UCRT_FN(in_scanset)(c, set, set_end, negate)) {
                        ucrt_sc_unget(src, c);
                        break;
                    }
                    if (conv == 'S' || conv == 's') {
                        /* %s reads non-space characters (white space was skipped above) */
                    }
                    if (!suppress) {
                        unsigned room = dst_size - (unsigned)count;
                        if (secure && room <= (is_char ? 0u : 1u)) {
                            ucrt_set_errno(UCRT_ERANGE);
                            if (!is_char) {
                                if (store_wide)
                                    ((ucrt_wchar *)dst)[0] = 0;
                                else
                                    ((char *)dst)[0] = 0;
                            }
                            goto done;
                        }
                        if (store_wide)
                            ((ucrt_wchar *)dst)[count] = (ucrt_wchar)c;
                        else
                            ((char *)dst)[count] = (char)c;
                    }
                    ++count;
                    if (count >= width)
                        break;
                }
                if (count == 0) {
                    if (failed_input)
                        goto done;
                    goto done;
                }
                if (!is_char && !suppress) {
                    if (store_wide)
                        ((ucrt_wchar *)dst)[count] = 0;
                    else
                        ((char *)dst)[count] = 0;
                }
                if (!suppress)
                    ++assigned;
                ++conversions;
                break;
            }
            default:
                goto done; /* an unknown conversion ends the scan */
            }
        }
    }
done:
    ucrt_sc_finish(src);
    eof_before_first = failed_input && assigned == 0;
    (void)conversions;
    return eof_before_first ? UCRT_EOF : assigned;
}

#undef UCRT_FN
#undef UCRT_CAT
#undef UCRT_CAT_
