/*
 * ucrt_printf_t.h - the printf formatting engine, written once for char and for 16-bit wide output.
 *
 * Included by ucrt_printf.h with UCRT_CT (the output and format character type), UCRT_P (the name prefix),
 * UCRT_WIDE (1 for the wide engine, whose %s takes a wide string) and UCRT_PUT(sk, ptr, n) set.
 *
 * Behaviour follows the Microsoft runtime: sizes h, hh, l (32 bits), ll, I (pointer size), I32, I64, j, z, t,
 * L (a double) and w; %p prints sixteen upper case hex digits; %s of NULL prints (null); %Z and %wZ print a
 * counted string; %n is refused unless enabled; the exponent has at least two digits (three for the old
 * msvcrt, g_ucrt_state.printf_exp_digits); infinity and NaN print as inf, nan, nan(ind) and nan(snan).
 */
#define UCRT_CAT_(a, b) a##b
#define UCRT_CAT(a, b) UCRT_CAT_(a, b)
#define UCRT_FN(name) UCRT_CAT(UCRT_P, name)

/* Writes n ASCII characters from a char buffer. */
static inline void UCRT_FN(emit_ascii)(ucrt_pf_sink *sk, const char *s, size_t n)
{
    UCRT_CT tmp[64];
    while (n) {
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; ++i)
            tmp[i] = (UCRT_CT)(unsigned char)s[i];
        UCRT_PUT(sk, tmp, k);
        sk->count += (long long)k;
        s += k;
        n -= k;
    }
}

static inline void UCRT_FN(emit_chars)(ucrt_pf_sink *sk, const UCRT_CT *s, size_t n)
{
    if (n) {
        UCRT_PUT(sk, s, n);
        sk->count += (long long)n;
    }
}

static inline void UCRT_FN(emit_fill)(ucrt_pf_sink *sk, char c, size_t n)
{
    char tmp[64];
    for (size_t i = 0; i < 64; ++i)
        tmp[i] = c;
    while (n) {
        size_t k = n < 64 ? n : 64;
        UCRT_FN(emit_ascii)(sk, tmp, k);
        n -= k;
    }
}

/* A number: prefix (sign, 0x), zeros, then the body, with the padding that width and flags ask for. The body
 * is ASCII. `zero_pad` says the zero flag applies (it does not when a precision is given for integers). */
static inline void UCRT_FN(emit_number)(ucrt_pf_sink *sk, const char *prefix, size_t prefix_len, size_t zeros,
                                        const char *body, size_t body_len, int width, int minus, int zero_pad)
{
    size_t total = prefix_len + zeros + body_len;
    size_t pad = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;
    if (pad && !minus && !zero_pad)
        UCRT_FN(emit_fill)(sk, ' ', pad);
    UCRT_FN(emit_ascii)(sk, prefix, prefix_len);
    if (pad && !minus && zero_pad)
        UCRT_FN(emit_fill)(sk, '0', pad);
    UCRT_FN(emit_fill)(sk, '0', zeros);
    UCRT_FN(emit_ascii)(sk, body, body_len);
    if (pad && minus)
        UCRT_FN(emit_fill)(sk, ' ', pad);
}

/* Text of a floating point value (without padding): sign handling is the caller's. Writes the digits and
 * punctuation of the conversion into out (at least UCRT_PF_FLOAT_BUF bytes) and returns the length. */
