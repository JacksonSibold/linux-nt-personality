/*
 * ucrt_stdio.h - C streams (FILE) on top of the file descriptors of ucrt_io.h: buffering, fopen and its mode
 * strings, getc/putc and the string and block functions, fseek/ftell with text-mode translation, the wide
 * character functions with their ANSI, UTF-16 and UTF-8 stream modes, and printf to streams.
 *
 * The FILE structure has the 48-byte layout of the classic Microsoft one (the _file field is what the
 * _fileno macro of old headers reads), so programs built against either header work. Host tests:
 * tests/crt-host/test_stdio.c.
 */
#ifndef UCRT_STDIO_H
#define UCRT_STDIO_H

#include <stdarg.h>

#include "ucrt_io.h"
#include "ucrt_printf.h"
#include "ucrt_string.h"

typedef struct ucrt_file {
    char *_ptr;
    int _cnt;
    char *_base;
    int _flag;
    int _file;
    int _charbuf;
    int _bufsiz;
    char *_tmpfname;
} ucrt_file;

_Static_assert(sizeof(ucrt_file) == 48, "FILE must keep the 48-byte layout of the Microsoft runtime");

/* _flag bits */
#define UCRT_IOREAD 0x0001
#define UCRT_IOWRT 0x0002
#define UCRT_IONBF 0x0004
#define UCRT_IOMYBUF 0x0008
#define UCRT_IOEOF 0x0010
#define UCRT_IOERR 0x0020
#define UCRT_IOSTRG 0x0040
#define UCRT_IORW 0x0080
#define UCRT_IOYOURBUF 0x0100
#define UCRT_IOAPPEND 0x0200
#define UCRT_IOSETVBUF 0x0400
#define UCRT_IOUSED 0x8000 /* the slot is in use (a closed stream has no other bit set) */

/* setvbuf modes */
#define UCRT_IOFBF 0x0000
#define UCRT_IOLBF 0x0040
#define UCRT_IONBF_MODE 0x0004

#define UCRT_BUFSIZ 512
#define UCRT_INTERNAL_BUFSIZ 4096
#define UCRT_NSTREAM 512

#define UCRT_SEEK_SET 0
#define UCRT_SEEK_CUR 1
#define UCRT_SEEK_END 2

/* Per-stream state kept beside the FILE (the structure itself has no room). */
typedef struct ucrt_file_x {
    int64_t rawstart; /* the file offset at which the current read buffer was loaded */
    int has_rawstart;
    int wide_unget;   /* a wide character pushed back by ungetwc, or -1 */
    int high;         /* a pending UTF-16 high surrogate being written to a UTF-8 stream, or 0 */
    int cr_pending;   /* a CR read from a wide text stream whose LF decision is pending: -1 none */
} ucrt_file_x;

typedef struct ucrt_stdio_state {
    ucrt_file iob[UCRT_NSTREAM];
    ucrt_file_x x[UCRT_NSTREAM];
    int initialised;
} ucrt_stdio_state;

static ucrt_stdio_state g_ucrt_stdio;

#define UCRT_WEOF_INT 0xFFFF

static inline void ucrt_stdio_init(void)
{
    if (g_ucrt_stdio.initialised)
        return;
    g_ucrt_stdio.initialised = 1;
    for (int i = 0; i < UCRT_NSTREAM; ++i) {
        __builtin_memset(&g_ucrt_stdio.iob[i], 0, sizeof(ucrt_file));
        g_ucrt_stdio.iob[i]._file = -1;
        g_ucrt_stdio.x[i].wide_unget = -1;
        g_ucrt_stdio.x[i].cr_pending = -1;
    }
    for (int i = 0; i < 3; ++i) {
        g_ucrt_stdio.iob[i]._file = i;
        g_ucrt_stdio.iob[i]._flag = UCRT_IOUSED | (i == 0 ? UCRT_IOREAD : UCRT_IOWRT);
    }
}

static inline ucrt_file *ucrt_stdin(void)
{
    ucrt_stdio_init();
    return &g_ucrt_stdio.iob[0];
}

static inline ucrt_file *ucrt_stdout(void)
{
    ucrt_stdio_init();
    return &g_ucrt_stdio.iob[1];
}

static inline ucrt_file *ucrt_stderr(void)
{
    ucrt_stdio_init();
    return &g_ucrt_stdio.iob[2];
}

/* __acrt_iob_func(0..2) and the _iob_func array */
static inline ucrt_file *ucrt_iob_func(unsigned i)
{
    ucrt_stdio_init();
    return &g_ucrt_stdio.iob[i < 3 ? i : 0];
}

static inline ucrt_file_x *ucrt_fx(ucrt_file *f)
{
    return &g_ucrt_stdio.x[f - g_ucrt_stdio.iob];
}

/* ---- Buffers ------------------------------------------------------------------------------------------ */

/* Gives a stream its buffer on first use. A write-only stream on a device (the console) and stderr are
 * unbuffered; everything else gets an internal buffer, or the single-character one when allocation fails. */
static inline void ucrt_getbuf(ucrt_file *f)
{
    int unbuffered = (f->_flag & UCRT_IONBF) != 0;
    if (f->_base)
        return;
    if (!unbuffered && f->_file >= 0) {
        ucrt_fd *fd = ucrt_fd_get(f->_file);
        if (f->_file == 2)
            unbuffered = 1;
        else if (fd && (fd->flags & UCRT_FDEV) && !(f->_flag & (UCRT_IOREAD | UCRT_IORW)))
            unbuffered = 1;
    }
    if (!unbuffered) {
        char *b = (char *)ucrt_alloc(UCRT_INTERNAL_BUFSIZ);
        if (b) {
            f->_base = f->_ptr = b;
            f->_bufsiz = UCRT_INTERNAL_BUFSIZ;
            f->_flag |= UCRT_IOMYBUF;
            f->_cnt = 0;
            return;
        }
    }
    f->_flag |= UCRT_IONBF;
    f->_base = f->_ptr = (char *)&f->_charbuf;
    f->_bufsiz = 1;
    f->_cnt = 0;
}

static inline void ucrt_freebuf(ucrt_file *f)
{
    if ((f->_flag & UCRT_IOMYBUF) && f->_base)
        ucrt_release(f->_base);
    f->_base = f->_ptr = NULL;
    f->_cnt = 0;
    f->_bufsiz = 0;
    f->_flag &= ~(UCRT_IOMYBUF | UCRT_IOYOURBUF | UCRT_IONBF | UCRT_IOSETVBUF);
}

