/*
 * ucrt_io.h - low-level file descriptors: _open, _read, _write, _lseek and friends, with the text-mode
 * translation of line ends and the Ctrl-Z end-of-file convention. Built on the table in ucrt_os.h.
 * Host tests: tests/crt-host/test_io.c, with a POSIX backend.
 */
#ifndef UCRT_IO_H
#define UCRT_IO_H

#include "ucrt_os.h"

/* _open flags */
#define UCRT_O_RDONLY 0x0000
#define UCRT_O_WRONLY 0x0001
#define UCRT_O_RDWR 0x0002
#define UCRT_O_APPEND 0x0008
#define UCRT_O_RANDOM 0x0010
#define UCRT_O_SEQUENTIAL 0x0020
#define UCRT_O_TEMPORARY 0x0040
#define UCRT_O_NOINHERIT 0x0080
#define UCRT_O_CREAT 0x0100
#define UCRT_O_TRUNC 0x0200
#define UCRT_O_EXCL 0x0400
#define UCRT_O_SHORT_LIVED 0x1000
#define UCRT_O_TEXT 0x4000
#define UCRT_O_BINARY 0x8000
#define UCRT_O_WTEXT 0x10000
#define UCRT_O_U16TEXT 0x20000
#define UCRT_O_U8TEXT 0x40000

/* _sopen share modes */
#define UCRT_SH_DENYRW 0x10
#define UCRT_SH_DENYWR 0x20
#define UCRT_SH_DENYRD 0x30
#define UCRT_SH_DENYNO 0x40

#define UCRT_S_IWRITE 0x0080
#define UCRT_S_IREAD 0x0100

#define UCRT_NFILE 512
#define UCRT_CTRL_Z 0x1A

/* per-descriptor flags */
#define UCRT_FOPEN 0x01
#define UCRT_FEOFLAG 0x02
#define UCRT_FCRLF 0x04
#define UCRT_FPIPE 0x08
#define UCRT_FNOINHERIT 0x10
#define UCRT_FAPPEND 0x20
#define UCRT_FDEV 0x40
#define UCRT_FTEXT 0x80

/* text-mode flavours kept beside the flags: ANSI text, UTF-16 text, UTF-8 text */
#define UCRT_TM_ANSI 0
#define UCRT_TM_UTF16 1
#define UCRT_TM_UTF8 2

typedef struct ucrt_fd {
    void *handle;
    unsigned char flags;
    unsigned char textmode; /* UCRT_TM_* when FTEXT is set */
    int peek;               /* a byte read ahead on a pipe or device, or -1 */
} ucrt_fd;

typedef struct ucrt_io_state {
    ucrt_fd fds[UCRT_NFILE];
    int fmode; /* the default translation: UCRT_O_TEXT or UCRT_O_BINARY */
    int initialised;
} ucrt_io_state;

static ucrt_io_state g_ucrt_io = {{{0, 0, 0, -1}}, UCRT_O_TEXT, 0};

static inline ucrt_fd *ucrt_fd_get(int fd)
{
    if (fd < 0 || fd >= UCRT_NFILE || !(g_ucrt_io.fds[fd].flags & UCRT_FOPEN))
        return NULL;
    return &g_ucrt_io.fds[fd];
}

/* Descriptors 0, 1 and 2 start out as the process's standard handles. */
static inline void ucrt_io_init(void)
{
    if (g_ucrt_io.initialised)
        return;
    g_ucrt_io.initialised = 1;
    for (int i = 0; i < UCRT_NFILE; ++i)
        g_ucrt_io.fds[i].peek = -1;
    if (!g_ucrt_os || !g_ucrt_os->get_std)
        return;
    for (unsigned i = 0; i < 3; ++i) {
        void *h = NULL;
        if (g_ucrt_os->get_std(i, &h) == 0 && h) {
            unsigned type = g_ucrt_os->filetype ? g_ucrt_os->filetype(h) : UCRT_FILE_TYPE_UNKNOWN;
            ucrt_fd *f = &g_ucrt_io.fds[i];
            f->handle = h;
            f->flags = UCRT_FOPEN | UCRT_FTEXT;
            if (type == UCRT_FILE_TYPE_CHAR)
                f->flags |= UCRT_FDEV;
            else if (type == UCRT_FILE_TYPE_PIPE)
                f->flags |= UCRT_FPIPE;
            f->textmode = UCRT_TM_ANSI;
        }
    }
}