static inline size_t UCRT_FN(float_text)(char *out, double av, char conv, int prec, int alt, int upper, int exp_min)
{
    char dig[UCRT_DTOA_MAX_DIGITS + 8];
    int decpt = 0, n, i;
    char *p = out;
    int is_zero = (av == 0.0);
    int lower_conv = conv | 0x20;
    if (prec > UCRT_DTOA_MAX_DIGITS - 1)
        prec = UCRT_DTOA_MAX_DIGITS - 1;
    if (lower_conv == 'g') {
        int P = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
        int X;
        n = ucrt_dtoa(av, 2, P, dig, &decpt);
        X = is_zero ? 0 : decpt - 1;
        if (P > X && X >= -4) { /* fixed, with P-1-X fraction digits taken from the P significant digits */
            char *dot;
            if (X >= 0) {
                for (i = 0; i <= X; ++i)
                    *p++ = i < n ? dig[i] : '0';
            } else {
                *p++ = '0';
            }
            if (P - 1 - X > 0 || alt) {
                dot = p;
                *p++ = '.';
                if (X < 0) {
                    for (i = 0; i < -X - 1; ++i)
                        *p++ = '0';
                    for (i = 0; i < P; ++i)
                        *p++ = i < n ? dig[i] : '0';
                } else {
                    for (i = X + 1; i < P; ++i)
                        *p++ = i < n ? dig[i] : '0';
                }
                if (!alt) { /* trailing zeros and a bare point go */
                    while (p > dot + 1 && p[-1] == '0')
                        --p;
                    if (p == dot + 1)
                        --p;
                }
            }
        } else { /* exponent form with P-1 fraction digits */
            int e = X;
            *p++ = n > 0 ? dig[0] : '0';
            if (P > 1 || alt) {
                char *dot = p;
                *p++ = '.';
                for (i = 1; i < P; ++i)
                    *p++ = i < n ? dig[i] : '0';
                if (!alt) {
                    while (p > dot + 1 && p[-1] == '0')
                        --p;
                    if (p == dot + 1)
                        --p;
                }
            }
            *p++ = upper ? 'E' : 'e';
            *p++ = e < 0 ? '-' : '+';
            {
                int a = e < 0 ? -e : e, digs = 0;
                char tmp[8];
                do {
                    tmp[digs++] = (char)('0' + a % 10);
                    a /= 10;
                } while (a);
                for (i = digs; i < exp_min; ++i)
                    *p++ = '0';
                while (digs)
                    *p++ = tmp[--digs];
            }
        }
        return (size_t)(p - out);
    }
    if (lower_conv == 'f') {
        int pr = prec < 0 ? 6 : prec;
        n = ucrt_dtoa(av, 3, pr, dig, &decpt);
        if (decpt <= 0) {
            *p++ = '0';
        } else {
            for (i = 0; i < decpt; ++i)
                *p++ = i < n ? dig[i] : '0';
        }
        if (pr > 0 || alt) {
            *p++ = '.';
            for (i = 0; i < pr; ++i) {
                int idx = decpt + i;
                *p++ = (idx >= 0 && idx < n) ? dig[idx] : '0';
            }
        }
        return (size_t)(p - out);
    }
    /* 'e' */
    {
        int pr = prec < 0 ? 6 : prec;
        int e;
        n = ucrt_dtoa(av, 2, pr + 1, dig, &decpt);
        e = is_zero ? 0 : decpt - 1;
        *p++ = n > 0 ? dig[0] : '0';
        if (pr > 0 || alt) {
            *p++ = '.';
            for (i = 1; i <= pr; ++i)
                *p++ = i < n ? dig[i] : '0';
        }
        *p++ = upper ? 'E' : 'e';
        *p++ = e < 0 ? '-' : '+';
        {
            int a = e < 0 ? -e : e, digs = 0;
            char tmp[8];
            do {
                tmp[digs++] = (char)('0' + a % 10);
                a /= 10;
            } while (a);
            for (i = digs; i < exp_min; ++i)
                *p++ = '0';
            while (digs)
                *p++ = tmp[--digs];
        }
        return (size_t)(p - out);
    }
}

