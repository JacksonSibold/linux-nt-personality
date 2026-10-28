/* ucrtbase_misc.inc - strings with state, character class tables, number-to-text conversions, multibyte conversion and the
 * time functions that return static buffers (included by ucrtbase.c). */

/* ---- Strings ------------------------------------------------------------------------------------------------------------------ */

char *w_strtok(char *s, const char *delim) { return ucrt_strtok_r(s, delim, &uct_thr()->tok); }
ucrt_wchar *w_wcstok(ucrt_wchar *s, const ucrt_wchar *delim) { return ucrt_wcstok_r(s, delim, &uct_thr()->wtok); }

char *w__strdup(const char *s)
{
    size_t n;
    char *d;
    if (!s)
        return NULL;
    n = ucrt_strlen(s) + 1;
    d = (char *)w_malloc(n);
    if (d)
        ucrt_memcpy(d, s, n);
    return d;
}

ucrt_wchar *w__wcsdup(const ucrt_wchar *s)
{
    size_t n;
    ucrt_wchar *d;
    if (!s)
        return NULL;
    n = ucrt_wcslen(s) + 1;
    d = (ucrt_wchar *)w_malloc(n * sizeof(ucrt_wchar));
    if (d)
        ucrt_memcpy(d, s, n * sizeof(ucrt_wchar));
    return d;
}

char *w_strerror(int e) { return (char *)(uintptr_t)ucrt_strerror(e); }

char *w__strerror(const char *msg)
{
    uct_thread *t = uct_thr();
    char *p = t->errbuf;
    size_t room = sizeof t->errbuf - 1, n;
    const char *text = ucrt_strerror(ucrt_get_errno());
    if (msg && *msg) {
        n = ucrt_strlen(msg);
        if (n > 94)
            n = 94;
        ucrt_memcpy(p, msg, n);
        p += n;
        room -= n;
        if (room > 2) {
            *p++ = ':';
            *p++ = ' ';
            room -= 2;
        }
    }
    n = ucrt_strlen(text);
    if (n + 1 > room)
        n = room - 1;
    ucrt_memcpy(p, text, n);
    p[n] = '\n';
    p[n + 1] = 0;
    return t->errbuf;
}

int w_strerror_s(char *buf, size_t size, int e)
{
    const char *text = ucrt_strerror(e);
    size_t n = ucrt_strlen(text);
    if (!buf || !size)
        return ucrt_invalid(UCRT_EINVAL);
    if (n + 1 > size) {
        ucrt_memcpy(buf, text, size - 1);
        buf[size - 1] = 0;
        return UCRT_ERANGE;
    }
    ucrt_memcpy(buf, text, n + 1);
    return 0;
}

ucrt_wchar *w__wcserror(int e)
{
    uct_thread *t = uct_thr();
    const char *text = ucrt_strerror(e);
    size_t i;
    for (i = 0; text[i] && i < 159; ++i)
        t->werrbuf[i] = (unsigned char)text[i];
    t->werrbuf[i] = 0;
    return t->werrbuf;
}

size_t w___strncnt(const char *s, size_t n) { return ucrt_strnlen(s, n); }
int w__mbscmp(const unsigned char *a, const unsigned char *b) { return ucrt_strcmp((const char *)a, (const char *)b); }