static inline int ucrt_fd_alloc(void)
{
    for (int i = 0; i < UCRT_NFILE; ++i)
        if (!(g_ucrt_io.fds[i].flags & UCRT_FOPEN))
            return i;
    return -1;
}

/* ---- Opening and closing ----------------------------------------------------------------------------- */

/* _wsopen_s: the common implementation. *pfh receives the descriptor. Returns 0 or an errno value. */
static inline ucrt_errno_t ucrt_wsopen_s(int *pfh, const ucrt_wchar *path, int oflag, int shflag, int pmode)
{
    unsigned access, share, disp, attrs = UCRT_ATTR_NORMAL, err;
    void *h = NULL;
    int fd, text;
    unsigned char fl;
    ucrt_fd *f;
    if (!pfh)
        return ucrt_invalid(UCRT_EINVAL);
    *pfh = -1;
    if (!path)
        return ucrt_invalid(UCRT_EINVAL);
    if (!g_ucrt_os)
        return UCRT_ENOSYS;
    ucrt_io_lock();
    ucrt_io_init();
    switch (oflag & 3) {
    case UCRT_O_RDONLY: access = UCRT_GENERIC_READ; break;
    case UCRT_O_WRONLY: access = UCRT_GENERIC_WRITE; break;
    case UCRT_O_RDWR: access = UCRT_GENERIC_READ | UCRT_GENERIC_WRITE; break;
    default:
        ucrt_io_unlock();
        return ucrt_invalid(UCRT_EINVAL);
    }
    switch (shflag & 0x70) {
    case 0: /* the default for _open is _SH_DENYNO */
    case UCRT_SH_DENYNO: share = UCRT_FILE_SHARE_READ | UCRT_FILE_SHARE_WRITE; break;
    case UCRT_SH_DENYRW: share = 0; break;
    case UCRT_SH_DENYWR: share = UCRT_FILE_SHARE_READ; break;
    case UCRT_SH_DENYRD: share = UCRT_FILE_SHARE_WRITE; break;
    default:
        ucrt_io_unlock();
        return ucrt_invalid(UCRT_EINVAL);
    }
    switch (oflag & (UCRT_O_CREAT | UCRT_O_EXCL | UCRT_O_TRUNC)) {
    case 0:
    case UCRT_O_EXCL: disp = UCRT_OPEN_EXISTING; break;
    case UCRT_O_CREAT: disp = UCRT_OPEN_ALWAYS; break;
    case UCRT_O_CREAT | UCRT_O_EXCL:
    case UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_EXCL: disp = UCRT_CREATE_NEW; break;
    case UCRT_O_TRUNC:
    case UCRT_O_TRUNC | UCRT_O_EXCL: disp = UCRT_TRUNCATE_EXISTING; break;
    default: disp = UCRT_CREATE_ALWAYS; break; /* CREAT | TRUNC */
    }
    if ((oflag & UCRT_O_CREAT) && !(pmode & UCRT_S_IWRITE))
        attrs = UCRT_ATTR_READONLY;
    if (oflag & UCRT_O_TEMPORARY) {
        attrs |= UCRT_FLAG_DELETE_ON_CLOSE;
        access |= 0x00010000u; /* DELETE */
        share |= 4u;           /* FILE_SHARE_DELETE */
    }
    if (oflag & UCRT_O_SHORT_LIVED)
        attrs |= UCRT_ATTR_TEMPORARY;
    fd = ucrt_fd_alloc();
    if (fd < 0) {
        ucrt_set_errno(UCRT_EMFILE);
        ucrt_io_unlock();
        return UCRT_EMFILE;
    }
    err = g_ucrt_os->open(path, access, share, disp, attrs, &h);
    if (err) {
        ucrt_dosmaperr(err);
        ucrt_io_unlock();
        return ucrt_get_errno();
    }
    f = &g_ucrt_io.fds[fd];
    f->handle = h;
    f->peek = -1;
    fl = UCRT_FOPEN;
    {
        unsigned type = g_ucrt_os->filetype ? g_ucrt_os->filetype(h) : UCRT_FILE_TYPE_DISK;
        if (type == UCRT_FILE_TYPE_CHAR)
            fl |= UCRT_FDEV;
        else if (type == UCRT_FILE_TYPE_PIPE)
            fl |= UCRT_FPIPE;
    }
    if (oflag & UCRT_O_NOINHERIT)
        fl |= UCRT_FNOINHERIT;
    if (oflag & UCRT_O_APPEND)
        fl |= UCRT_FAPPEND;
    text = 0;
    f->textmode = UCRT_TM_ANSI;
    if (oflag & (UCRT_O_WTEXT | UCRT_O_U16TEXT | UCRT_O_U8TEXT)) {
        text = 1;
        f->textmode = (oflag & UCRT_O_U8TEXT) ? UCRT_TM_UTF8 : UCRT_TM_UTF16;
    } else if (oflag & UCRT_O_BINARY) {
        text = 0;
    } else if (oflag & UCRT_O_TEXT) {
        text = 1;
    } else {
        text = g_ucrt_io.fmode == UCRT_O_TEXT;
    }
    if (text)
        fl |= UCRT_FTEXT;
    f->flags = fl;
    /* A text file opened to append loses a trailing Ctrl-Z, so the new data follows the real end. */
    if (text && (oflag & UCRT_O_APPEND) && (access & UCRT_GENERIC_WRITE) && (access & UCRT_GENERIC_READ) &&
        !(fl & (UCRT_FDEV | UCRT_FPIPE)) && g_ucrt_os->seek && g_ucrt_os->set_eof) {
        int64_t end = 0, p;
        if (g_ucrt_os->seek(h, 0, 2, &end) == 0 && end > 0 && g_ucrt_os->seek(h, end - 1, 0, &p) == 0) {
            unsigned char c = 0;
            unsigned got = 0;
            if (g_ucrt_os->read(h, &c, 1, &got) == 0 && got == 1 && c == UCRT_CTRL_Z)
                g_ucrt_os->set_eof(h, end - 1);
            g_ucrt_os->seek(h, 0, 0, &p);
        }
    }
    *pfh = fd;
    ucrt_io_unlock();
    return 0;
}

