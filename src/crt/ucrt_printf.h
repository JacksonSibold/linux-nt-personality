/*
 * ucrt_printf.h - printf-style formatting for the C runtime: the engine (ucrt_printf_t.h) for narrow and wide
 * output, buffer sinks, and the sprintf, snprintf and _snprintf families with their different return rules.
 * Host tests: tests/crt-host/test_printf.c against glibc and against expected Windows output.
 */
#ifndef UCRT_PRINTF_H
#define UCRT_PRINTF_H

#include <stdarg.h>

#include "ucrt_dtoa.h"
#include "ucrt_state.h"

/* Where formatted characters go. A sink implements put_n (narrow engine) or put_w (wide engine) and may set
 * error to make the formatting fail. count is the characters produced so far. */
typedef struct ucrt_pf_sink {
    void *ctx;
    void (*put_n)(void *ctx, const char *s, size_t n);
    void (*put_w)(void *ctx, const ucrt_wchar *s, size_t n);
    long long count;
    int error;
} ucrt_pf_sink;

#define UCRT_PF_FLOAT_BUF (UCRT_DTOA_MAX_DIGITS + 360)

#define UCRT_PF_LEFT 0x01
#define UCRT_PF_PLUS 0x02
#define UCRT_PF_SPACE 0x04
#define UCRT_PF_ALT 0x08
#define UCRT_PF_ZERO 0x10

enum { UCRT_SZ_DEFAULT, UCRT_SZ_HH, UCRT_SZ_H, UCRT_SZ_L, UCRT_SZ_LL, UCRT_SZ_I32, UCRT_SZ_W, UCRT_SZ_BIGL };


#define UCRT_CT char
#define UCRT_P ucrt_pf_
#define UCRT_WIDE 0
#define UCRT_PUT(sk, ptr, n) (sk)->put_n((sk)->ctx, (ptr), (n))
#include "ucrt_printf_t.h"
#undef UCRT_CT
#undef UCRT_P
#undef UCRT_WIDE
#undef UCRT_PUT

#define UCRT_CT ucrt_wchar
#define UCRT_P ucrt_pfw_
#define UCRT_WIDE 1
#define UCRT_PUT(sk, ptr, n) (sk)->put_w((sk)->ctx, (ptr), (n))
#include "ucrt_printf_t.h"
#undef UCRT_CT
#undef UCRT_P
#undef UCRT_WIDE
#undef UCRT_PUT

/* ---- Buffer sinks --------------------------------------------------------------------------------- */

typedef struct ucrt_pf_buf {
    void *buf;
    size_t cap; /* units the buffer holds */
    size_t pos; /* units stored so far (never past cap) */
} ucrt_pf_buf;

static inline void ucrt_pf_buf_put_n(void *ctx, const char *s, size_t n)
{
    ucrt_pf_buf *b = (ucrt_pf_buf *)ctx;
    char *dst = (char *)b->buf;
    for (size_t i = 0; i < n && b->pos < b->cap; ++i)
        dst[b->pos++] = s[i];
}

static inline void ucrt_pf_buf_put_w(void *ctx, const ucrt_wchar *s, size_t n)
{
    ucrt_pf_buf *b = (ucrt_pf_buf *)ctx;
    ucrt_wchar *dst = (ucrt_wchar *)b->buf;
    for (size_t i = 0; i < n && b->pos < b->cap; ++i)
        dst[b->pos++] = s[i];
}

/* How a buffer printf treats a result that does not fit. */
enum ucrt_pf_mode {
    UCRT_PF_C99,       /* snprintf: always terminates (if there is room for it), returns the full length */
    UCRT_PF_LEGACY,    /* _snprintf: terminates only when the result is shorter than the buffer; -1 if too long */
    UCRT_PF_SWPRINTF,  /* swprintf: terminates; returns -1 when the result does not fit */
    UCRT_PF_UNBOUNDED  /* sprintf: no limit */
};