ucrt_wchar *w_wmemcpy(ucrt_wchar *d, const ucrt_wchar *s, size_t n) { return (ucrt_wchar *)ucrt_memcpy(d, s, n * sizeof(ucrt_wchar)); }
ucrt_wchar *w_wmemmove(ucrt_wchar *d, const ucrt_wchar *s, size_t n) { return (ucrt_wchar *)ucrt_memmove(d, s, n * sizeof(ucrt_wchar)); }
int w_wmemcmp(const ucrt_wchar *a, const ucrt_wchar *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

ucrt_wchar *w_wmemset(ucrt_wchar *d, ucrt_wchar c, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        d[i] = c;
    return d;
}

ucrt_wchar *w_wmemchr(const ucrt_wchar *s, ucrt_wchar c, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (s[i] == c)
            return (ucrt_wchar *)(uintptr_t)(s + i);
    return NULL;
}

int w_wmemcpy_s(ucrt_wchar *d, size_t dn, const ucrt_wchar *s, size_t n)
{
    return ucrt_memcpy_s(d, dn * sizeof(ucrt_wchar), s, n * sizeof(ucrt_wchar));
}

int w_wmemmove_s(ucrt_wchar *d, size_t dn, const ucrt_wchar *s, size_t n)
{
    return ucrt_memmove_s(d, dn * sizeof(ucrt_wchar), s, n * sizeof(ucrt_wchar));
}

/* ---- Character classes ----------------------------------------------------------------------------------------------------------- */

static unsigned short g_uct_pwctype[257];
static int g_uct_pwctype_ready;

/* The classification bits of a wide character (the same values as the narrow table). */
static unsigned uct_wclass(ucrt_wchar c)
{
    unsigned f = 0;
    if (ucrt_iswupper(c))
        f |= UCRT_UPPER;
    if (ucrt_iswlower(c))
        f |= UCRT_LOWER;
    if (ucrt_iswdigit(c))
        f |= UCRT_DIGIT;
    if (ucrt_iswspace(c))
        f |= UCRT_SPACE;
    if (ucrt_iswpunct(c))
        f |= UCRT_PUNCT;
    if (ucrt_iswcntrl(c))
        f |= UCRT_CONTROL;
    if (ucrt_iswblank(c))
        f |= UCRT_BLANK;
    if (ucrt_iswxdigit(c))
        f |= UCRT_HEX;
    if (ucrt_iswalpha(c))
        f |= 0x100u;
    return f;
}

const unsigned short *w___pctype_func(void)
{
    ucrt_loc_init();
    ucrt_loc_refresh_ctype();
    return ucrt_pctype();
}

const unsigned short *w___pwctype_func(void)
{
    if (!g_uct_pwctype_ready) {
        g_uct_pwctype[0] = 0; /* EOF */
        for (int c = 0; c < 256; ++c)
            g_uct_pwctype[c + 1] = (unsigned short)uct_wclass((ucrt_wchar)c);
        g_uct_pwctype_ready = 1;
    }
    return g_uct_pwctype + 1;
}

int w__isctype(int c, int mask) { return (int)(ucrt_loc_class(c) & (unsigned)mask); }

int w_iswctype(ucrt_wchar c, unsigned short mask) { return (int)(uct_wclass(c) & mask); }
int w__iswctype(ucrt_wchar c, unsigned short mask) { return w_iswctype(c, mask); }
int w__isleadbyte(int c) { return (ucrt_loc_class(c) & UCRT_LEADBYTE) != 0; }

int w____mb_cur_max_func(void)
{
    ucrt_loc_init();
    return g_ucrt_loc.mb_cur_max;
}

unsigned w____lc_codepage_func(void) { return (unsigned)ucrt_loc_codepage(); }

/* Locale objects are opaque to this runtime: every handle selects the process locale. */
static int g_uct_locale_object;
void *w__get_current_locale(void) { return &g_uct_locale_object; }
void *w__create_locale(int category, const char *name)
{
    (void)category;
    (void)name;
    return &g_uct_locale_object;
}
void w__free_locale(void *loc) { (void)loc; }

/* ---- Number to text ---------------------------------------------------------------------------------------------------------------- */

/* _itoa and _ltoa take 32-bit values: a negative one prints as a 32-bit unsigned number in any radix but 10. */
static int uct_i32toa_s(int32_t v, char *buf, size_t size, int radix)
{
    if (v < 0 && radix != 10)
        return ucrt_stri64toa_s((int64_t)(uint32_t)v, buf, size, radix, 1);
    return ucrt_stri64toa_s(v, buf, size, radix, 0);
}

static int uct_i32tow_s(int32_t v, ucrt_wchar *buf, size_t size, int radix)
{
    if (v < 0 && radix != 10)
        return ucrt_wcsi64toa_s((int64_t)(uint32_t)v, buf, size, radix, 1);
    return ucrt_wcsi64toa_s(v, buf, size, radix, 0);
}

char *w__itoa(int v, char *buf, int radix)
{
    uct_i32toa_s(v, buf, (size_t)-1 >> 1, radix);
    return buf;
}

char *w__ltoa(long v, char *buf, int radix)
{
    uct_i32toa_s((int32_t)v, buf, (size_t)-1 >> 1, radix);
    return buf;
}

char *w__ultoa(unsigned long v, char *buf, int radix)
{
    ucrt_stri64toa_s((int64_t)v, buf, (size_t)-1 >> 1, radix, 1);
    return buf;
}

char *w__i64toa(long long v, char *buf, int radix)
{
    ucrt_stri64toa_s(v, buf, (size_t)-1 >> 1, radix, 0);
    return buf;
}

char *w__ui64toa(unsigned long long v, char *buf, int radix)
{
    ucrt_stri64toa_s((int64_t)v, buf, (size_t)-1 >> 1, radix, 1);
    return buf;
}

int w__itoa_s(int v, char *buf, size_t size, int radix) { return uct_i32toa_s(v, buf, size, radix); }
int w__ltoa_s(long v, char *buf, size_t size, int radix) { return uct_i32toa_s((int32_t)v, buf, size, radix); }
int w__ultoa_s(unsigned long v, char *buf, size_t size, int radix) { return ucrt_stri64toa_s((int64_t)v, buf, size, radix, 1); }
int w__i64toa_s(long long v, char *buf, size_t size, int radix) { return ucrt_stri64toa_s(v, buf, size, radix, 0); }
int w__ui64toa_s(unsigned long long v, char *buf, size_t size, int radix) { return ucrt_stri64toa_s((int64_t)v, buf, size, radix, 1); }

ucrt_wchar *w__itow(int v, ucrt_wchar *buf, int radix)
{
    uct_i32tow_s(v, buf, (size_t)-1 >> 2, radix);
    return buf;
}

ucrt_wchar *w__ltow(long v, ucrt_wchar *buf, int radix)
{
    uct_i32tow_s((int32_t)v, buf, (size_t)-1 >> 2, radix);
    return buf;
}

ucrt_wchar *w__ultow(unsigned long v, ucrt_wchar *buf, int radix)
{
    ucrt_wcsi64toa_s((int64_t)v, buf, (size_t)-1 >> 2, radix, 1);
    return buf;
}

ucrt_wchar *w__i64tow(long long v, ucrt_wchar *buf, int radix)
{
    ucrt_wcsi64toa_s(v, buf, (size_t)-1 >> 2, radix, 0);
    return buf;
}

ucrt_wchar *w__ui64tow(unsigned long long v, ucrt_wchar *buf, int radix)
{
    ucrt_wcsi64toa_s((int64_t)v, buf, (size_t)-1 >> 2, radix, 1);
    return buf;
}

int w__itow_s(int v, ucrt_wchar *buf, size_t size, int radix) { return uct_i32tow_s(v, buf, size, radix); }
int w__ltow_s(long v, ucrt_wchar *buf, size_t size, int radix) { return uct_i32tow_s((int32_t)v, buf, size, radix); }
int w__ultow_s(unsigned long v, ucrt_wchar *buf, size_t size, int radix) { return ucrt_wcsi64toa_s((int64_t)v, buf, size, radix, 1); }
int w__i64tow_s(long long v, ucrt_wchar *buf, size_t size, int radix) { return ucrt_wcsi64toa_s(v, buf, size, radix, 0); }
int w__ui64tow_s(unsigned long long v, ucrt_wchar *buf, size_t size, int radix) { return ucrt_wcsi64toa_s((int64_t)v, buf, size, radix, 1); }

double w__wtof(const ucrt_wchar *s) { return ucrt_wcstod(s, NULL); }

/* _ecvt and _fcvt: the digits of value without a decimal point, with the position of the point and the sign beside them. */
static char *uct_cvt(double value, int ndigits, int *dec, int *sign, int fixed)
{
    uct_thread *t = uct_thr();
    char digits[UCRT_DTOA_MAX_DIGITS + 8];
    int decpt = 0, n, i, want;
    *sign = ucrt_signbit(value) ? 1 : 0;
    if (ndigits < 0)
        ndigits = 0;
    if (ndigits > 340)
        ndigits = 340;
    if (ucrt_isnan(value) || ucrt_isinf(value)) {
        const char *s = ucrt_isnan(value) ? "1#QNAN" : "1#INF";
        n = (int)ucrt_strlen(s);
        ucrt_memcpy(t->cvt, s, (size_t)n + 1);
        *dec = 1;
        return t->cvt;
    }
    if (value == 0) {
        want = fixed ? ndigits : ndigits;
        for (i = 0; i < want; ++i)
            t->cvt[i] = '0';
        t->cvt[want] = 0;
        *dec = 0;
        return t->cvt;
    }
    n = ucrt_dtoa(ucrt_fabs(value), fixed ? 3 : 2, ndigits, digits, &decpt);
    want = fixed ? decpt + ndigits : ndigits;
    if (want < 0)
        want = 0;
    if (want > 390)
        want = 390;
    for (i = 0; i < want; ++i)
        t->cvt[i] = i < n ? digits[i] : '0';
    t->cvt[want] = 0;
    *dec = decpt;
    return t->cvt;
}

char *w__ecvt(double v, int ndigits, int *dec, int *sign) { return uct_cvt(v, ndigits, dec, sign, 0); }
char *w__fcvt(double v, int ndigits, int *dec, int *sign) { return uct_cvt(v, ndigits, dec, sign, 1); }

int w__ecvt_s(char *buf, size_t size, double v, int ndigits, int *dec, int *sign)
{
    const char *s;
    size_t n;
    if (!buf || !dec || !sign || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    s = uct_cvt(v, ndigits, dec, sign, 0);
    n = ucrt_strlen(s);
    if (n + 1 > size) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memcpy(buf, s, n + 1);
    return 0;
}

int w__fcvt_s(char *buf, size_t size, double v, int ndigits, int *dec, int *sign)
{
    const char *s;
    size_t n;
    if (!buf || !dec || !sign || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    s = uct_cvt(v, ndigits, dec, sign, 1);
    n = ucrt_strlen(s);
    if (n + 1 > size) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memcpy(buf, s, n + 1);
    return 0;
}

/* _gcvt: like %.*g, but a result that would show only digits ends in a decimal point ("1."), as the Microsoft runtime has it. */
char *w__gcvt(double v, int digits, char *buf)
{
    size_t n;
    int plain = 1;
    ucrt_snprintf(buf, 400, "%.*g", digits < 1 ? 1 : digits, v);
    n = ucrt_strlen(buf);
    for (size_t i = 0; i < n; ++i)
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E' || buf[i] == 'n' || buf[i] == 'i')
            plain = 0;
    if (plain) {
        buf[n] = '.';
        buf[n + 1] = 0;
    }
    return buf;
}

int w__gcvt_s(char *buf, size_t size, double v, int digits)
{
    char tmp[400];
    size_t n;
    if (!buf || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    w__gcvt(v, digits, tmp);
    n = ucrt_strlen(tmp);
    if (n + 1 > size) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memcpy(buf, tmp, n + 1);
    return 0;
}

/* ---- Multibyte restartable conversions ------------------------------------------------------------------------------------------------ */

size_t w_mbsrtowcs(ucrt_wchar *dst, const char **src, size_t len, ucrt_mbstate *st)
{
    size_t n = 0;
    const char *s;
    ucrt_mbstate local;
    if (!src || !*src) {
        ucrt_invalid(UCRT_EINVAL);
        return (size_t)-1;
    }
    if (!st) {
        ucrt_memset(&local, 0, sizeof local);
        st = &local;
    }
    s = *src;
    for (;;) {
        ucrt_wchar wc = 0;
        size_t r;
        if (dst && n >= len)
            break;
        r = ucrt_mbrtowc(&wc, s, 8, st);
        if (r == (size_t)-1 || r == (size_t)-2) {
            if (dst)
                *src = s;
            return (size_t)-1;
        }
        if (r == 0) {
            if (dst) {
                dst[n] = 0;
                *src = NULL;
            }
            return n;
        }
        if (dst)
            dst[n] = wc;
        ++n;
        if (r != (size_t)-3) /* -3: the second half of a surrogate pair; no input was used */
            s += r;
    }
    *src = s;
    return n;
}

size_t w_wcsrtombs(char *dst, const ucrt_wchar **src, size_t len, ucrt_mbstate *st)
{
    size_t n = 0;
    const ucrt_wchar *s;
    ucrt_mbstate local;
    if (!src || !*src) {
        ucrt_invalid(UCRT_EINVAL);
        return (size_t)-1;
    }
    if (!st) {
        ucrt_memset(&local, 0, sizeof local);
        st = &local;
    }
    s = *src;
    for (;;) {
        char tmp[8];
        size_t r = ucrt_wcrtomb(tmp, *s, st);
        if (r == (size_t)-1) {
            if (dst)
                *src = s;
            return (size_t)-1;
        }
        if (*s == 0) { /* r includes the shift-back bytes and the terminator */
            if (dst) {
                if (n + r > len) {
                    *src = s;
                    return n;
                }
                ucrt_memcpy(dst + n, tmp, r);
                *src = NULL;
            }
            return n + (r ? r - 1 : 0);
        }
        if (dst) {
            if (n + r > len)
                break;
            ucrt_memcpy(dst + n, tmp, r);
        }
        n += r;
        ++s;
    }
    *src = s;
    return n;
}

size_t w__mbslen(const unsigned char *s) { return ucrt_mbstrlen((const char *)s); }

/* ---- Time functions with static results ---------------------------------------------------------------------------------------------- */

ucrt_tm *w__localtime64(const int64_t *t)
{
    ucrt_tm *tm = &uct_thr()->tm;
    return ucrt_localtime64_s(tm, t) ? NULL : tm;
}

ucrt_tm *w__gmtime64(const int64_t *t)
{
    ucrt_tm *tm = &uct_thr()->tm;
    return ucrt_gmtime64_s(tm, t) ? NULL : tm;
}

ucrt_tm *w_localtime(const int64_t *t) { return w__localtime64(t); }
ucrt_tm *w_gmtime(const int64_t *t) { return w__gmtime64(t); }

char *w__ctime64(const int64_t *t)
{
    char *b = uct_thr()->timebuf;
    return ucrt_ctime64_s(b, 32, t) ? NULL : b;
}

char *w_ctime(const int64_t *t) { return w__ctime64(t); }

char *w_asctime(const ucrt_tm *tm)
{
    char *b = uct_thr()->timebuf;
    return ucrt_asctime_s(b, 32, tm) ? NULL : b;
}

int w__localtime32_s(ucrt_tm *tm, const int *t)
{
    int64_t v;
    if (!t) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    v = *t;
    return ucrt_localtime64_s(tm, &v);
}

int w__gmtime32_s(ucrt_tm *tm, const int *t)
{
    int64_t v;
    if (!t) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    v = *t;
    return ucrt_gmtime64_s(tm, &v);
}

int w__ctime32_s(char *buf, size_t size, const int *t)
{
    int64_t v;
    if (!t) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    v = *t;
    return ucrt_ctime64_s(buf, size, &v);
}

int w__mktime32(ucrt_tm *tm)
{
    long long v = ucrt_mktime64(tm);
    return v < 0 || v > 0x7FFFFFFF ? -1 : (int)v;
}

int w__ftime64_s(ucrt_timeb *tb)
{
    if (!tb)
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_ftime64(tb);
    return 0;
}

void w__ftime64(ucrt_timeb *tb)
{
    if (tb)
        ucrt_ftime64(tb);
}

/* _timezone, _daylight, _dstbias and _tzname are variables the program reads through these accessors. */
long *w___p__timezone(void)
{
    ucrt_tz_ensure();
    return (long *)(void *)&g_ucrt_tz.timezone;
}

int *w___p__daylight(void)
{
    ucrt_tz_ensure();
    return &g_ucrt_tz.daylight;
}

long *w___p__dstbias(void)
{
    ucrt_tz_ensure();
    return (long *)(void *)&g_ucrt_tz.dstbias;
}

static char *g_uct_tzname[2];

char **w___p__tzname(void)
{
    ucrt_tz_ensure();
    g_uct_tzname[0] = g_ucrt_tz.tzname[0];
    g_uct_tzname[1] = g_ucrt_tz.tzname[1];
    return g_uct_tzname;
}

int w__get_timezone(long *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_tz_ensure();
    *out = g_ucrt_tz.timezone;
    return 0;
}

int w__get_daylight(int *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_tz_ensure();
    *out = g_ucrt_tz.daylight;
    return 0;
}

int w__get_dstbias(long *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_tz_ensure();
    *out = g_ucrt_tz.dstbias;
    return 0;
}

int w__get_tzname(size_t *len, char *buf, size_t size, int index)
{
    const char *name;
    size_t n;
    if (index < 0 || index > 1 || !len || (buf == NULL && size != 0))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_tz_ensure();
    name = g_ucrt_tz.tzname[index];
    n = ucrt_strlen(name) + 1;
    *len = n;
    if (!buf)
        return 0;
    if (size < n) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memcpy(buf, name, n);
    return 0;
}