/* A narrow path becomes a wide one through the OS layer's code page; the default maps bytes to characters. */
static inline size_t ucrt_widen_path(const char *s, ucrt_wchar *out, size_t cap)
{
    size_t i = 0;
    for (; s[i] && i + 1 < cap; ++i)
        out[i] = (ucrt_wchar)(unsigned char)s[i];
    out[i] = 0;
    return i;
}

#define UCRT_PATH_MAX 520

static inline ucrt_errno_t ucrt_sopen_s(int *pfh, const char *path, int oflag, int shflag, int pmode)
{
    ucrt_wchar w[UCRT_PATH_MAX];
    if (!path) {
        if (pfh)
            *pfh = -1;
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (ucrt_widen_path(path, w, UCRT_PATH_MAX) >= UCRT_PATH_MAX - 1 && path[UCRT_PATH_MAX - 1]) {
        if (pfh)
            *pfh = -1;
        ucrt_set_errno(UCRT_ENAMETOOLONG);
        return UCRT_ENAMETOOLONG;
    }
    return ucrt_wsopen_s(pfh, w, oflag, shflag, pmode);
}

static inline int ucrt_open(const char *path, int oflag, int pmode)
{
    int fd;
    return ucrt_sopen_s(&fd, path, oflag, UCRT_SH_DENYNO, pmode) ? -1 : fd;
}

static inline int ucrt_wopen(const ucrt_wchar *path, int oflag, int pmode)
{
    int fd;
    return ucrt_wsopen_s(&fd, path, oflag, UCRT_SH_DENYNO, pmode) ? -1 : fd;
}

static inline int ucrt_creat(const char *path, int pmode)
{
    return ucrt_open(path, UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_RDWR, pmode);
}

static inline int ucrt_close(int fd)
{
    ucrt_fd *f;
    unsigned err = 0;
    ucrt_io_lock();
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        ucrt_io_unlock();
        return -1;
    }
    /* the three standard descriptors may share one handle after a dup2; close it only once */
    if (g_ucrt_os && g_ucrt_os->close)
        err = g_ucrt_os->close(f->handle);
    f->handle = NULL;
    f->flags = 0;
    f->peek = -1;
    ucrt_io_unlock();
    if (err) {
        ucrt_dosmaperr(err);
        return -1;
    }
    return 0;
}

