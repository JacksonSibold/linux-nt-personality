/*
 * ucrt_strftime_t.h - strftime for char and wide output, in the "C" locale, with the Microsoft conventions:
 * %c is "MM/DD/YY HH:MM:SS", %x "MM/DD/YY", %X "HH:MM:SS"; the # flag selects the long forms of %c and %x and
 * removes the leading zeros of the numeric fields; an unknown specifier is an invalid parameter.
 *
 * Included twice by ucrt_time.h with UCRT_CT and UCRT_P set.
 */
#define UCRT_CAT_(a, b) a##b
#define UCRT_CAT(a, b) UCRT_CAT_(a, b)
#define UCRT_FN(name) UCRT_CAT(UCRT_P, name)

typedef struct UCRT_FN(out) {
    UCRT_CT *buf;
    size_t cap, n;
    int overflow;
} UCRT_FN(out);

static inline void UCRT_FN(putc)(UCRT_FN(out) *o, int c)
{
    if (o->n + 1 < o->cap)
        o->buf[o->n] = (UCRT_CT)c;
    else
        o->overflow = 1;
    ++o->n;
}

static inline void UCRT_FN(puts)(UCRT_FN(out) *o, const char *s)
{
    while (*s)
        UCRT_FN(putc)(o, (unsigned char)*s++);
}

/* a number, padded with zeros (or spaces, pad == ' ') to width digits; with the # flag no padding at all */
static inline void UCRT_FN(putnum)(UCRT_FN(out) *o, long long v, int width, int pad, int nopad)
{
    char tmp[24];
    int n = 0, neg = v < 0;
    unsigned long long u = neg ? 0ull - (unsigned long long)v : (unsigned long long)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        UCRT_FN(putc)(o, '-');
    if (!nopad)
        for (int i = n; i < width; ++i)
            UCRT_FN(putc)(o, pad);
    while (n)
        UCRT_FN(putc)(o, tmp[--n]);
}

static inline int UCRT_FN(format)(UCRT_FN(out) *o, const UCRT_CT *fmt, const ucrt_tm *tm);

