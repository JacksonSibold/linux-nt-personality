/*
 * ucrt_scanf.h - scanf for the C runtime: the source abstraction, the engine (ucrt_scanf_t.h) in narrow and
 * wide form, and sscanf, swscanf, fscanf, scanf and their _s forms. Host tests: tests/crt-host/test_scanf.c.
 */
#ifndef UCRT_SCANF_H
#define UCRT_SCANF_H

#include <stdarg.h>

#include "ucrt_ctype.h"
#include "ucrt_printf.h" /* for the size enum */
#include "ucrt_stdio.h"
#include "ucrt_strtod.h"

/* Where characters come from. get returns the next character or a negative value at the end; unget puts one back
 * (the engine calls it for characters read ahead, in the reverse of the order they were read). */
typedef struct ucrt_sc_src {
    void *ctx;
    int (*get)(void *ctx);
    void (*unget)(void *ctx, int c);
    long long consumed;
    int pushed[16];
    int npushed;
} ucrt_sc_src;

static inline int ucrt_sc_get(ucrt_sc_src *s)
{
    int c;
    if (s->npushed > 0) {
        c = s->pushed[--s->npushed];
        ++s->consumed;
        return c;
    }
    c = s->get(s->ctx);
    if (c >= 0)
        ++s->consumed;
    return c;
}

static inline void ucrt_sc_unget(ucrt_sc_src *s, int c)
{
    if (c < 0)
        return;
    if (s->npushed < 16)
        s->pushed[s->npushed++] = c;
    --s->consumed;
}

/* Gives the characters still held back to the underlying source. The first pushed is the last in the stream. */
static inline void ucrt_sc_finish(ucrt_sc_src *s)
{
    for (int i = 0; i < s->npushed; ++i)
        s->unget(s->ctx, s->pushed[i]);
    s->npushed = 0;
}

#define UCRT_CT char
#define UCRT_P ucrt_sc_
#define UCRT_WIDE 0
#include "ucrt_scanf_t.h"
#undef UCRT_CT
#undef UCRT_P
#undef UCRT_WIDE

#define UCRT_CT ucrt_wchar
#define UCRT_P ucrt_scw_
#define UCRT_WIDE 1
#include "ucrt_scanf_t.h"
#undef UCRT_CT
#undef UCRT_P
#undef UCRT_WIDE

/* ---- Sources ----------------------------------------------------------------------------------------- */

typedef struct ucrt_sc_str {
    const void *s;
    size_t pos, len; /* characters */
    int wide;
} ucrt_sc_str;

static inline int ucrt_sc_str_get(void *ctx)
{
    ucrt_sc_str *z = (ucrt_sc_str *)ctx;
    int c;
    if (z->pos >= z->len)
        return -1;
    c = z->wide ? ((const ucrt_wchar *)z->s)[z->pos] : ((const unsigned char *)z->s)[z->pos];
    if (c == 0 && z->len == (size_t)-1)
        return -1;
    ++z->pos;
    return c;
}

static inline void ucrt_sc_str_unget(void *ctx, int c)
{
    ucrt_sc_str *z = (ucrt_sc_str *)ctx;
    (void)c;
    if (z->pos > 0)
        --z->pos;
}

static inline int ucrt_sc_file_get(void *ctx)
{
    ucrt_file *f = (ucrt_file *)ctx;
    int c = ucrt_getc_nl(f);
    return c == UCRT_EOF ? -1 : c;
}

static inline void ucrt_sc_file_unget(void *ctx, int c)
{
    ucrt_ungetc(c, (ucrt_file *)ctx);
}

static inline int ucrt_scw_file_get(void *ctx)
{
    int c = ucrt_fgetwc_nl((ucrt_file *)ctx);
    return c == UCRT_WEOF_INT ? -1 : c;
}

static inline void ucrt_scw_file_unget(void *ctx, int c)
{
    ucrt_fx((ucrt_file *)ctx)->wide_unget = c;
}

static inline void ucrt_sc_src_init(ucrt_sc_src *s, void *ctx, int (*get)(void *), void (*unget)(void *, int))
{
    s->ctx = ctx;
    s->get = get;
    s->unget = unget;
    s->consumed = 0;
    s->npushed = 0;
}

/* ---- Entry points ------------------------------------------------------------------------------------ */

/* The common routine behind sscanf and the _s forms. count is the number of characters available, or (size_t)-1
 * for a NUL-terminated string. */
static inline int ucrt_vsscanf_n(const char *s, size_t count, const char *fmt, va_list ap, int secure)
{
    ucrt_sc_str z;
    ucrt_sc_src src;
    if (!s || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    z.s = s;
    z.pos = 0;
    z.len = count;
    z.wide = 0;
    ucrt_sc_src_init(&src, &z, ucrt_sc_str_get, ucrt_sc_str_unget);
    return ucrt_sc_vscan(&src, fmt, ap, secure);
}

static inline int ucrt_vswscanf_n(const ucrt_wchar *s, size_t count, const ucrt_wchar *fmt, va_list ap, int secure)
{
    ucrt_sc_str z;
    ucrt_sc_src src;
    if (!s || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    z.s = s;
    z.pos = 0;
    z.len = count;
    z.wide = 1;
    ucrt_sc_src_init(&src, &z, ucrt_sc_str_get, ucrt_sc_str_unget);
    return ucrt_scw_vscan(&src, fmt, ap, secure);
}

static inline int ucrt_vfscanf_n(ucrt_file *f, const char *fmt, va_list ap, int secure)
{
    ucrt_sc_src src;
    int r;
    if (!UCRT_FILE_VALID(f) || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_sc_src_init(&src, f, ucrt_sc_file_get, ucrt_sc_file_unget);
    ucrt_io_lock();
    r = ucrt_sc_vscan(&src, fmt, ap, secure);
    ucrt_io_unlock();
    return r;
}

static inline int ucrt_vfwscanf_n(ucrt_file *f, const ucrt_wchar *fmt, va_list ap, int secure)
{
    ucrt_sc_src src;
    int r;
    if (!UCRT_FILE_VALID(f) || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_sc_src_init(&src, f, ucrt_scw_file_get, ucrt_scw_file_unget);
    ucrt_io_lock();
    r = ucrt_scw_vscan(&src, fmt, ap, secure);
    ucrt_io_unlock();
    return r;
}

static inline int ucrt_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vsscanf_n(s, (size_t)-1, fmt, ap, 0);
    va_end(ap);
    return r;
}

static inline int ucrt_sscanf_s(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vsscanf_n(s, (size_t)-1, fmt, ap, 1);
    va_end(ap);
    return r;
}

static inline int ucrt_swscanf(const ucrt_wchar *s, const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vswscanf_n(s, (size_t)-1, fmt, ap, 0);
    va_end(ap);
    return r;
}

static inline int ucrt_fscanf(ucrt_file *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfscanf_n(f, fmt, ap, 0);
    va_end(ap);
    return r;
}

static inline int ucrt_fscanf_s(ucrt_file *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfscanf_n(f, fmt, ap, 1);
    va_end(ap);
    return r;
}

static inline int ucrt_scanf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfscanf_n(ucrt_stdin(), fmt, ap, 0);
    va_end(ap);
    return r;
}

static inline int ucrt_fwscanf(ucrt_file *f, const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfwscanf_n(f, fmt, ap, 0);
    va_end(ap);
    return r;
}

#endif /* UCRT_SCANF_H */