static inline int ucrt_vformat_buf(char *buf, size_t n, enum ucrt_pf_mode mode, const char *fmt, va_list ap)
{
    ucrt_pf_buf b;
    ucrt_pf_sink sk;
    int len;
    b.buf = buf;
    b.cap = mode == UCRT_PF_UNBOUNDED ? (size_t)-1 : n;
    b.pos = 0;
    sk.ctx = &b;
    sk.put_n = ucrt_pf_buf_put_n;
    sk.put_w = NULL;
    sk.error = 0;
    len = ucrt_pf_vformat(&sk, (const char *)fmt, ap);
    if (len < 0) {
        if (n && buf && mode != UCRT_PF_UNBOUNDED)
            buf[0] = 0;
        return -1;
    }
    switch (mode) {
    case UCRT_PF_UNBOUNDED:
        buf[len] = 0;
        return len;
    case UCRT_PF_C99:
        if (n)
            buf[(size_t)len < n ? (size_t)len : n - 1] = 0;
        return len;
    case UCRT_PF_LEGACY:
        if ((size_t)len < n) {
            buf[len] = 0;
            return len;
        }
        return (size_t)len == n ? len : -1;
    case UCRT_PF_SWPRINTF:
        break;
    }
    return len; /* not reached for narrow output */
}

static inline int ucrt_vformat_wbuf(ucrt_wchar *buf, size_t n, enum ucrt_pf_mode mode, const ucrt_wchar *fmt, va_list ap)
{
    ucrt_pf_buf b;
    ucrt_pf_sink sk;
    int len;
    b.buf = buf;
    b.cap = mode == UCRT_PF_UNBOUNDED ? (size_t)-1 : n;
    b.pos = 0;
    sk.ctx = &b;
    sk.put_n = NULL;
    sk.put_w = ucrt_pf_buf_put_w;
    sk.error = 0;
    len = ucrt_pfw_vformat(&sk, fmt, ap);
    if (len < 0) {
        if (n && buf && mode != UCRT_PF_UNBOUNDED)
            buf[0] = 0;
        return -1;
    }
    switch (mode) {
    case UCRT_PF_UNBOUNDED:
        buf[len] = 0;
        return len;
    case UCRT_PF_C99:
        if (n)
            buf[(size_t)len < n ? (size_t)len : n - 1] = 0;
        return len;
    case UCRT_PF_LEGACY:
        if ((size_t)len < n) {
            buf[len] = 0;
            return len;
        }
        return (size_t)len == n ? len : -1;
    case UCRT_PF_SWPRINTF: /* the C99 swprintf: -1 when the output and its terminator do not fit */
        if ((size_t)len + 1 > n) {
            if (n)
                buf[0] = 0;
            return -1;
        }
        buf[len] = 0;
        return len;
    }
    return len;
}

static inline int ucrt_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vformat_buf(buf, n, UCRT_PF_C99, fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vformat_buf(buf, 0, UCRT_PF_UNBOUNDED, fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_legacy_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vformat_buf(buf, n, UCRT_PF_LEGACY, fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_swprintf(ucrt_wchar *buf, size_t n, const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vformat_wbuf(buf, n, UCRT_PF_SWPRINTF, fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_legacy_snwprintf(ucrt_wchar *buf, size_t n, const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vformat_wbuf(buf, n, UCRT_PF_LEGACY, fmt, ap);
    va_end(ap);
    return r;
}

/* sprintf_s and _snprintf_s: an unfit result empties the buffer and reports ERANGE. count is _TRUNCATE-aware. */
static inline int ucrt_vsprintf_s(char *buf, size_t size, const char *fmt, va_list ap)
{
    int len;
    if (!buf || size == 0 || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    len = ucrt_vformat_buf(buf, size, UCRT_PF_C99, fmt, ap);
    if (len < 0)
        return -1;
    if ((size_t)len >= size) {
        buf[0] = 0;
        ucrt_invalid(UCRT_ERANGE);
        return -1;
    }
    return len;
}

#endif /* UCRT_PRINTF_H */