/* Writes out the pending bytes of a write buffer. Returns 0 or UCRT_EOF. */
static inline int ucrt_flush_nl(ucrt_file *f)
{
    int n;
    if ((f->_flag & (UCRT_IOWRT | UCRT_IOSTRG)) != UCRT_IOWRT || !f->_base)
        return 0;
    n = (int)(f->_ptr - f->_base);
    if (n > 0 && !(f->_flag & UCRT_IONBF && f->_base == (char *)&f->_charbuf)) {
        int w = ucrt_write(f->_file, f->_base, (unsigned)n);
        f->_ptr = f->_base;
        f->_cnt = f->_bufsiz;
        if (w != n) {
            f->_flag |= UCRT_IOERR;
            return UCRT_EOF;
        }
        return 0;
    }
    f->_ptr = f->_base;
    f->_cnt = f->_bufsiz;
    return 0;
}

/* The raw file offset of the next unread character of a read stream in text mode: the translation of line ends
 * means the buffer holds fewer characters than the file bytes it came from, so rescan the raw bytes. */
static inline int64_t ucrt_raw_offset_of(ucrt_file *f, int64_t filepos, int consumed)
{
    ucrt_file_x *x = ucrt_fx(f);
    ucrt_fd *fd = ucrt_fd_get(f->_file);
    unsigned char raw[UCRT_INTERNAL_BUFSIZ + 8];
    int64_t n, p;
    int i = 0, out = 0;
    unsigned got = 0;
    unsigned char saved;
    if (!x->has_rawstart || !fd)
        return filepos - f->_cnt;
    n = filepos - x->rawstart;
    if (n <= 0 || n > (int64_t)sizeof(raw))
        return filepos - f->_cnt;
    saved = fd->flags;
    fd->flags &= (unsigned char)~UCRT_FTEXT;
    g_ucrt_os->seek(fd->handle, x->rawstart, 0, &p);
    if (g_ucrt_os->read(fd->handle, raw, (unsigned)n, &got) != 0)
        got = 0;
    g_ucrt_os->seek(fd->handle, filepos, 0, &p);
    fd->flags = saved;
    while (out < consumed && (int64_t)i < (int64_t)got) {
        if (raw[i] == '\r' && (int64_t)i + 1 < (int64_t)got && raw[i + 1] == '\n')
            i += 2;
        else
            ++i;
        ++out;
    }
    return x->rawstart + i;
}

static inline int64_t ucrt_ftell_nl(ucrt_file *f)
{
    int64_t pos;
    ucrt_fd *fd = ucrt_fd_get(f->_file);
    if (!fd) {
        ucrt_set_errno(UCRT_EBADF);
        return -1;
    }
    pos = ucrt_lseeki64(f->_file, 0, UCRT_SEEK_CUR);
    if (pos < 0)
        return -1;
    if (f->_flag & UCRT_IOWRT) {
        if (f->_base && f->_base != (char *)&f->_charbuf) {
            int n = (int)(f->_ptr - f->_base);
            pos += n;
            if ((fd->flags & UCRT_FTEXT) && fd->textmode == UCRT_TM_ANSI)
                for (int i = 0; i < n; ++i)
                    if (f->_base[i] == '\n')
                        ++pos;
        }
        return pos;
    }
    if (f->_cnt <= 0 || !f->_base)
        return pos;
    if ((fd->flags & UCRT_FTEXT) && fd->textmode == UCRT_TM_ANSI) {
        int consumed = (int)(f->_ptr - f->_base);
        return ucrt_raw_offset_of(f, pos, consumed);
    }
    return pos - f->_cnt;
}

/* Brings a stream into read mode (flushing writes first) or write mode (dropping read-ahead). */
static inline int ucrt_to_read(ucrt_file *f)
{
    if (!(f->_flag & (UCRT_IOREAD | UCRT_IORW))) {
        f->_flag |= UCRT_IOERR;
        ucrt_set_errno(UCRT_EBADF);
        return UCRT_EOF;
    }
    if (f->_flag & UCRT_IOWRT) {
        if (ucrt_flush_nl(f))
            return UCRT_EOF;
        f->_flag &= ~UCRT_IOWRT;
        f->_cnt = 0;
        f->_ptr = f->_base;
    }
    f->_flag |= UCRT_IOREAD;
    return 0;
}

static inline int ucrt_to_write(ucrt_file *f)
{
    if (!(f->_flag & (UCRT_IOWRT | UCRT_IORW))) {
        f->_flag |= UCRT_IOERR;
        ucrt_set_errno(UCRT_EBADF);
        return UCRT_EOF;
    }
    if ((f->_flag & UCRT_IOREAD) && !(f->_flag & UCRT_IOSTRG)) {
        /* a read buffer with unread data: put the file position where the program thinks it is */
        if (f->_cnt > 0 && f->_base) {
            int64_t p = ucrt_ftell_nl(f);
            if (p >= 0)
                ucrt_lseeki64(f->_file, p, UCRT_SEEK_SET);
        }
        f->_flag &= ~UCRT_IOREAD;
        f->_cnt = 0;
        f->_ptr = f->_base;
    }
    f->_flag |= UCRT_IOWRT;
    if (!f->_base)
        ucrt_getbuf(f);
    if (f->_cnt == 0 && f->_ptr == f->_base)
        f->_cnt = f->_bufsiz;
    return 0;
}

/* Refills the read buffer and returns the first character, or UCRT_EOF. */
static inline int ucrt_filbuf_nl(ucrt_file *f)
{
    int n;
    ucrt_file_x *x = ucrt_fx(f);
    if (f->_flag & UCRT_IOSTRG)
        return UCRT_EOF;
    if (ucrt_to_read(f))
        return UCRT_EOF;
    if (!f->_base)
        ucrt_getbuf(f);
    x->rawstart = ucrt_lseeki64(f->_file, 0, UCRT_SEEK_CUR);
    x->has_rawstart = x->rawstart >= 0;
    n = ucrt_read(f->_file, f->_base, (unsigned)f->_bufsiz);
    if (n <= 0) {
        f->_flag |= n == 0 ? UCRT_IOEOF : UCRT_IOERR;
        f->_cnt = 0;
        return UCRT_EOF;
    }
    f->_cnt = n - 1;
    f->_ptr = f->_base + 1;
    return (unsigned char)f->_base[0];
}

