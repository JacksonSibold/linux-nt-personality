/* ucrtbase_stdio.inc - the formatted I/O entry points (included by ucrtbase.c).
 *
 * Programs built against the UCRT headers call __stdio_common_v*printf and __stdio_common_v*scanf for all of printf
 * and scanf; the classic names are exported as well for programs that import them directly. The options word of
 * the __stdio_common_* functions selects the variant:
 *   printf: 0x01 legacy vsprintf termination, 0x02 standard snprintf behaviour, 0x04 Microsoft wide specifiers (%s in a
 *           wide function is a wide string), 0x08 msvcrt compatibility, 0x10 three-digit exponents, 0x200 positional.
 *   scanf:  0x01 secure (_s) form, 0x02 Microsoft wide specifiers, 0x04 msvcrt compatibility.
 */

#define UCT_PF_LEGACY_TERMINATE 0x1u
#define UCT_PF_STANDARD_SNPRINTF 0x2u
#define UCT_PF_MS_WIDE 0x4u
#define UCT_PF_THREE_DIGIT_EXP 0x10u

typedef struct uct_pf_saved {
    int iso_wide, exp_digits;
} uct_pf_saved;

/* The engine reads its variant from the runtime state; set it for the call and put it back after. */
static uct_pf_saved uct_pf_enter(uint64_t options)
{
    uct_pf_saved s;
    s.iso_wide = g_ucrt_state.printf_iso_wide;
    s.exp_digits = g_ucrt_state.printf_exp_digits;
    g_ucrt_state.printf_iso_wide = (options & UCT_PF_MS_WIDE) ? 0 : 1;
    g_ucrt_state.printf_exp_digits = (options & UCT_PF_THREE_DIGIT_EXP) ? 3 : 2;
    return s;
}

static void uct_pf_leave(uct_pf_saved s)
{
    g_ucrt_state.printf_iso_wide = s.iso_wide;
    g_ucrt_state.printf_exp_digits = s.exp_digits;
}

int w___stdio_common_vfprintf(uint64_t options, ucrt_file *f, const char *fmt, void *loc, va_list ap)
{
    uct_pf_saved s = uct_pf_enter(options);
    int r = ucrt_vfprintf(f, fmt, ap);
    (void)loc;
    uct_pf_leave(s);
    return r;
}

int w___stdio_common_vfprintf_s(uint64_t options, ucrt_file *f, const char *fmt, void *loc, va_list ap)
{
    return w___stdio_common_vfprintf(options, f, fmt, loc, ap);
}

int w___stdio_common_vfprintf_p(uint64_t options, ucrt_file *f, const char *fmt, void *loc, va_list ap)
{
    return w___stdio_common_vfprintf(options, f, fmt, loc, ap);
}

int w___stdio_common_vfwprintf(uint64_t options, ucrt_file *f, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    uct_pf_saved s = uct_pf_enter(options);
    int r = ucrt_vfwprintf(f, fmt, ap);
    (void)loc;
    uct_pf_leave(s);
    return r;
}

int w___stdio_common_vfwprintf_s(uint64_t options, ucrt_file *f, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    return w___stdio_common_vfwprintf(options, f, fmt, loc, ap);
}

int w___stdio_common_vfwprintf_p(uint64_t options, ucrt_file *f, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    return w___stdio_common_vfwprintf(options, f, fmt, loc, ap);
}

/* vsprintf and the _snprintf forms. A count of -1 is "no limit"; the standard snprintf behaviour returns the length the
 * full result needs and always terminates; the legacy one returns -1 when the buffer is too small, and terminates only
 * when asked to (option 1) or when the result fits. */
int w___stdio_common_vsprintf(uint64_t options, char *buf, size_t count, const char *fmt, void *loc, va_list ap)
{
    uct_pf_saved s;
    int r;
    (void)loc;
    if (!fmt || (!buf && count)) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    s = uct_pf_enter(options);
    if (count == (size_t)-1) {
        r = ucrt_vformat_buf(buf, 0, UCRT_PF_UNBOUNDED, fmt, ap);
    } else if (options & UCT_PF_STANDARD_SNPRINTF) {
        r = ucrt_vformat_buf(buf, count, UCRT_PF_C99, fmt, ap);
    } else {
        r = ucrt_vformat_buf(buf, count, UCRT_PF_LEGACY, fmt, ap);
        if ((options & UCT_PF_LEGACY_TERMINATE) && count && (r < 0 || (size_t)r >= count))
            buf[count - 1] = 0;
    }
    uct_pf_leave(s);
    return r;
}