static inline int UCRT_FN(spec)(UCRT_FN(out) *o, int c, int alt, const ucrt_tm *tm)
{
    int h12;
    switch (c) {
    case 'a': UCRT_FN(puts)(o, ucrt_day_abbr[tm->tm_wday]); break;
    case 'A': UCRT_FN(puts)(o, ucrt_day_full[tm->tm_wday]); break;
    case 'b': case 'h': UCRT_FN(puts)(o, ucrt_month_abbr[tm->tm_mon]); break;
    case 'B': UCRT_FN(puts)(o, ucrt_month_full[tm->tm_mon]); break;
    case 'c':
        if (alt) { /* "dddd, MMMM dd, yyyy HH:mm:ss" */
            UCRT_FN(puts)(o, ucrt_day_full[tm->tm_wday]);
            UCRT_FN(puts)(o, ", ");
            UCRT_FN(puts)(o, ucrt_month_full[tm->tm_mon]);
            UCRT_FN(putc)(o, ' ');
            UCRT_FN(putnum)(o, tm->tm_mday, 2, '0', 0);
            UCRT_FN(puts)(o, ", ");
            UCRT_FN(putnum)(o, tm->tm_year + 1900, 4, '0', 0);
            UCRT_FN(putc)(o, ' ');
            UCRT_FN(putnum)(o, tm->tm_hour, 2, '0', 0);
            UCRT_FN(putc)(o, ':');
            UCRT_FN(putnum)(o, tm->tm_min, 2, '0', 0);
            UCRT_FN(putc)(o, ':');
            UCRT_FN(putnum)(o, tm->tm_sec, 2, '0', 0);
        } else {
            UCRT_FN(spec)(o, 'x', 0, tm);
            UCRT_FN(putc)(o, ' ');
            UCRT_FN(spec)(o, 'X', 0, tm);
        }
        break;
    case 'C': UCRT_FN(putnum)(o, (tm->tm_year + 1900) / 100, 2, '0', alt); break;
    case 'd': UCRT_FN(putnum)(o, tm->tm_mday, 2, '0', alt); break;
    case 'D':
        UCRT_FN(putnum)(o, tm->tm_mon + 1, 2, '0', 0);
        UCRT_FN(putc)(o, '/');
        UCRT_FN(putnum)(o, tm->tm_mday, 2, '0', 0);
        UCRT_FN(putc)(o, '/');
        UCRT_FN(putnum)(o, (tm->tm_year + 1900) % 100, 2, '0', 0);
        break;
    case 'e': UCRT_FN(putnum)(o, tm->tm_mday, 2, ' ', alt); break;
    case 'F':
        UCRT_FN(putnum)(o, tm->tm_year + 1900, 4, '0', 0);
        UCRT_FN(putc)(o, '-');
        UCRT_FN(putnum)(o, tm->tm_mon + 1, 2, '0', 0);
        UCRT_FN(putc)(o, '-');
        UCRT_FN(putnum)(o, tm->tm_mday, 2, '0', 0);
        break;
    case 'g': {
        int iy;
        ucrt_iso_week(tm, &iy);
        UCRT_FN(putnum)(o, iy % 100, 2, '0', alt);
        break;
    }
    case 'G': {
        int iy;
        ucrt_iso_week(tm, &iy);
        UCRT_FN(putnum)(o, iy, 4, '0', alt);
        break;
    }
    case 'H': UCRT_FN(putnum)(o, tm->tm_hour, 2, '0', alt); break;
    case 'I':
        h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        UCRT_FN(putnum)(o, h12, 2, '0', alt);
        break;
    case 'j': UCRT_FN(putnum)(o, tm->tm_yday + 1, 3, '0', alt); break;
    case 'm': UCRT_FN(putnum)(o, tm->tm_mon + 1, 2, '0', alt); break;
    case 'M': UCRT_FN(putnum)(o, tm->tm_min, 2, '0', alt); break;
    case 'n': UCRT_FN(putc)(o, '\n'); break;
    case 'p': UCRT_FN(puts)(o, tm->tm_hour >= 12 ? "PM" : "AM"); break;
    case 'r':
        UCRT_FN(spec)(o, 'I', 0, tm);
        UCRT_FN(putc)(o, ':');
        UCRT_FN(spec)(o, 'M', 0, tm);
        UCRT_FN(putc)(o, ':');
        UCRT_FN(spec)(o, 'S', 0, tm);
        UCRT_FN(putc)(o, ' ');
        UCRT_FN(spec)(o, 'p', 0, tm);
        break;
    case 'R':
        UCRT_FN(spec)(o, 'H', 0, tm);
        UCRT_FN(putc)(o, ':');
        UCRT_FN(spec)(o, 'M', 0, tm);
        break;
    case 'S': UCRT_FN(putnum)(o, tm->tm_sec, 2, '0', alt); break;
    case 't': UCRT_FN(putc)(o, '\t'); break;
    case 'T': case 'X':
        UCRT_FN(spec)(o, 'H', 0, tm);
        UCRT_FN(putc)(o, ':');
        UCRT_FN(spec)(o, 'M', 0, tm);
        UCRT_FN(putc)(o, ':');
        UCRT_FN(spec)(o, 'S', 0, tm);
        break;
    case 'u': UCRT_FN(putnum)(o, tm->tm_wday ? tm->tm_wday : 7, 1, '0', 0); break;
    case 'U': UCRT_FN(putnum)(o, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0', alt); break;
    case 'V': {
        int iy;
        UCRT_FN(putnum)(o, ucrt_iso_week(tm, &iy), 2, '0', alt);
        break;
    }
    case 'w': UCRT_FN(putnum)(o, tm->tm_wday, 1, '0', 0); break;
    case 'W': UCRT_FN(putnum)(o, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7, 2, '0', alt); break;
    case 'x':
        if (alt) { /* "dddd, MMMM dd, yyyy" */
            UCRT_FN(puts)(o, ucrt_day_full[tm->tm_wday]);
            UCRT_FN(puts)(o, ", ");
            UCRT_FN(puts)(o, ucrt_month_full[tm->tm_mon]);
            UCRT_FN(putc)(o, ' ');
            UCRT_FN(putnum)(o, tm->tm_mday, 2, '0', 0);
            UCRT_FN(puts)(o, ", ");
            UCRT_FN(putnum)(o, tm->tm_year + 1900, 4, '0', 0);
        } else {
            UCRT_FN(spec)(o, 'D', 0, tm);
        }
        break;
    case 'y': UCRT_FN(putnum)(o, (tm->tm_year + 1900) % 100, 2, '0', alt); break;
    case 'Y': UCRT_FN(putnum)(o, tm->tm_year + 1900, 1, '0', 0); break;
    case 'z': {
        int32_t bias = g_ucrt_tz.tz.bias + (tm->tm_isdst > 0 ? g_ucrt_tz.tz.dst_bias : g_ucrt_tz.tz.std_bias);
        int east = -bias;
        UCRT_FN(putc)(o, east < 0 ? '-' : '+');
        if (east < 0)
            east = -east;
        UCRT_FN(putnum)(o, east / 60, 2, '0', 0);
        UCRT_FN(putnum)(o, east % 60, 2, '0', 0);
        break;
    }
    case 'Z': UCRT_FN(puts)(o, g_ucrt_tz.tzname[tm->tm_isdst > 0 ? 1 : 0]); break;
    case '%': UCRT_FN(putc)(o, '%'); break;
    default: return -1;
    }
    return 0;
}

static inline int UCRT_FN(format)(UCRT_FN(out) *o, const UCRT_CT *fmt, const ucrt_tm *tm)
{
    for (; *fmt; ++fmt) {
        if (*fmt != '%') {
            UCRT_FN(putc)(o, (int)(unsigned)*fmt);
            continue;
        }
        ++fmt;
        {
            int alt = 0;
            if (*fmt == '#') {
                alt = 1;
                ++fmt;
            }
            if (!*fmt || UCRT_FN(spec)(o, (int)(unsigned)*fmt, alt, tm))
                return -1;
        }
    }
    return 0;
}

/* Returns the number of characters stored (not counting the NUL), or 0 when they do not fit or an argument is bad. */
static inline size_t UCRT_FN(strftime)(UCRT_CT *buf, size_t max, const UCRT_CT *fmt, const ucrt_tm *tm)
{
    UCRT_FN(out) o;
    if (!buf || max == 0 || !fmt || !tm) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    if (!ucrt_tm_valid(tm)) {
        buf[0] = 0;
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    ucrt_tz_ensure();
    o.buf = buf;
    o.cap = max;
    o.n = 0;
    o.overflow = 0;
    if (UCRT_FN(format)(&o, fmt, tm)) {
        buf[0] = 0;
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    if (o.overflow) {
        buf[0] = 0;
        return 0;
    }
    buf[o.n] = 0;
    return o.n;
}

#undef UCRT_FN
#undef UCRT_CAT
#undef UCRT_CAT_