/* ---- Reading --------------------------------------------------------------------------------------- */

static inline int ucrt_read(int fd, void *buffer, unsigned count)
{
    ucrt_fd *f;
    unsigned char *buf = (unsigned char *)buffer;
    unsigned raw = 0, got = 0, err;
    int out = 0;
    ucrt_io_lock();
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        ucrt_set_doserrno(0);
        ucrt_io_unlock();
        return -1;
    }
    if (count > 0x7FFFFFFFu || !buf) {
        if (count == 0) {
            ucrt_io_unlock();
            return 0;
        }
        ucrt_io_unlock();
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    if (count == 0 || (f->flags & UCRT_FEOFLAG)) {
        ucrt_io_unlock();
        return 0;
    }
    if ((f->flags & UCRT_FTEXT) && f->textmode != UCRT_TM_ANSI && (count & 1)) {
        ucrt_io_unlock();
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    if (f->peek >= 0) { /* a byte read ahead by an earlier call */
        buf[0] = (unsigned char)f->peek;
        f->peek = -1;
        raw = 1;
        if (count > 1 && !(f->flags & UCRT_FDEV)) {
            err = g_ucrt_os->read(f->handle, buf + 1, count - 1, &got);
            if (err == 0)
                raw += got;
        }
    } else {
        err = g_ucrt_os->read(f->handle, buf, count, &raw);
        if (err) {
            if (err == UCRT_ERROR_BROKEN_PIPE || err == UCRT_ERROR_HANDLE_EOF) {
                ucrt_io_unlock();
                return 0; /* the write end closed: end of file */
            }
            if (err == UCRT_ERROR_ACCESS_DENIED) {
                ucrt_set_errno(UCRT_EBADF);
                ucrt_set_doserrno(err);
            } else {
                ucrt_dosmaperr(err);
            }
            ucrt_io_unlock();
            return -1;
        }
    }
    if (raw == 0) {
        ucrt_io_unlock();
        return 0;
    }
    if (!(f->flags & UCRT_FTEXT) || f->textmode != UCRT_TM_ANSI) {
        ucrt_io_unlock();
        return (int)raw; /* binary and wide text modes are not translated here */
    }
    {
        unsigned char *p = buf, *q = buf, *end = buf + raw;
        while (p < end) {
            unsigned char c = *p;
            if (c == UCRT_CTRL_Z) {
                if (!(f->flags & UCRT_FDEV))
                    f->flags |= UCRT_FEOFLAG;
                break;
            }
            if (c == '\r') {
                if (p + 1 < end) {
                    if (p[1] == '\n') {
                        *q++ = '\n';
                        p += 2;
                    } else {
                        *q++ = '\r';
                        ++p;
                    }
                } else {
                    /* a CR at the very end: look at the next byte to know whether it is half of a CRLF */
                    unsigned char c2 = 0;
                    unsigned n2 = 0;
                    ++p;
                    if (g_ucrt_os->read(f->handle, &c2, 1, &n2) == 0 && n2 == 1) {
                        if (c2 == '\n') {
                            *q++ = '\n';
                        } else {
                            *q++ = '\r';
                            if (f->flags & (UCRT_FDEV | UCRT_FPIPE)) {
                                f->peek = c2;
                            } else {
                                int64_t pos;
                                g_ucrt_os->seek(f->handle, -1, 1, &pos);
                            }
                        }
                    } else {
                        *q++ = '\r';
                    }
                }
            } else {
                *q++ = c;
                ++p;
            }
        }
        out = (int)(q - buf);
    }
    ucrt_io_unlock();
    return out;
}

/* ---- Writing --------------------------------------------------------------------------------------- */

static inline int ucrt_write(int fd, const void *buffer, unsigned count)
{
    ucrt_fd *f;
    const unsigned char *buf = (const unsigned char *)buffer;
    unsigned err, wrote = 0;
    ucrt_io_lock();
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        ucrt_set_doserrno(0);
        ucrt_io_unlock();
        return -1;
    }
    if (count == 0) {
        ucrt_io_unlock();
        return 0;
    }
    if (!buf || count > 0x7FFFFFFFu || ((f->flags & UCRT_FTEXT) && f->textmode != UCRT_TM_ANSI && (count & 1))) {
        ucrt_io_unlock();
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    if (f->flags & UCRT_FAPPEND) {
        int64_t pos;
        g_ucrt_os->seek(f->handle, 0, 2, &pos);
    }
    if (!(f->flags & UCRT_FTEXT) || f->textmode != UCRT_TM_ANSI) {
        err = g_ucrt_os->write(f->handle, buf, count, &wrote);
        if (err) {
            ucrt_dosmaperr(err);
            ucrt_io_unlock();
            return -1;
        }
        if (wrote < count) {
            ucrt_set_errno(UCRT_ENOSPC);
            ucrt_set_doserrno(0);
            ucrt_io_unlock();
            return -1;
        }
        ucrt_io_unlock();
        return (int)wrote;
    }
    {
        /* text mode: every LF becomes CRLF; translate in chunks */
        unsigned char tmp[2048];
        unsigned done = 0;
        while (done < count) {
            unsigned n = 0, used = 0;
            while (done + used < count && n + 2 <= sizeof(tmp)) {
                unsigned char c = buf[done + used++];
                if (c == '\n')
                    tmp[n++] = '\r';
                tmp[n++] = c;
            }
            err = g_ucrt_os->write(f->handle, tmp, n, &wrote);
            if (err) {
                ucrt_dosmaperr(err);
                ucrt_io_unlock();
                return done ? (int)done : -1;
            }
            if (wrote < n) {
                ucrt_set_errno(UCRT_ENOSPC);
                ucrt_io_unlock();
                return -1;
            }
            done += used;
        }
    }
    ucrt_io_unlock();
    return (int)count;
}

/* ---- Positioning -------------------------------------------------------------------------------------- */

static inline int64_t ucrt_lseeki64(int fd, int64_t off, int origin)
{
    ucrt_fd *f;
    int64_t pos = 0;
    unsigned err;
    ucrt_io_lock();
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        ucrt_io_unlock();
        return -1;
    }
    if (origin < 0 || origin > 2) {
        ucrt_io_unlock();
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    f->peek = -1;
    err = g_ucrt_os->seek(f->handle, off, origin, &pos);
    if (err) {
        ucrt_dosmaperr(err);
        ucrt_io_unlock();
        return -1;
    }
    f->flags &= (unsigned char)~UCRT_FEOFLAG;
    ucrt_io_unlock();
    return pos;
}

static inline int32_t ucrt_lseek(int fd, int32_t off, int origin)
{
    int64_t r = ucrt_lseeki64(fd, off, origin);
    if (r < 0)
        return -1;
    if (r > 0x7FFFFFFF) {
        ucrt_set_errno(UCRT_EINVAL);
        return -1;
    }
    return (int32_t)r;
}

static inline int64_t ucrt_telli64(int fd)
{
    return ucrt_lseeki64(fd, 0, 1);
}

static inline int32_t ucrt_tell(int fd)
{
    return ucrt_lseek(fd, 0, 1);
}

static inline int64_t ucrt_filelengthi64(int fd)
{
    int64_t cur = ucrt_lseeki64(fd, 0, 1), end;
    if (cur < 0)
        return -1;
    end = ucrt_lseeki64(fd, 0, 2);
    if (end < 0)
        return -1;
    ucrt_lseeki64(fd, cur, 0);
    return end;
}

static inline int32_t ucrt_filelength(int fd)
{
    int64_t n = ucrt_filelengthi64(fd);
    if (n < 0)
        return -1;
    if (n > 0x7FFFFFFF) {
        ucrt_set_errno(UCRT_EINVAL);
        return -1;
    }
    return (int32_t)n;
}

/* 1 at the end of the file, 0 elsewhere, -1 for a bad descriptor. */
static inline int ucrt_eof(int fd)
{
    int64_t cur, end;
    if (!ucrt_fd_get(fd)) {
        ucrt_set_errno(UCRT_EBADF);
        return -1;
    }
    cur = ucrt_lseeki64(fd, 0, 1);
    if (cur < 0)
        return -1;
    end = ucrt_lseeki64(fd, 0, 2);
    if (end < 0)
        return -1;
    if (cur != end)
        ucrt_lseeki64(fd, cur, 0);
    return cur == end;
}

/* ---- Miscellaneous descriptor operations ----------------------------------------------------------------- */

static inline int ucrt_isatty(int fd)
{
    ucrt_fd *f;
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        return 0;
    }
    return (f->flags & UCRT_FDEV) != 0;
}