int w___stdio_common_vsprintf_s(uint64_t options, char *buf, size_t size, const char *fmt, void *loc, va_list ap)
{
    uct_pf_saved s = uct_pf_enter(options);
    int r = ucrt_vsprintf_s(buf, size, fmt, ap);
    (void)loc;
    uct_pf_leave(s);
    return r;
}

int w___stdio_common_vsprintf_p(uint64_t options, char *buf, size_t size, const char *fmt, void *loc, va_list ap)
{
    return w___stdio_common_vsprintf_s(options, buf, size, fmt, loc, ap);
}

/* _snprintf_s: `max` limits the characters written; _TRUNCATE (-1) means "as many as fit". */
int w___stdio_common_vsnprintf_s(uint64_t options, char *buf, size_t size, size_t max, const char *fmt, void *loc, va_list ap)
{
    uct_pf_saved s;
    int len;
    (void)loc;
    if (!buf || !size || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    s = uct_pf_enter(options);
    len = ucrt_vformat_buf(buf, size, UCRT_PF_C99, fmt, ap);
    uct_pf_leave(s);
    if (len < 0)
        return -1;
    if (max == (size_t)-1) {
        if ((size_t)len >= size) {
            buf[size - 1] = 0;
            return -1;
        }
        return len;
    }
    if ((size_t)len > max) {
        if (max >= size) {
            buf[0] = 0;
            ucrt_invalid(UCRT_ERANGE);
            return -1;
        }
        buf[max] = 0;
        return -1;
    }
    if ((size_t)len >= size) {
        buf[0] = 0;
        ucrt_invalid(UCRT_ERANGE);
        return -1;
    }
    return len;
}

int w___stdio_common_vswprintf(uint64_t options, ucrt_wchar *buf, size_t count, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    uct_pf_saved s;
    int r;
    (void)loc;
    if (!fmt || (!buf && count)) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    s = uct_pf_enter(options);
    if (count == (size_t)-1)
        r = ucrt_vformat_wbuf(buf, 0, UCRT_PF_UNBOUNDED, fmt, ap);
    else if ((options & UCT_PF_STANDARD_SNPRINTF) && !buf)
        r = ucrt_vformat_wbuf(NULL, 0, UCRT_PF_C99, fmt, ap); /* _scwprintf: only the length */
    else if (options & UCT_PF_STANDARD_SNPRINTF)
        r = ucrt_vformat_wbuf(buf, count, UCRT_PF_SWPRINTF, fmt, ap);
    else
        r = ucrt_vformat_wbuf(buf, count, UCRT_PF_LEGACY, fmt, ap);
    uct_pf_leave(s);
    return r;
}

int w___stdio_common_vswprintf_s(uint64_t options, ucrt_wchar *buf, size_t size, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    uct_pf_saved s;
    int len;
    (void)loc;
    if (!buf || !size || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    s = uct_pf_enter(options);
    len = ucrt_vformat_wbuf(buf, size, UCRT_PF_C99, fmt, ap);
    uct_pf_leave(s);
    if (len < 0)
        return -1;
    if ((size_t)len >= size) {
        buf[0] = 0;
        ucrt_invalid(UCRT_ERANGE);
        return -1;
    }
    return len;
}

int w___stdio_common_vsnwprintf_s(uint64_t options, ucrt_wchar *buf, size_t size, size_t max, const ucrt_wchar *fmt, void *loc,
                                  va_list ap)
{
    uct_pf_saved s;
    int len;
    (void)loc;
    if (!buf || !size || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    s = uct_pf_enter(options);
    len = ucrt_vformat_wbuf(buf, size, UCRT_PF_C99, fmt, ap);
    uct_pf_leave(s);
    if (len < 0)
        return -1;
    if (max == (size_t)-1) {
        if ((size_t)len >= size) {
            buf[size - 1] = 0;
            return -1;
        }
        return len;
    }
    if ((size_t)len > max) {
        if (max >= size) {
            buf[0] = 0;
            ucrt_invalid(UCRT_ERANGE);
            return -1;
        }
        buf[max] = 0;
        return -1;
    }
    if ((size_t)len >= size) {
        buf[0] = 0;
        ucrt_invalid(UCRT_ERANGE);
        return -1;
    }
    return len;
}

/* ---- scanf ---------------------------------------------------------------------------------------------------------- */

int w___stdio_common_vfscanf(uint64_t options, ucrt_file *f, const char *fmt, void *loc, va_list ap)
{
    (void)loc;
    return ucrt_vfscanf_n(f, fmt, ap, (int)(options & 1));
}

int w___stdio_common_vfwscanf(uint64_t options, ucrt_file *f, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    (void)loc;
    return ucrt_vfwscanf_n(f, fmt, ap, (int)(options & 1));
}

int w___stdio_common_vsscanf(uint64_t options, const char *buf, size_t count, const char *fmt, void *loc, va_list ap)
{
    (void)loc;
    return ucrt_vsscanf_n(buf, count, fmt, ap, (int)(options & 1));
}

int w___stdio_common_vswscanf(uint64_t options, const ucrt_wchar *buf, size_t count, const ucrt_wchar *fmt, void *loc, va_list ap)
{
    (void)loc;
    return ucrt_vswscanf_n(buf, count, fmt, ap, (int)(options & 1));
}

/* ---- The classic names ----------------------------------------------------------------------------------------------- */

/* The default options of the classic functions: Microsoft wide specifiers, and the legacy or standard snprintf rule. */
#define UCT_OPT_STD (UCT_PF_MS_WIDE)
#define UCT_OPT_SNPRINTF (UCT_PF_MS_WIDE | UCT_PF_STANDARD_SNPRINTF)
#define UCT_OPT_LEGACY (UCT_PF_MS_WIDE | UCT_PF_LEGACY_TERMINATE)
#define UCT_OPT_SNPRINTF_LEGACY (UCT_PF_MS_WIDE) /* _snprintf: a result that does not fit leaves no terminator */

int w_vfprintf(ucrt_file *f, const char *fmt, va_list ap) { return w___stdio_common_vfprintf(UCT_OPT_STD, f, fmt, 0, ap); }
int w_vprintf(const char *fmt, va_list ap) { return w___stdio_common_vfprintf(UCT_OPT_STD, ucrt_iob_func(1), fmt, 0, ap); }
int w_vsprintf(char *buf, const char *fmt, va_list ap)
{
    return w___stdio_common_vsprintf(UCT_OPT_LEGACY, buf, (size_t)-1, fmt, 0, ap);
}
int w_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return w___stdio_common_vsprintf(UCT_OPT_SNPRINTF, buf, n, fmt, 0, ap);
}
int w__vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return w___stdio_common_vsprintf(UCT_OPT_SNPRINTF_LEGACY, buf, n, fmt, 0, ap);
}
int w__vscprintf(const char *fmt, va_list ap) { return w___stdio_common_vsprintf(UCT_OPT_SNPRINTF, NULL, 0, fmt, 0, ap); }
int w_vfwprintf(ucrt_file *f, const ucrt_wchar *fmt, va_list ap) { return w___stdio_common_vfwprintf(UCT_OPT_STD, f, fmt, 0, ap); }
int w_vswprintf(ucrt_wchar *buf, size_t n, const ucrt_wchar *fmt, va_list ap)
{
    return w___stdio_common_vswprintf(UCT_OPT_SNPRINTF, buf, n, fmt, 0, ap);
}
int w__vsnwprintf(ucrt_wchar *buf, size_t n, const ucrt_wchar *fmt, va_list ap)
{
    return w___stdio_common_vswprintf(UCT_OPT_SNPRINTF_LEGACY, buf, n, fmt, 0, ap);
}
int w__vscwprintf(const ucrt_wchar *fmt, va_list ap)
{
    return w___stdio_common_vswprintf(UCT_OPT_SNPRINTF, NULL, 0, fmt, 0, ap);
}

#define UCT_VARIADIC(ret, name, params, last, call)                                                                    \
    ret name params                                                                                                    \
    {                                                                                                                  \
        va_list ap;                                                                                                    \
        ret r;                                                                                                         \
        va_start(ap, last);                                                                                            \
        r = call;                                                                                                      \
        va_end(ap);                                                                                                    \
        return r;                                                                                                      \
    }

UCT_VARIADIC(int, w_printf, (const char *fmt, ...), fmt, w_vprintf(fmt, ap))
UCT_VARIADIC(int, w_fprintf, (ucrt_file *f, const char *fmt, ...), fmt, w_vfprintf(f, fmt, ap))
UCT_VARIADIC(int, w_sprintf, (char *buf, const char *fmt, ...), fmt, w_vsprintf(buf, fmt, ap))
UCT_VARIADIC(int, w_snprintf, (char *buf, size_t n, const char *fmt, ...), fmt, w_vsnprintf(buf, n, fmt, ap))
UCT_VARIADIC(int, w__snprintf, (char *buf, size_t n, const char *fmt, ...), fmt, w__vsnprintf(buf, n, fmt, ap))
UCT_VARIADIC(int, w__scprintf, (const char *fmt, ...), fmt, w__vscprintf(fmt, ap))
UCT_VARIADIC(int, w_wprintf, (const ucrt_wchar *fmt, ...), fmt, w_vfwprintf(ucrt_iob_func(1), fmt, ap))
UCT_VARIADIC(int, w_fwprintf, (ucrt_file *f, const ucrt_wchar *fmt, ...), fmt, w_vfwprintf(f, fmt, ap))
UCT_VARIADIC(int, w_swprintf, (ucrt_wchar *buf, size_t n, const ucrt_wchar *fmt, ...), fmt, w_vswprintf(buf, n, fmt, ap))
UCT_VARIADIC(int, w__snwprintf, (ucrt_wchar *buf, size_t n, const ucrt_wchar *fmt, ...), fmt, w__vsnwprintf(buf, n, fmt, ap))
UCT_VARIADIC(int, w__scwprintf, (const ucrt_wchar *fmt, ...), fmt, w__vscwprintf(fmt, ap))
UCT_VARIADIC(int, w_printf_s, (const char *fmt, ...), fmt, w___stdio_common_vfprintf_s(UCT_OPT_STD, ucrt_iob_func(1), fmt, 0, ap))
UCT_VARIADIC(int, w_fprintf_s, (ucrt_file *f, const char *fmt, ...), fmt, w___stdio_common_vfprintf_s(UCT_OPT_STD, f, fmt, 0, ap))
UCT_VARIADIC(int, w_sprintf_s, (char *buf, size_t n, const char *fmt, ...), fmt,
             w___stdio_common_vsprintf_s(UCT_OPT_STD, buf, n, fmt, 0, ap))

int w_vsscanf(const char *s, const char *fmt, va_list ap) { return w___stdio_common_vsscanf(0, s, (size_t)-1, fmt, 0, ap); }
int w_vfscanf(ucrt_file *f, const char *fmt, va_list ap) { return w___stdio_common_vfscanf(0, f, fmt, 0, ap); }
int w_vswscanf(const ucrt_wchar *s, const ucrt_wchar *fmt, va_list ap)
{
    return w___stdio_common_vswscanf(0, s, (size_t)-1, fmt, 0, ap);
}

UCT_VARIADIC(int, w_sscanf, (const char *s, const char *fmt, ...), fmt, w___stdio_common_vsscanf(0, s, (size_t)-1, fmt, 0, ap))
UCT_VARIADIC(int, w_sscanf_s, (const char *s, const char *fmt, ...), fmt, w___stdio_common_vsscanf(1, s, (size_t)-1, fmt, 0, ap))
UCT_VARIADIC(int, w_fscanf, (ucrt_file *f, const char *fmt, ...), fmt, w___stdio_common_vfscanf(0, f, fmt, 0, ap))
UCT_VARIADIC(int, w_scanf, (const char *fmt, ...), fmt, w___stdio_common_vfscanf(0, ucrt_iob_func(0), fmt, 0, ap))
UCT_VARIADIC(int, w_swscanf, (const ucrt_wchar *s, const ucrt_wchar *fmt, ...), fmt,
             w___stdio_common_vswscanf(0, s, (size_t)-1, fmt, 0, ap))
UCT_VARIADIC(int, w_fwscanf, (ucrt_file *f, const ucrt_wchar *fmt, ...), fmt, w___stdio_common_vfwscanf(0, f, fmt, 0, ap))
UCT_VARIADIC(int, w_wscanf, (const ucrt_wchar *fmt, ...), fmt, w___stdio_common_vfwscanf(0, ucrt_iob_func(0), fmt, 0, ap))

/* %n is honoured only after this is set (the secure-CRT default is off). */
int w__set_printf_count_output(int on)
{
    int old = g_ucrt_state.printf_count_output;
    g_ucrt_state.printf_count_output = on != 0;
    return old;
}

int w__get_printf_count_output(void) { return g_ucrt_state.printf_count_output; }

void w_perror(const char *msg)
{
    ucrt_file *err = ucrt_iob_func(2);
    int e = ucrt_get_errno();
    if (msg && *msg) {
        ucrt_fputs(msg, err);
        ucrt_fputs(": ", err);
    }
    ucrt_fputs(ucrt_strerror(e), err);
    ucrt_fputc('\n', err);
}

void w__lock_file(ucrt_file *f)
{
    (void)f;
    ucrt_io_lock();
}

void w__unlock_file(ucrt_file *f)
{
    (void)f;
    ucrt_io_unlock();
}