/* Stores one character in a write buffer, flushing as needed. Returns the character or UCRT_EOF. */
static inline int ucrt_flsbuf_nl(int c, ucrt_file *f)
{
    if (ucrt_to_write(f))
        return UCRT_EOF;
    if (f->_flag & UCRT_IOSTRG)
        return UCRT_EOF;
    if (f->_base == (char *)&f->_charbuf || f->_bufsiz <= 1) {
        char ch = (char)c;
        if (ucrt_write(f->_file, &ch, 1) != 1) {
            f->_flag |= UCRT_IOERR;
            return UCRT_EOF;
        }
        return (unsigned char)c;
    }
    if (f->_ptr >= f->_base + f->_bufsiz && ucrt_flush_nl(f))
        return UCRT_EOF;
    *f->_ptr++ = (char)c;
    f->_cnt = f->_bufsiz - (int)(f->_ptr - f->_base);
    return (unsigned char)c;
}

static inline int ucrt_getc_nl(ucrt_file *f)
{
    if ((f->_flag & UCRT_IOWRT) && ucrt_to_read(f))
        return UCRT_EOF;
    if (--f->_cnt >= 0)
        return (unsigned char)*f->_ptr++;
    f->_cnt = 0;
    return ucrt_filbuf_nl(f);
}

static inline int ucrt_putc_nl(int c, ucrt_file *f)
{
    if (!(f->_flag & UCRT_IOWRT) || !f->_base || f->_cnt <= 0 || f->_base == (char *)&f->_charbuf)
        return ucrt_flsbuf_nl(c, f);
    *f->_ptr++ = (char)c;
    --f->_cnt;
    return (unsigned char)c;
}

#define UCRT_FILE_VALID(f) ((f) && ((f)->_flag & UCRT_IOUSED))

static inline int ucrt_fgetc(ucrt_file *f)
{
    int c;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_io_lock();
    c = ucrt_getc_nl(f);
    ucrt_io_unlock();
    return c;
}

static inline int ucrt_fputc(int c, ucrt_file *f)
{
    int r;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_io_lock();
    r = ucrt_putc_nl(c, f);
    ucrt_io_unlock();
    return r;
}

static inline int ucrt_ungetc(int c, ucrt_file *f)
{
    int r = UCRT_EOF;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    if (c == UCRT_EOF)
        return UCRT_EOF;
    ucrt_io_lock();
    if ((f->_flag & UCRT_IOWRT) && !(f->_flag & UCRT_IORW)) {
        ucrt_io_unlock();
        return UCRT_EOF;
    }
    if (!(f->_flag & (UCRT_IOREAD | UCRT_IORW))) {
        ucrt_io_unlock();
        return UCRT_EOF;
    }
    if (f->_flag & UCRT_IOWRT)
        ucrt_to_read(f);
    if (!f->_base)
        ucrt_getbuf(f);
    if (f->_ptr == f->_base) {
        if (f->_cnt == 0)
            ++f->_ptr; /* an empty buffer: make room for one character */
        else
            goto out;  /* a full buffer has nowhere to put it */
    }
    if (f->_flag & UCRT_IOSTRG) {
        if (*(f->_ptr - 1) != (char)c)
            goto out;
        --f->_ptr;
    } else {
        *--f->_ptr = (char)c;
    }
    ++f->_cnt;
    f->_flag &= ~UCRT_IOEOF;
    f->_flag |= UCRT_IOREAD;
    r = (unsigned char)c;
out:
    ucrt_io_unlock();
    return r;
}

/* ---- Blocks and lines -------------------------------------------------------------------------------- */

static inline size_t ucrt_fread_nl(void *buffer, size_t size, size_t count, ucrt_file *f)
{
    char *dst = (char *)buffer;
    size_t total = size * count, left = total;
    if ((f->_flag & UCRT_IOWRT) && ucrt_to_read(f))
        return 0;
    while (left) {
        if (f->_cnt > 0) {
            size_t n = (size_t)f->_cnt < left ? (size_t)f->_cnt : left;
            __builtin_memcpy(dst, f->_ptr, n);
            f->_ptr += n;
            f->_cnt -= (int)n;
            dst += n;
            left -= n;
        } else if (left >= (size_t)UCRT_INTERNAL_BUFSIZ && f->_base != (char *)&f->_charbuf && !(f->_flag & (UCRT_IOEOF | UCRT_IOSTRG))) {
            /* a big request with an empty buffer: read straight into the caller's memory */
            unsigned want = (unsigned)(left > 0x40000000u ? 0x40000000u : left);
            int n;
            if (ucrt_to_read(f))
                break;
            ucrt_fx(f)->has_rawstart = 0;
            n = ucrt_read(f->_file, dst, want);
            if (n <= 0) {
                f->_flag |= n == 0 ? UCRT_IOEOF : UCRT_IOERR;
                break;
            }
            dst += n;
            left -= (size_t)n;
        } else {
            int c = ucrt_filbuf_nl(f);
            if (c == UCRT_EOF)
                break;
            *dst++ = (char)c;
            --left;
        }
    }
    return size ? (total - left) / size : 0;
}