/* _setmode returns the previous mode, _O_TEXT or _O_BINARY (or a wide mode). */
static inline int ucrt_setmode(int fd, int mode)
{
    ucrt_fd *f;
    int old;
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        return -1;
    }
    if (mode != UCRT_O_TEXT && mode != UCRT_O_BINARY && mode != UCRT_O_WTEXT && mode != UCRT_O_U16TEXT && mode != UCRT_O_U8TEXT) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    if (!(f->flags & UCRT_FTEXT))
        old = UCRT_O_BINARY;
    else if (f->textmode == UCRT_TM_UTF16)
        old = UCRT_O_WTEXT;
    else if (f->textmode == UCRT_TM_UTF8)
        old = UCRT_O_U8TEXT;
    else
        old = UCRT_O_TEXT;
    if (mode == UCRT_O_BINARY) {
        f->flags &= (unsigned char)~UCRT_FTEXT;
    } else {
        f->flags |= UCRT_FTEXT;
        f->textmode = mode == UCRT_O_TEXT ? UCRT_TM_ANSI : mode == UCRT_O_U8TEXT ? UCRT_TM_UTF8 : UCRT_TM_UTF16;
    }
    return old;
}

static inline void *ucrt_get_osfhandle(int fd)
{
    ucrt_fd *f;
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        return (void *)(intptr_t)-1;
    }
    return f->handle;
}