/* %a: 0x1.hhhhp+e. prec < 0 means all thirteen hex digits of a double. */
static inline size_t UCRT_FN(hexfloat_text)(char *out, double av, int prec, int alt, int upper)
{
    uint64_t bits, frac;
    int be, lead, e, i;
    char *p = out;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    __builtin_memcpy(&bits, &av, sizeof(bits));
    bits &= 0x7FFFFFFFFFFFFFFFull;
    be = (int)(bits >> 52);
    frac = bits & 0xFFFFFFFFFFFFFull;
    if (be == 0) {
        lead = 0;
        e = frac ? -1022 : 0;
    } else {
        lead = 1;
        e = be - 1023;
    }
    if (prec < 0)
        prec = 13;
    if (prec < 13) { /* round the fraction to prec hex digits, half to even */
        int drop = 4 * (13 - prec);
        uint64_t rem = frac & (((uint64_t)1 << drop) - 1), half = (uint64_t)1 << (drop - 1), kept = frac >> drop;
        int last_odd = (prec == 0) ? (lead & 1) : (int)(kept & 1);
        int up = rem > half || (rem == half && last_odd);
        if (prec == 0) {
            lead += up;
            frac = 0;
        } else {
            kept += (uint64_t)up;
            if (kept >> (4 * prec)) { /* carry out of the fraction into the leading digit */
                kept &= ((uint64_t)1 << (4 * prec)) - 1;
                ++lead;
            }
            frac = kept;
        }
    }
    *p++ = '0';
    *p++ = upper ? 'X' : 'x';
    *p++ = (char)('0' + lead);
    if (prec > 0 || alt) {
        *p++ = '.';
        for (i = 0; i < prec; ++i) {
            unsigned d = 0;
            if (prec <= 13)
                d = (unsigned)((frac >> (4 * (prec - 1 - i))) & 0xF);
            else if (i < 13)
                d = (unsigned)((frac >> (4 * (12 - i))) & 0xF);
            *p++ = digs[d];
        }
    }
    *p++ = upper ? 'P' : 'p';
    *p++ = e < 0 ? '-' : '+';
    {
        int a = e < 0 ? -e : e, nd = 0;
        char tmp[8];
        do {
            tmp[nd++] = (char)('0' + a % 10);
            a /= 10;
        } while (a);
        while (nd)
            *p++ = tmp[--nd];
    }
    return (size_t)(p - out);
}