static inline size_t ucrt_fread(void *buffer, size_t size, size_t count, ucrt_file *f)
{
    size_t r;
    if (size == 0 || count == 0)
        return 0;
    if (!buffer || !UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    if (count > (size_t)-1 / size) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    ucrt_io_lock();
    r = ucrt_fread_nl(buffer, size, count, f);
    ucrt_io_unlock();
    return r;
}

static inline size_t ucrt_fwrite_nl(const void *buffer, size_t size, size_t count, ucrt_file *f)
{
    const char *src = (const char *)buffer;
    size_t total = size * count, left = total;
    if (ucrt_to_write(f))
        return 0;
    if (f->_flag & UCRT_IOSTRG)
        return 0;
    while (left) {
        if (f->_base != (char *)&f->_charbuf && f->_bufsiz > 1 && f->_ptr < f->_base + f->_bufsiz) {
            size_t room = (size_t)(f->_base + f->_bufsiz - f->_ptr), n = room < left ? room : left;
            __builtin_memcpy(f->_ptr, src, n);
            f->_ptr += n;
            f->_cnt = f->_bufsiz - (int)(f->_ptr - f->_base);
            src += n;
            left -= n;
        } else if (f->_ptr > f->_base && f->_base != (char *)&f->_charbuf) {
            if (ucrt_flush_nl(f))
                break;
        } else {
            /* nothing buffered and nothing to buffer into (unbuffered, or a request beyond the buffer):
             * write whole blocks directly */
            size_t n = left;
            int w;
            if (f->_base != (char *)&f->_charbuf && f->_bufsiz > 1 && left < (size_t)f->_bufsiz)
                continue;
            if (f->_base != (char *)&f->_charbuf && f->_bufsiz > 1)
                n = left - left % (size_t)f->_bufsiz;
            if (n > 0x40000000u)
                n = 0x40000000u;
            w = ucrt_write(f->_file, src, (unsigned)n);
            if (w < 0) {
                f->_flag |= UCRT_IOERR;
                break;
            }
            src += w;
            left -= (size_t)w;
            if ((size_t)w < n) {
                f->_flag |= UCRT_IOERR;
                break;
            }
        }
    }
    return size ? (total - left) / size : 0;
}

static inline size_t ucrt_fwrite(const void *buffer, size_t size, size_t count, ucrt_file *f)
{
    size_t r;
    if (size == 0 || count == 0)
        return 0;
    if (!buffer || !UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    if (count > (size_t)-1 / size) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    ucrt_io_lock();
    r = ucrt_fwrite_nl(buffer, size, count, f);
    ucrt_io_unlock();
    return r;
}

static inline char *ucrt_fgets(char *buf, int n, ucrt_file *f)
{
    int i = 0;
    if (!buf || n <= 0 || !UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    if (n == 1) {
        buf[0] = 0;
        return buf;
    }
    ucrt_io_lock();
    while (i < n - 1) {
        int c = ucrt_getc_nl(f);
        if (c == UCRT_EOF) {
            if (i == 0 || (f->_flag & UCRT_IOERR)) {
                ucrt_io_unlock();
                return NULL;
            }
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    buf[i] = 0;
    ucrt_io_unlock();
    return buf;
}

static inline int ucrt_fputs(const char *s, ucrt_file *f)
{
    size_t n;
    size_t w;
    if (!s || !UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    n = ucrt_strlen(s);
    ucrt_io_lock();
    w = n ? ucrt_fwrite_nl(s, 1, n, f) : 0;
    ucrt_io_unlock();
    return w == n ? 0 : UCRT_EOF;
}

static inline int ucrt_puts(const char *s)
{
    ucrt_file *out = ucrt_stdout();
    int r = 0;
    if (!s) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_io_lock();
    if (ucrt_fputs(s, out) == UCRT_EOF || ucrt_putc_nl('\n', out) == UCRT_EOF)
        r = UCRT_EOF;
    ucrt_io_unlock();
    return r < 0 ? r : 1;
}

static inline int ucrt_getchar(void)
{
    return ucrt_fgetc(ucrt_stdin());
}

static inline int ucrt_putchar(int c)
{
    return ucrt_fputc(c, ucrt_stdout());
}

/* gets_s: reads a line from stdin without the newline; a line longer than the buffer is an error. */
static inline char *ucrt_gets_s(char *buf, size_t size)
{
    ucrt_file *in = ucrt_stdin();
    size_t i = 0;
    int c;
    if (!buf || size == 0) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_io_lock();
    for (;;) {
        c = ucrt_getc_nl(in);
        if (c == UCRT_EOF) {
            if (i == 0) {
                buf[0] = 0;
                ucrt_io_unlock();
                return NULL;
            }
            break;
        }
        if (c == '\n')
            break;
        if (i + 1 >= size) {
            buf[0] = 0;
            ucrt_io_unlock();
            ucrt_invalid(UCRT_ERANGE);
            return NULL;
        }
        buf[i++] = (char)c;
    }
    buf[i] = 0;
    ucrt_io_unlock();
    return buf;
}

/* ---- Flushing, positioning, status ----------------------------------------------------------------------- */

static inline int ucrt_fflush_nl(ucrt_file *f)
{
    if (f->_flag & UCRT_IOWRT)
        return ucrt_flush_nl(f);
    if ((f->_flag & UCRT_IOREAD) && f->_cnt > 0 && f->_base && !(f->_flag & UCRT_IOSTRG)) {
        /* a read stream: drop the read-ahead and leave the file position at the program's position */
        int64_t p = ucrt_ftell_nl(f);
        if (p >= 0)
            ucrt_lseeki64(f->_file, p, UCRT_SEEK_SET);
        f->_cnt = 0;
        f->_ptr = f->_base;
    }
    return 0;
}

static inline int ucrt_fflush(ucrt_file *f)
{
    int r = 0;
    ucrt_stdio_init();
    ucrt_io_lock();
    if (!f) {
        for (int i = 0; i < UCRT_NSTREAM; ++i) {
            ucrt_file *s = &g_ucrt_stdio.iob[i];
            if ((s->_flag & UCRT_IOUSED) && (s->_flag & UCRT_IOWRT) && ucrt_flush_nl(s))
                r = UCRT_EOF;
        }
    } else if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        r = UCRT_EOF;
    } else {
        r = ucrt_fflush_nl(f);
    }
    ucrt_io_unlock();
    return r;
}

/* _flushall: flushes every stream and returns how many are open for reading or writing. */
static inline int ucrt_flushall(void)
{
    int n = 0;
    ucrt_stdio_init();
    ucrt_io_lock();
    for (int i = 0; i < UCRT_NSTREAM; ++i) {
        ucrt_file *s = &g_ucrt_stdio.iob[i];
        if (s->_flag & UCRT_IOUSED) {
            ++n;
            ucrt_fflush_nl(s);
        }
    }
    ucrt_io_unlock();
    return n;
}

static inline int64_t ucrt_ftelli64(ucrt_file *f)
{
    int64_t p;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_io_lock();
    p = ucrt_ftell_nl(f);
    ucrt_io_unlock();
    return p;
}

static inline int32_t ucrt_ftell(ucrt_file *f)
{
    int64_t p = ucrt_ftelli64(f);
    if (p > 0x7FFFFFFF) {
        ucrt_set_errno(UCRT_EINVAL);
        return -1;
    }
    return (int32_t)p;
}

static inline int ucrt_fseeki64(ucrt_file *f, int64_t off, int origin)
{
    int rc = 0;
    if (!UCRT_FILE_VALID(f) || origin < 0 || origin > 2) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_io_lock();
    if ((f->_flag & UCRT_IOWRT) && ucrt_flush_nl(f)) {
        ucrt_io_unlock();
        return -1;
    }
    if (origin == UCRT_SEEK_CUR) {
        int64_t p = ucrt_ftell_nl(f);
        if (p < 0) {
            ucrt_io_unlock();
            return -1;
        }
        off += p;
        origin = UCRT_SEEK_SET;
    }
    /* move the file first: a seek that fails must leave the stream as it was */
    if (ucrt_lseeki64(f->_file, off, origin) < 0) {
        ucrt_io_unlock();
        return -1;
    }
    f->_flag &= ~UCRT_IOEOF;
    if (f->_flag & UCRT_IORW)
        f->_flag &= ~(UCRT_IOREAD | UCRT_IOWRT);
    if (!(f->_flag & UCRT_IOSTRG) && f->_base) {
        f->_cnt = 0;
        f->_ptr = f->_base;
    }
    ucrt_fx(f)->wide_unget = -1;
    ucrt_fx(f)->has_rawstart = 0;
    ucrt_io_unlock();
    return rc;
}

static inline int ucrt_fseek(ucrt_file *f, int32_t off, int origin)
{
    return ucrt_fseeki64(f, off, origin);
}

static inline int ucrt_fgetpos(ucrt_file *f, int64_t *pos)
{
    int64_t p;
    if (!pos) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    p = ucrt_ftelli64(f);
    if (p < 0)
        return -1;
    *pos = p;
    return 0;
}

static inline int ucrt_fsetpos(ucrt_file *f, const int64_t *pos)
{
    if (!pos) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    return ucrt_fseeki64(f, *pos, UCRT_SEEK_SET);
}

static inline void ucrt_clearerr(ucrt_file *f)
{
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return;
    }
    f->_flag &= ~(UCRT_IOERR | UCRT_IOEOF);
    if (f->_file >= 0) {
        ucrt_fd *fd = ucrt_fd_get(f->_file);
        if (fd)
            fd->flags &= (unsigned char)~UCRT_FEOFLAG;
    }
}

static inline void ucrt_rewind(ucrt_file *f)
{
    ucrt_fseeki64(f, 0, UCRT_SEEK_SET);
    if (UCRT_FILE_VALID(f))
        ucrt_clearerr(f);
}

static inline int ucrt_feof(ucrt_file *f)
{
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    return (f->_flag & UCRT_IOEOF) != 0;
}

static inline int ucrt_ferror(ucrt_file *f)
{
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    return (f->_flag & UCRT_IOERR) != 0;
}

static inline int ucrt_fileno(ucrt_file *f)
{
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    return f->_file;
}

static inline int ucrt_setvbuf(ucrt_file *f, char *buf, int mode, size_t size)
{
    if (!UCRT_FILE_VALID(f) || (mode != UCRT_IOFBF && mode != UCRT_IOLBF && mode != UCRT_IONBF_MODE) ||
        (mode != UCRT_IONBF_MODE && (size < 2 || size > 0x7FFFFFFF))) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_io_lock();
    if (f->_base && f->_ptr != f->_base && (f->_flag & UCRT_IOWRT))
        ucrt_flush_nl(f);
    ucrt_freebuf(f);
    if (mode == UCRT_IONBF_MODE) {
        f->_flag |= UCRT_IONBF;
        f->_base = f->_ptr = (char *)&f->_charbuf;
        f->_bufsiz = 1;
    } else {
        if (!buf) {
            buf = (char *)ucrt_alloc(size);
            if (buf)
                f->_flag |= UCRT_IOMYBUF;
        } else {
            f->_flag |= UCRT_IOYOURBUF;
        }
        if (buf) {
            f->_base = f->_ptr = buf;
            f->_bufsiz = (int)size;
        } else {
            f->_flag |= UCRT_IONBF;
            f->_base = f->_ptr = (char *)&f->_charbuf;
            f->_bufsiz = 1;
        }
        f->_flag |= UCRT_IOSETVBUF;
    }
    f->_cnt = 0;
    ucrt_io_unlock();
    return 0;
}

static inline void ucrt_setbuf(ucrt_file *f, char *buf)
{
    if (buf)
        ucrt_setvbuf(f, buf, UCRT_IOFBF, UCRT_BUFSIZ);
    else
        ucrt_setvbuf(f, NULL, UCRT_IONBF_MODE, 0);
}

/* ---- Opening and closing streams --------------------------------------------------------------------------- */

static inline ucrt_file *ucrt_stream_alloc(void)
{
    ucrt_stdio_init();
    for (int i = 3; i < UCRT_NSTREAM; ++i) {
        ucrt_file *f = &g_ucrt_stdio.iob[i];
        if (!(f->_flag & UCRT_IOUSED)) {
            __builtin_memset(f, 0, sizeof(*f));
            f->_file = -1;
            f->_flag = UCRT_IOUSED;
            g_ucrt_stdio.x[i].wide_unget = -1;
            g_ucrt_stdio.x[i].cr_pending = -1;
            g_ucrt_stdio.x[i].has_rawstart = 0;
            g_ucrt_stdio.x[i].high = 0;
            return f;
        }
    }
    ucrt_set_errno(UCRT_EMFILE);
    return NULL;
}

/* What a mode string asks for. */
typedef struct ucrt_openmode {
    int oflag;
    int streamflag;
    int ccs; /* UCRT_O_WTEXT, UCRT_O_U8TEXT, UCRT_O_U16TEXT or 0 */
    int bom;
} ucrt_openmode;

static inline int ucrt_ccs_match(const char *s, const char *name)
{
    while (*name) {
        char a = *s++, b = *name++;
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 32);
        if (a != b)
            return 0;
    }
    return 1;
}

/* Parses "r", "w+b", "a,ccs=UTF-8" and the rest. Returns 0, or -1 for an invalid string. */
static inline int ucrt_parse_mode(const char *mode, ucrt_openmode *m)
{
    int plus = 0, text = 0, binary = 0, first;
    const char *p = mode;
    m->oflag = 0;
    m->streamflag = 0;
    m->ccs = 0;
    m->bom = 0;
    while (*p == ' ')
        ++p;
    first = *p;
    switch (first) {
    case 'r': m->oflag = UCRT_O_RDONLY; m->streamflag = UCRT_IOREAD; break;
    case 'w': m->oflag = UCRT_O_WRONLY | UCRT_O_CREAT | UCRT_O_TRUNC; m->streamflag = UCRT_IOWRT; break;
    case 'a': m->oflag = UCRT_O_WRONLY | UCRT_O_CREAT | UCRT_O_APPEND; m->streamflag = UCRT_IOWRT | UCRT_IOAPPEND; break;
    default: return -1;
    }
    for (++p; *p; ++p) {
        switch (*p) {
        case '+':
            if (plus)
                return -1;
            plus = 1;
            m->oflag = (m->oflag & ~3) | UCRT_O_RDWR;
            m->streamflag = (m->streamflag & UCRT_IOAPPEND) | UCRT_IORW;
            break;
        case 'b': if (text || binary) return -1; binary = 1; m->oflag |= UCRT_O_BINARY; break;
        case 't': if (text || binary) return -1; text = 1; m->oflag |= UCRT_O_TEXT; break;
        case 'c': case 'n': break; /* commit-to-disk flags: every write already reaches the OS */
        case 'N': m->oflag |= UCRT_O_NOINHERIT; break;
        case 'S': m->oflag |= UCRT_O_SEQUENTIAL; break;
        case 'R': m->oflag |= UCRT_O_RANDOM; break;
        case 'T': m->oflag |= UCRT_O_SHORT_LIVED; break;
        case 'D': m->oflag |= UCRT_O_TEMPORARY; break;
        case ' ': break;
        case ',':
            ++p;
            while (*p == ' ')
                ++p;
            if (!(p[0] == 'c' || p[0] == 'C') || !(p[1] == 'c' || p[1] == 'C') || !(p[2] == 's' || p[2] == 'S') || p[3] != '=')
                return -1;
            p += 4;
            if (ucrt_ccs_match(p, "UNICODE")) {
                m->ccs = UCRT_O_WTEXT;
                m->bom = 16;
                p += 6;
            } else if (ucrt_ccs_match(p, "UTF-8")) {
                m->ccs = UCRT_O_U8TEXT;
                m->bom = 8;
                p += 4;
            } else if (ucrt_ccs_match(p, "UTF-16LE")) {
                m->ccs = UCRT_O_U16TEXT;
                m->bom = 16;
                p += 7;
            } else {
                return -1;
            }
            if (text || binary)
                return -1;
            m->oflag &= ~(UCRT_O_TEXT | UCRT_O_BINARY);
            m->oflag |= m->ccs;
            while (p[1] == ' ')
                ++p;
            if (p[1])
                return -1;
            break;
        default: return -1;
        }
    }
    return 0;
}

/* Wraps descriptor fd in a new stream. */
static inline ucrt_file *ucrt_stream_from_fd(int fd, const ucrt_openmode *m)
{
    ucrt_file *f = ucrt_stream_alloc();
    if (!f)
        return NULL;
    f->_file = fd;
    f->_flag = UCRT_IOUSED | m->streamflag;
    return f;
}

static inline ucrt_file *ucrt_wfsopen(const ucrt_wchar *path, const char *mode, int shflag)
{
    ucrt_openmode m;
    int fd;
    ucrt_file *f;
    if (!path || !mode || ucrt_parse_mode(mode, &m)) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_io_lock();
    if (ucrt_wsopen_s(&fd, path, m.oflag, shflag, UCRT_S_IWRITE | UCRT_S_IREAD)) {
        ucrt_io_unlock();
        return NULL;
    }
    f = ucrt_stream_from_fd(fd, &m);
    if (!f) {
        ucrt_close(fd);
        ucrt_io_unlock();
        return NULL;
    }
    if (m.ccs) {
        ucrt_fd *d = ucrt_fd_get(fd);
        int64_t size = ucrt_filelengthi64(fd);
        if ((m.oflag & 3) != UCRT_O_RDONLY && size <= 0 && !(m.streamflag & UCRT_IOAPPEND && size > 0)) {
            /* a new file: begin it with a byte order mark (UTF-8 and UTF-16LE) */
            static const unsigned char bom8[3] = {0xEF, 0xBB, 0xBF}, bom16[2] = {0xFF, 0xFE};
            unsigned char saved = d->flags;
            d->flags &= (unsigned char)~UCRT_FTEXT;
            if (m.ccs == UCRT_O_U8TEXT)
                ucrt_write(fd, bom8, 3);
            else
                ucrt_write(fd, bom16, 2);
            d->flags = saved;
        } else if ((m.oflag & 3) != UCRT_O_WRONLY && size > 0) {
            /* an existing file: a byte order mark decides the encoding, and is skipped */
            unsigned char b[3] = {0, 0, 0};
            unsigned char saved = d->flags;
            int n;
            d->flags &= (unsigned char)~UCRT_FTEXT;
            n = ucrt_read(fd, b, 3);
            d->flags = saved;
            if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
                d->textmode = UCRT_TM_UTF8;
            } else if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
                d->textmode = UCRT_TM_UTF16;
                ucrt_lseeki64(fd, 2, UCRT_SEEK_SET);
            } else {
                ucrt_lseeki64(fd, 0, UCRT_SEEK_SET);
            }
            if (n >= 3 && d->textmode == UCRT_TM_UTF8)
                ucrt_lseeki64(fd, 3, UCRT_SEEK_SET);
        }
    }
    ucrt_io_unlock();
    return f;
}

static inline ucrt_file *ucrt_fsopen(const char *path, const char *mode, int shflag)
{
    ucrt_wchar w[UCRT_PATH_MAX];
    if (!path) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_widen_path(path, w, UCRT_PATH_MAX);
    return ucrt_wfsopen(w, mode, shflag);
}

static inline ucrt_file *ucrt_fopen(const char *path, const char *mode)
{
    return ucrt_fsopen(path, mode, UCRT_SH_DENYNO);
}

static inline ucrt_file *ucrt_wfopen(const ucrt_wchar *path, const ucrt_wchar *wmode)
{
    char mode[32];
    size_t i = 0;
    if (!wmode) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    for (; wmode[i] && i + 1 < sizeof(mode); ++i)
        mode[i] = (char)wmode[i];
    mode[i] = 0;
    return ucrt_wfsopen(path, mode, UCRT_SH_DENYNO);
}

static inline ucrt_errno_t ucrt_fopen_s(ucrt_file **out, const char *path, const char *mode)
{
    if (!out) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    *out = ucrt_fopen(path, mode);
    return *out ? 0 : ucrt_get_errno();
}

static inline ucrt_file *ucrt_fdopen(int fd, const char *mode)
{
    ucrt_openmode m;
    ucrt_file *f;
    if (!mode || ucrt_parse_mode(mode, &m) || !ucrt_fd_get(fd)) {
        if (mode && !ucrt_fd_get(fd))
            ucrt_set_errno(UCRT_EBADF);
        else
            ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_io_lock();
    f = ucrt_stream_from_fd(fd, &m);
    ucrt_io_unlock();
    return f;
}

static inline int ucrt_fclose_nl(ucrt_file *f)
{
    int r = 0, fd = f->_file;
    if (f->_flag & UCRT_IOWRT) {
        if (ucrt_flush_nl(f))
            r = UCRT_EOF;
    } else if ((f->_flag & UCRT_IOREAD) && f->_cnt > 0 && !(f->_flag & UCRT_IOSTRG)) {
        int64_t p = ucrt_ftell_nl(f);
        if (p >= 0)
            ucrt_lseeki64(fd, p, UCRT_SEEK_SET);
    }
    ucrt_freebuf(f);
    if (fd >= 0 && ucrt_close(fd))
        r = UCRT_EOF;
    if (f->_tmpfname) {
        ucrt_release(f->_tmpfname);
        f->_tmpfname = NULL;
    }
    f->_flag = 0;
    f->_file = -1;
    return r;
}

static inline int ucrt_fclose(ucrt_file *f)
{
    int r;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_io_lock();
    r = ucrt_fclose_nl(f);
    ucrt_io_unlock();
    return r;
}

/* _fcloseall: closes every stream but the standard three; returns how many. */
static inline int ucrt_fcloseall(void)
{
    int n = 0;
    ucrt_stdio_init();
    ucrt_io_lock();
    for (int i = 3; i < UCRT_NSTREAM; ++i) {
        ucrt_file *f = &g_ucrt_stdio.iob[i];
        if (f->_flag & UCRT_IOUSED) {
            if (ucrt_fclose_nl(f) == 0)
                ++n;
        }
    }
    ucrt_io_unlock();
    return n;
}

/* freopen: closes the stream's file and reopens the same FILE on another (or, with a NULL path, changes the mode). */
static inline ucrt_file *ucrt_freopen(const char *path, const char *mode, ucrt_file *f)
{
    ucrt_openmode m;
    int fd;
    ucrt_wchar w[UCRT_PATH_MAX];
    if (!path || !mode || !UCRT_FILE_VALID(f) || ucrt_parse_mode(mode, &m)) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_io_lock();
    ucrt_fflush_nl(f);
    ucrt_freebuf(f);
    if (f->_file >= 0)
        ucrt_close(f->_file);
    ucrt_widen_path(path, w, UCRT_PATH_MAX);
    if (ucrt_wsopen_s(&fd, w, m.oflag, UCRT_SH_DENYNO, UCRT_S_IWRITE | UCRT_S_IREAD)) {
        f->_flag = 0;
        f->_file = -1;
        ucrt_io_unlock();
        return NULL;
    }
    f->_file = fd;
    f->_flag = UCRT_IOUSED | m.streamflag;
    f->_cnt = 0;
    ucrt_fx(f)->wide_unget = -1;
    ucrt_fx(f)->has_rawstart = 0;
    ucrt_io_unlock();
    return f;
}

/* ---- Wide characters ---------------------------------------------------------------------------------- */

/* Writes the encoded form of one wide character to a stream: two bytes for a binary or UTF-16 stream, UTF-8 for a
 * UTF-8 stream (pairing surrogates), a single byte for an ANSI text stream in the "C" locale. */
static inline int ucrt_fputwc_nl(ucrt_wchar c, ucrt_file *f)
{
    ucrt_fd *fd = ucrt_fd_get(f->_file);
    int mode = (fd && (fd->flags & UCRT_FTEXT)) ? fd->textmode : -1; /* -1: binary */
    ucrt_file_x *x = ucrt_fx(f);
    if (mode == UCRT_TM_ANSI) {
        if (c > 0xFF) {
            ucrt_set_errno(UCRT_EILSEQ);
            return UCRT_WEOF_INT;
        }
        return ucrt_putc_nl((int)c, f) == UCRT_EOF ? UCRT_WEOF_INT : (int)c;
    }
    if (mode == UCRT_TM_UTF8) {
        unsigned cp = c;
        unsigned char b[4];
        int n;
        if (c == '\n' && ucrt_putc_nl('\r', f) == UCRT_EOF)
            return UCRT_WEOF_INT;
        if (c >= 0xD800 && c <= 0xDBFF) {
            x->high = c;
            return c;
        }
        if (c >= 0xDC00 && c <= 0xDFFF && x->high) {
            cp = 0x10000 + (((unsigned)x->high - 0xD800) << 10) + (c - 0xDC00);
            x->high = 0;
        }
        if (cp < 0x80) {
            b[0] = (unsigned char)cp;
            n = 1;
        } else if (cp < 0x800) {
            b[0] = (unsigned char)(0xC0 | (cp >> 6));
            b[1] = (unsigned char)(0x80 | (cp & 0x3F));
            n = 2;
        } else if (cp < 0x10000) {
            b[0] = (unsigned char)(0xE0 | (cp >> 12));
            b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            b[2] = (unsigned char)(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            b[0] = (unsigned char)(0xF0 | (cp >> 18));
            b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            b[3] = (unsigned char)(0x80 | (cp & 0x3F));
            n = 4;
        }
        for (int i = 0; i < n; ++i)
            if (ucrt_putc_nl(b[i], f) == UCRT_EOF)
                return UCRT_WEOF_INT;
        return c;
    }
    /* binary or UTF-16: two bytes, little endian; text mode puts CR before LF */
    if (mode == UCRT_TM_UTF16 && c == '\n') {
        if (ucrt_putc_nl('\r', f) == UCRT_EOF || ucrt_putc_nl(0, f) == UCRT_EOF)
            return UCRT_WEOF_INT;
    }
    if (ucrt_putc_nl(c & 0xFF, f) == UCRT_EOF || ucrt_putc_nl((c >> 8) & 0xFF, f) == UCRT_EOF)
        return UCRT_WEOF_INT;
    return c;
}

static inline int ucrt_fgetwc_nl(ucrt_file *f)
{
    ucrt_fd *fd = ucrt_fd_get(f->_file);
    int mode = (fd && (fd->flags & UCRT_FTEXT)) ? fd->textmode : -1;
    ucrt_file_x *x = ucrt_fx(f);
    int a, b;
    if (x->wide_unget >= 0) {
        int c = x->wide_unget;
        x->wide_unget = -1;
        return c;
    }
    if (mode == UCRT_TM_ANSI) {
        a = ucrt_getc_nl(f);
        return a == UCRT_EOF ? UCRT_WEOF_INT : a;
    }
    if (mode == UCRT_TM_UTF8) {
        a = ucrt_getc_nl(f);
        if (a == UCRT_EOF)
            return UCRT_WEOF_INT;
        if (a == '\r') {
            int n = ucrt_getc_nl(f);
            if (n == '\n')
                return '\n';
            if (n != UCRT_EOF)
                ucrt_ungetc(n, f);
            return '\r';
        }
        if (a < 0x80)
            return a;
        {
            int extra = a >= 0xF0 ? 3 : a >= 0xE0 ? 2 : a >= 0xC0 ? 1 : -1;
            unsigned cp = extra == 3 ? (unsigned)(a & 7) : extra == 2 ? (unsigned)(a & 15) : (unsigned)(a & 31);
            if (extra < 0) {
                ucrt_set_errno(UCRT_EILSEQ);
                return UCRT_WEOF_INT;
            }
            for (int i = 0; i < extra; ++i) {
                int c = ucrt_getc_nl(f);
                if (c == UCRT_EOF || (c & 0xC0) != 0x80) {
                    ucrt_set_errno(UCRT_EILSEQ);
                    return UCRT_WEOF_INT;
                }
                cp = (cp << 6) | (unsigned)(c & 0x3F);
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                x->wide_unget = (int)(0xDC00 + (cp & 0x3FF));
                return (int)(0xD800 + (cp >> 10));
            }
            return (int)cp;
        }
    }
    a = ucrt_getc_nl(f);
    if (a == UCRT_EOF)
        return UCRT_WEOF_INT;
    b = ucrt_getc_nl(f);
    if (b == UCRT_EOF)
        return UCRT_WEOF_INT;
    a |= b << 8;
    if (mode == UCRT_TM_UTF16 && a == '\r') {
        int a2 = ucrt_getc_nl(f), b2 = a2 == UCRT_EOF ? UCRT_EOF : ucrt_getc_nl(f);
        if (a2 == '\n' && b2 == 0)
            return '\n';
        if (b2 != UCRT_EOF) {
            ucrt_ungetc(b2, f);
            ucrt_ungetc(a2, f);
        }
    }
    return a;
}

static inline int ucrt_fputwc(ucrt_wchar c, ucrt_file *f)
{
    int r;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_WEOF_INT;
    }
    ucrt_io_lock();
    r = ucrt_fputwc_nl(c, f);
    ucrt_io_unlock();
    return r;
}

static inline int ucrt_fgetwc(ucrt_file *f)
{
    int r;
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_WEOF_INT;
    }
    ucrt_io_lock();
    r = ucrt_fgetwc_nl(f);
    ucrt_io_unlock();
    return r;
}

static inline int ucrt_ungetwc(ucrt_wchar c, ucrt_file *f)
{
    if (!UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_WEOF_INT;
    }
    if (c == UCRT_WEOF)
        return UCRT_WEOF_INT;
    ucrt_fx(f)->wide_unget = c;
    f->_flag &= ~UCRT_IOEOF;
    return c;
}

static inline int ucrt_fputws(const ucrt_wchar *s, ucrt_file *f)
{
    if (!s || !UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EOF;
    }
    ucrt_io_lock();
    for (; *s; ++s)
        if (ucrt_fputwc_nl(*s, f) == UCRT_WEOF_INT) {
            ucrt_io_unlock();
            return UCRT_EOF;
        }
    ucrt_io_unlock();
    return 0;
}

static inline ucrt_wchar *ucrt_fgetws(ucrt_wchar *buf, int n, ucrt_file *f)
{
    int i = 0;
    if (!buf || n <= 0 || !UCRT_FILE_VALID(f)) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_io_lock();
    while (i < n - 1) {
        int c = ucrt_fgetwc_nl(f);
        if (c == UCRT_WEOF_INT) {
            if (i == 0) {
                ucrt_io_unlock();
                return NULL;
            }
            break;
        }
        buf[i++] = (ucrt_wchar)c;
        if (c == '\n')
            break;
    }
    buf[i] = 0;
    ucrt_io_unlock();
    return buf;
}

/* ---- printf to streams -------------------------------------------------------------------------------- */

typedef struct ucrt_stream_sink {
    ucrt_pf_sink sink;
    ucrt_file *f;
} ucrt_stream_sink;

static inline void ucrt_stream_sink_put_n(void *ctx, const char *s, size_t n)
{
    ucrt_stream_sink *k = (ucrt_stream_sink *)ctx;
    if (n && !k->sink.error && ucrt_fwrite_nl(s, 1, n, k->f) != n)
        k->sink.error = 1;
}

static inline void ucrt_stream_sink_put_w(void *ctx, const ucrt_wchar *s, size_t n)
{
    ucrt_stream_sink *k = (ucrt_stream_sink *)ctx;
    for (size_t i = 0; i < n && !k->sink.error; ++i)
        if (ucrt_fputwc_nl(s[i], k->f) == UCRT_WEOF_INT)
            k->sink.error = 1;
}

static inline int ucrt_vfprintf(ucrt_file *f, const char *fmt, va_list ap)
{
    ucrt_stream_sink k;
    int r;
    if (!UCRT_FILE_VALID(f) || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    k.f = f;
    k.sink.ctx = &k;
    k.sink.put_n = ucrt_stream_sink_put_n;
    k.sink.put_w = ucrt_stream_sink_put_w;
    k.sink.count = 0;
    k.sink.error = 0;
    ucrt_io_lock();
    if (ucrt_to_write(f)) {
        ucrt_io_unlock();
        return -1;
    }
    r = ucrt_pf_vformat(&k.sink, fmt, ap);
    ucrt_io_unlock();
    return k.sink.error ? -1 : r;
}

static inline int ucrt_fprintf(ucrt_file *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_vprintf(const char *fmt, va_list ap)
{
    return ucrt_vfprintf(ucrt_stdout(), fmt, ap);
}

static inline int ucrt_printf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfprintf(ucrt_stdout(), fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_vfwprintf(ucrt_file *f, const ucrt_wchar *fmt, va_list ap)
{
    ucrt_stream_sink k;
    int r;
    if (!UCRT_FILE_VALID(f) || !fmt) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    k.f = f;
    k.sink.ctx = &k;
    k.sink.put_n = ucrt_stream_sink_put_n;
    k.sink.put_w = ucrt_stream_sink_put_w;
    k.sink.count = 0;
    k.sink.error = 0;
    ucrt_io_lock();
    if (ucrt_to_write(f)) {
        ucrt_io_unlock();
        return -1;
    }
    r = ucrt_pfw_vformat(&k.sink, fmt, ap);
    ucrt_io_unlock();
    return k.sink.error ? -1 : r;
}

static inline int ucrt_fwprintf(ucrt_file *f, const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfwprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

static inline int ucrt_wprintf(const ucrt_wchar *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ucrt_vfwprintf(ucrt_stdout(), fmt, ap);
    va_end(ap);
    return r;
}

#endif /* UCRT_STDIO_H */