static inline int ucrt_open_osfhandle(void *handle, int flags)
{
    int fd;
    ucrt_fd *f;
    ucrt_io_lock();
    ucrt_io_init();
    fd = ucrt_fd_alloc();
    if (fd < 0) {
        ucrt_set_errno(UCRT_EMFILE);
        ucrt_io_unlock();
        return -1;
    }
    f = &g_ucrt_io.fds[fd];
    f->handle = handle;
    f->peek = -1;
    f->flags = UCRT_FOPEN;
    f->textmode = UCRT_TM_ANSI;
    if (g_ucrt_os && g_ucrt_os->filetype) {
        unsigned t = g_ucrt_os->filetype(handle);
        if (t == UCRT_FILE_TYPE_CHAR)
            f->flags |= UCRT_FDEV;
        else if (t == UCRT_FILE_TYPE_PIPE)
            f->flags |= UCRT_FPIPE;
    }
    if (flags & UCRT_O_APPEND)
        f->flags |= UCRT_FAPPEND;
    if (flags & UCRT_O_NOINHERIT)
        f->flags |= UCRT_FNOINHERIT;
    if (!(flags & UCRT_O_BINARY) && ((flags & UCRT_O_TEXT) || g_ucrt_io.fmode == UCRT_O_TEXT))
        f->flags |= UCRT_FTEXT;
    ucrt_io_unlock();
    return fd;
}

static inline int ucrt_dup(int fd)
{
    ucrt_fd *f;
    void *nh = NULL;
    int nfd;
    unsigned err;
    ucrt_io_lock();
    ucrt_io_init();
    f = ucrt_fd_get(fd);
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        ucrt_io_unlock();
        return -1;
    }
    nfd = ucrt_fd_alloc();
    if (nfd < 0) {
        ucrt_set_errno(UCRT_EMFILE);
        ucrt_io_unlock();
        return -1;
    }
    err = g_ucrt_os->dup(f->handle, &nh);
    if (err) {
        ucrt_dosmaperr(err);
        ucrt_io_unlock();
        return -1;
    }
    g_ucrt_io.fds[nfd] = *f;
    g_ucrt_io.fds[nfd].handle = nh;
    g_ucrt_io.fds[nfd].flags &= (unsigned char)~UCRT_FNOINHERIT;
    ucrt_io_unlock();
    return nfd;
}