/* Formats fmt into the sink. Returns the number of characters, or -1 for a bad format or an output error. */
static inline int UCRT_FN(vformat)(ucrt_pf_sink *sk, const UCRT_CT *fmt, va_list ap)
{
    sk->count = 0;
    for (;;) {
        const UCRT_CT *lit = fmt;
        unsigned flags = 0;
        int width = -1, prec = -1, size = UCRT_SZ_DEFAULT, is64 = 0;
        UCRT_CT c;
        while (*fmt && *fmt != '%')
            ++fmt;
        UCRT_FN(emit_chars)(sk, lit, (size_t)(fmt - lit));
        if (!*fmt)
            break;
        ++fmt;
        if (*fmt == '%') {
            UCRT_FN(emit_ascii)(sk, "%", 1);
            ++fmt;
            continue;
        }
        for (;; ++fmt) {
            if (*fmt == '-')
                flags |= UCRT_PF_LEFT;
            else if (*fmt == '+')
                flags |= UCRT_PF_PLUS;
            else if (*fmt == ' ')
                flags |= UCRT_PF_SPACE;
            else if (*fmt == '#')
                flags |= UCRT_PF_ALT;
            else if (*fmt == '0')
                flags |= UCRT_PF_ZERO;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= UCRT_PF_LEFT;
                width = -width;
            }
            ++fmt;
        } else if (*fmt >= '0' && *fmt <= '9') {
            width = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                if (width < 100000000)
                    width = width * 10 + (*fmt - '0');
                ++fmt;
            }
        }
        if (*fmt == '.') {
            ++fmt;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
                ++fmt;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    if (prec < 100000000)
                        prec = prec * 10 + (*fmt - '0');
                    ++fmt;
                }
            }
        }
        /* size prefixes */
        for (;;) {
            if (*fmt == 'h') {
                size = (size == UCRT_SZ_H) ? UCRT_SZ_HH : UCRT_SZ_H;
                ++fmt;
            } else if (*fmt == 'l') {
                if (size == UCRT_SZ_L) {
                    size = UCRT_SZ_LL;
                    is64 = 1;
                } else {
                    size = UCRT_SZ_L;
                }
                ++fmt;
            } else if (*fmt == 'L') {
                size = UCRT_SZ_BIGL; /* long double is a double on Windows */
                ++fmt;
            } else if (*fmt == 'j' || *fmt == 'z' || *fmt == 't') {
                size = UCRT_SZ_LL;
                is64 = 1;
                ++fmt;
            } else if (*fmt == 'w') {
                size = UCRT_SZ_W;
                ++fmt;
            } else if (*fmt == 'I') {
                if (fmt[1] == '6' && fmt[2] == '4') {
                    size = UCRT_SZ_LL;
                    is64 = 1;
                    fmt += 3;
                } else if (fmt[1] == '3' && fmt[2] == '2') {
                    size = UCRT_SZ_I32;
                    fmt += 3;
                } else {
                    size = UCRT_SZ_LL; /* I alone is a pointer-sized integer */
                    is64 = 1;
                    ++fmt;
                }
            } else {
                break;
            }
        }
        c = *fmt;
        if (!c)
            return -1; /* a format that ends inside a conversion */
        ++fmt;
        switch (c) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            int is_signed = (c == 'd' || c == 'i');
            uint64_t mag;
            int neg = 0;
            char buf[72], prefix[3];
            size_t blen = 0, plen = 0, zeros = 0;
            unsigned base = (c == 'o') ? 8 : (c == 'x' || c == 'X') ? 16 : 10;
            const char *digs = (c == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            if (is_signed) {
                int64_t v;
                if (is64 || size == UCRT_SZ_LL)
                    v = va_arg(ap, long long);
                else if (size == UCRT_SZ_HH)
                    v = (signed char)va_arg(ap, int);
                else if (size == UCRT_SZ_H)
                    v = (short)va_arg(ap, int);
                else
                    v = (int32_t)va_arg(ap, int);
                neg = v < 0;
                mag = neg ? 0 - (uint64_t)v : (uint64_t)v;
            } else {
                if (is64 || size == UCRT_SZ_LL)
                    mag = va_arg(ap, unsigned long long);
                else if (size == UCRT_SZ_HH)
                    mag = (unsigned char)va_arg(ap, unsigned);
                else if (size == UCRT_SZ_H)
                    mag = (unsigned short)va_arg(ap, unsigned);
                else
                    mag = (uint32_t)va_arg(ap, unsigned);
            }
            {
                char rev[72];
                size_t n = 0;
                uint64_t m = mag;
                if (!(m == 0 && prec == 0)) {
                    do {
                        rev[n++] = digs[m % base];
                        m /= base;
                    } while (m);
                }
                for (size_t i = n; i-- > 0;)
                    buf[blen++] = rev[i];
            }
            if (prec > 0 && (size_t)prec > blen)
                zeros = (size_t)prec - blen;
            if (is_signed) {
                if (neg)
                    prefix[plen++] = '-';
                else if (flags & UCRT_PF_PLUS)
                    prefix[plen++] = '+';
                else if (flags & UCRT_PF_SPACE)
                    prefix[plen++] = ' ';
            } else if ((flags & UCRT_PF_ALT) && (c == 'x' || c == 'X') && mag != 0) {
                prefix[plen++] = '0';
                prefix[plen++] = (char)c;
            }
            if (c == 'o' && (flags & UCRT_PF_ALT) && zeros == 0 && (blen == 0 || buf[0] != '0'))
                zeros = 1; /* an octal number starts with a zero */
            UCRT_FN(emit_number)(sk, prefix, plen, zeros, buf, blen, width, (flags & UCRT_PF_LEFT) != 0,
                                 (flags & UCRT_PF_ZERO) && prec < 0);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            char buf[20];
            size_t n = 0;
            for (int sh = 60; sh >= 0; sh -= 4)
                buf[n++] = "0123456789ABCDEF"[(v >> sh) & 0xF];
            UCRT_FN(emit_number)(sk, "", 0, 0, buf, n, width, (flags & UCRT_PF_LEFT) != 0, (flags & UCRT_PF_ZERO) != 0);
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            double v = va_arg(ap, double);
            uint64_t bits;
            char text[UCRT_DTOA_MAX_DIGITS + 360];
            char sign[2];
            size_t slen = 0, tlen;
            int upper = (c == 'E' || c == 'F' || c == 'G' || c == 'A');
            int special = 0;
            __builtin_memcpy(&bits, &v, sizeof(bits));
            if (bits >> 63)
                sign[slen++] = '-';
            else if (flags & UCRT_PF_PLUS)
                sign[slen++] = '+';
            else if (flags & UCRT_PF_SPACE)
                sign[slen++] = ' ';
            if (((bits >> 52) & 0x7FF) == 0x7FF) {
                uint64_t fr = bits & 0xFFFFFFFFFFFFFull;
                special = 1;
                if (fr == 0) {
                    tlen = 3;
                    __builtin_memcpy(text, upper ? "INF" : "inf", 3);
                } else if (!(fr & 0x8000000000000ull)) { /* quiet bit clear: signalling */
                    tlen = 9;
                    __builtin_memcpy(text, upper ? "NAN(SNAN)" : "nan(snan)", 9);
                } else if ((bits >> 63) && fr == 0x8000000000000ull) { /* the x86 indefinite NaN */
                    tlen = 8;
                    __builtin_memcpy(text, upper ? "NAN(IND)" : "nan(ind)", 8);
                } else {
                    tlen = 3;
                    __builtin_memcpy(text, upper ? "NAN" : "nan", 3);
                }
            } else if (c == 'a' || c == 'A') {
                tlen = UCRT_FN(hexfloat_text)(text, v < 0 ? -v : v, prec, (flags & UCRT_PF_ALT) != 0, upper);
            } else {
                tlen = UCRT_FN(float_text)(text, v < 0 ? -v : v, (char)c, prec, (flags & UCRT_PF_ALT) != 0, upper,
                                           g_ucrt_state.printf_exp_digits);
            }
            if ((c == 'a' || c == 'A') && !special && (flags & UCRT_PF_ZERO) && !(flags & UCRT_PF_LEFT)) {
                /* zero padding goes after the 0x prefix */
                size_t total = slen + tlen;
                size_t pad = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;
                UCRT_FN(emit_ascii)(sk, sign, slen);
                UCRT_FN(emit_ascii)(sk, text, 2);
                UCRT_FN(emit_fill)(sk, '0', pad);
                UCRT_FN(emit_ascii)(sk, text + 2, tlen - 2);
            } else {
                UCRT_FN(emit_number)(sk, sign, slen, 0, text, tlen, width, (flags & UCRT_PF_LEFT) != 0,
                                     (flags & UCRT_PF_ZERO) && !special);
            }
            break;
        }
        case 'c':
        case 'C':
        case 's':
        case 'S':
        case 'Z': {
            /* Which width of string or character does this conversion take? */
            int arg_wide;
            int is_char = (c == 'c' || c == 'C');
            if (c == 'Z') {
                arg_wide = (size == UCRT_SZ_W);
            } else if (size == UCRT_SZ_L || size == UCRT_SZ_W) {
                arg_wide = 1;
            } else if (size == UCRT_SZ_H) {
                arg_wide = 0;
            } else if (UCRT_WIDE) {
                arg_wide = (c == 's' || c == 'c');
            } else {
                arg_wide = (c == 'S' || c == 'C');
            }
            if (is_char) {
                unsigned v = (unsigned)va_arg(ap, int);
                UCRT_CT out1[1];
                size_t pad = (width > 1) ? (size_t)width - 1 : 0;
                if (arg_wide)
                    out1[0] = UCRT_WIDE ? (UCRT_CT)(ucrt_wchar)v : (UCRT_CT)((ucrt_wchar)v < 0x80 ? (ucrt_wchar)v : '?');
                else
                    out1[0] = UCRT_WIDE ? (UCRT_CT)((unsigned char)v < 0x80 ? (unsigned char)v : '?') : (UCRT_CT)(unsigned char)v;
                if (pad && !(flags & UCRT_PF_LEFT))
                    UCRT_FN(emit_fill)(sk, (flags & UCRT_PF_ZERO) ? '0' : ' ', pad);
                UCRT_FN(emit_chars)(sk, out1, 1);
                if (pad && (flags & UCRT_PF_LEFT))
                    UCRT_FN(emit_fill)(sk, ' ', pad);
                break;
            } else {
                const void *sp;
                size_t len = 0, i, pad;
                const char *ns = NULL;
                const ucrt_wchar *ws = NULL;
                int null_str = 0;
                if (c == 'Z') {
                    const uint8_t *cs = (const uint8_t *)va_arg(ap, void *);
                    uint16_t clen = 0;
                    const void *buf = NULL;
                    if (cs) {
                        clen = (uint16_t)(cs[0] | (cs[1] << 8));
                        __builtin_memcpy(&buf, cs + 8, sizeof(buf));
                    }
                    if (!cs || !buf || !clen) {
                        null_str = 1;
                    } else if (arg_wide) {
                        ws = (const ucrt_wchar *)buf;
                        len = clen / 2u;
                    } else {
                        ns = (const char *)buf;
                        len = clen;
                    }
                    sp = NULL;
                } else {
                    sp = va_arg(ap, const void *);
                    if (!sp) {
                        null_str = 1;
                    } else if (arg_wide) {
                        ws = (const ucrt_wchar *)sp;
                        while ((prec < 0 || (int)len < prec) && ws[len])
                            ++len;
                    } else {
                        ns = (const char *)sp;
                        while ((prec < 0 || (int)len < prec) && ns[len])
                            ++len;
                    }
                }
                if (null_str) {
                    ns = "(null)";
                    ws = NULL;
                    len = 6;
                    if (prec >= 0 && (size_t)prec < len && c != 'Z')
                        len = (size_t)prec;
                    arg_wide = 0;
                }
                if (c == 'Z' && prec >= 0 && (size_t)prec < len)
                    len = (size_t)prec;
                pad = (width > 0 && (size_t)width > len) ? (size_t)width - len : 0;
                if (pad && !(flags & UCRT_PF_LEFT))
                    UCRT_FN(emit_fill)(sk, (flags & UCRT_PF_ZERO) ? '0' : ' ', pad);
                if (!arg_wide) {
                    for (i = 0; i < len;) {
                        UCRT_CT tmp[64];
                        size_t k = len - i < 64 ? len - i : 64;
                        for (size_t j = 0; j < k; ++j) {
                            unsigned char b = (unsigned char)ns[i + j];
                            tmp[j] = (UCRT_CT)((UCRT_WIDE && b >= 0x80) ? '?' : b);
                        }
                        UCRT_FN(emit_chars)(sk, tmp, k);
                        i += k;
                    }
                } else if (UCRT_WIDE) {
                    UCRT_FN(emit_chars)(sk, (const UCRT_CT *)ws, len);
                } else {
                    for (i = 0; i < len;) {
                        UCRT_CT tmp[64];
                        size_t k = len - i < 64 ? len - i : 64;
                        for (size_t j = 0; j < k; ++j)
                            tmp[j] = (UCRT_CT)(ws[i + j] < 0x80 ? ws[i + j] : '?');
                        UCRT_FN(emit_chars)(sk, tmp, k);
                        i += k;
                    }
                }
                if (pad && (flags & UCRT_PF_LEFT))
                    UCRT_FN(emit_fill)(sk, ' ', pad);
            }
            break;
        }
        case 'n': {
            void *target = va_arg(ap, void *);
            if (!g_ucrt_state.printf_count_output || !target) {
                ucrt_invalid(UCRT_EINVAL);
                return -1;
            }
            if (size == UCRT_SZ_HH)
                *(signed char *)target = (signed char)sk->count;
            else if (size == UCRT_SZ_H)
                *(short *)target = (short)sk->count;
            else if (size == UCRT_SZ_LL)
                *(long long *)target = sk->count;
            else
                *(int *)target = (int)sk->count;
            break;
        }
        default:
            ucrt_invalid(UCRT_EINVAL);
            return -1;
        }
        if (sk->error)
            return -1;
        if (sk->count > 0x7FFFFFFFLL) {
            ucrt_set_errno(75 /* EOVERFLOW */);
            return -1;
        }
    }
    if (sk->error)
        return -1;
    return (int)sk->count;
}

#undef UCRT_FN
#undef UCRT_CAT
#undef UCRT_CAT_