/* Makes dst refer to the same file as src; returns 0 or -1. */
static inline int ucrt_dup2(int src, int dst)
{
    ucrt_fd *f;
    void *nh = NULL;
    unsigned err;
    ucrt_io_lock();
    ucrt_io_init();
    f = ucrt_fd_get(src);
    if (!f || dst < 0 || dst >= UCRT_NFILE) {
        ucrt_set_errno(f ? UCRT_EBADF : UCRT_EBADF);
        ucrt_io_unlock();
        return -1;
    }
    if (src == dst) {
        ucrt_io_unlock();
        return 0;
    }
    err = g_ucrt_os->dup(f->handle, &nh);
    if (err) {
        ucrt_dosmaperr(err);
        ucrt_io_unlock();
        return -1;
    }
    if (g_ucrt_io.fds[dst].flags & UCRT_FOPEN) {
        g_ucrt_os->close(g_ucrt_io.fds[dst].handle);
    }
    g_ucrt_io.fds[dst] = *f;
    g_ucrt_io.fds[dst].handle = nh;
    g_ucrt_io.fds[dst].flags &= (unsigned char)~UCRT_FNOINHERIT;
    ucrt_io_unlock();
    return 0;
}

static inline int ucrt_commit(int fd)
{
    ucrt_fd *f = ucrt_fd_get(fd);
    unsigned err;
    if (!f) {
        ucrt_set_errno(UCRT_EBADF);
        return -1;
    }
    if (!g_ucrt_os->flush)
        return 0;
    err = g_ucrt_os->flush(f->handle);
    if (err) {
        ucrt_dosmaperr(err);
        return -1;
    }
    return 0;
}

/* _chsize_s: truncates or extends (with zero bytes) to size, leaving the file position unchanged. */
static inline ucrt_errno_t ucrt_chsize_s(int fd, int64_t size)
{
    ucrt_fd *f = ucrt_fd_get(fd);
    unsigned err;
    int64_t cur, pos;
    if (!f)
        return ucrt_invalid(UCRT_EBADF);
    if (size < 0)
        return ucrt_invalid(UCRT_EINVAL);
    cur = ucrt_lseeki64(fd, 0, 1);
    if (cur < 0)
        return ucrt_get_errno();
    err = g_ucrt_os->set_eof(f->handle, size);
    if (err) {
        ucrt_dosmaperr(err);
        return ucrt_get_errno();
    }
    g_ucrt_os->seek(f->handle, cur, 0, &pos);
    return 0;
}

static inline int ucrt_chsize(int fd, int32_t size)
{
    return ucrt_chsize_s(fd, size) ? -1 : 0;
}

static inline int ucrt_wunlink(const ucrt_wchar *path)
{
    unsigned err;
    if (!path) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    err = g_ucrt_os->unlink(path);
    if (err) {
        ucrt_dosmaperr(err);
        return -1;
    }
    return 0;
}

static inline int ucrt_unlink(const char *path)
{
    ucrt_wchar w[UCRT_PATH_MAX];
    if (!path) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_widen_path(path, w, UCRT_PATH_MAX);
    return ucrt_wunlink(w);
}

static inline int ucrt_wrename(const ucrt_wchar *from, const ucrt_wchar *to)
{
    unsigned err;
    if (!from || !to) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    err = g_ucrt_os->rename(from, to);
    if (err) {
        ucrt_dosmaperr(err);
        return -1;
    }
    return 0;
}

static inline int ucrt_rename(const char *from, const char *to)
{
    ucrt_wchar a[UCRT_PATH_MAX], b[UCRT_PATH_MAX];
    if (!from || !to) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_widen_path(from, a, UCRT_PATH_MAX);
    ucrt_widen_path(to, b, UCRT_PATH_MAX);
    return ucrt_wrename(a, b);
}

#endif /* UCRT_IO_H */
