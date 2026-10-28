/*
 * ucrt_os.h - the operating system interface the C runtime's I/O code sits on.
 *
 * The runtime calls these through one table, so the same stdio and file descriptor code runs in the DLL (on
 * the Win32 file functions) and in the host tests (on POSIX). Every function that can fail returns a Win32
 * error code, 0 for success; the runtime maps it to errno with ucrt_dosmaperr.
 */
#ifndef UCRT_OS_H
#define UCRT_OS_H

#include "ucrt_state.h"

/* access rights, creation dispositions and sharing, with the Win32 values */
#define UCRT_GENERIC_READ 0x80000000u
#define UCRT_GENERIC_WRITE 0x40000000u
#define UCRT_FILE_SHARE_READ 1u
#define UCRT_FILE_SHARE_WRITE 2u
#define UCRT_CREATE_NEW 1u
#define UCRT_CREATE_ALWAYS 2u
#define UCRT_OPEN_EXISTING 3u
#define UCRT_OPEN_ALWAYS 4u
#define UCRT_TRUNCATE_EXISTING 5u
#define UCRT_ATTR_READONLY 1u
#define UCRT_ATTR_NORMAL 0x80u
#define UCRT_ATTR_TEMPORARY 0x100u
#define UCRT_FLAG_DELETE_ON_CLOSE 0x04000000u

#define UCRT_FILE_TYPE_UNKNOWN 0u
#define UCRT_FILE_TYPE_DISK 1u
#define UCRT_FILE_TYPE_CHAR 2u
#define UCRT_FILE_TYPE_PIPE 3u

/* Win32 error codes the runtime produces or tests for */
#define UCRT_ERROR_FILE_NOT_FOUND 2u
#define UCRT_ERROR_ACCESS_DENIED 5u
#define UCRT_ERROR_INVALID_HANDLE 6u
#define UCRT_ERROR_BROKEN_PIPE 109u
#define UCRT_ERROR_HANDLE_EOF 38u
#define UCRT_ERROR_DISK_FULL 112u
#define UCRT_ERROR_NEGATIVE_SEEK 131u

typedef struct ucrt_os {
    unsigned (*open)(const ucrt_wchar *path, unsigned access, unsigned share, unsigned disp, unsigned attrs, void **h);
    unsigned (*read)(void *h, void *buf, unsigned n, unsigned *got);
    unsigned (*write)(void *h, const void *buf, unsigned n, unsigned *wrote);
    unsigned (*seek)(void *h, int64_t off, int whence, int64_t *pos); /* whence: 0 begin, 1 current, 2 end */
    unsigned (*close)(void *h);
    unsigned (*filetype)(void *h);
    unsigned (*set_eof)(void *h, int64_t size);                        /* truncate or extend to size */
    unsigned (*flush)(void *h);
    unsigned (*dup)(void *h, void **out);
    unsigned (*unlink)(const ucrt_wchar *path);
    unsigned (*rename)(const ucrt_wchar *from, const ucrt_wchar *to);
    unsigned (*get_std)(unsigned n, void **h);                         /* 0 stdin, 1 stdout, 2 stderr */
    void (*lock)(void);                                                 /* one reentrant lock for the I/O code */
    void (*unlock)(void);
} ucrt_os;

static const ucrt_os *g_ucrt_os;

/* The allocator the runtime's buffers come from: the DLL points these at the process heap, the host tests at
 * malloc and free. */
static void *(*g_ucrt_malloc)(size_t);
static void (*g_ucrt_free)(void *);

static inline void *ucrt_alloc(size_t n)
{
    return g_ucrt_malloc ? g_ucrt_malloc(n) : NULL;
}

static inline void ucrt_release(void *p)
{
    if (p && g_ucrt_free)
        g_ucrt_free(p);
}

static inline void ucrt_io_lock(void)
{
    if (g_ucrt_os && g_ucrt_os->lock)
        g_ucrt_os->lock();
}

static inline void ucrt_io_unlock(void)
{
    if (g_ucrt_os && g_ucrt_os->unlock)
        g_ucrt_os->unlock();
}

/* Win32 error code -> errno, with the C runtime's table. Unknown codes in the ranges the runtime knows map to
 * EACCES (19..36), ENOEXEC (188..202), otherwise EINVAL. */
static inline int ucrt_dosmaperr_code(unsigned e)
{
    switch (e) {
    case 1: return UCRT_EINVAL;
    case 2: case 3: return UCRT_ENOENT;
    case 4: return UCRT_EMFILE;
    case 5: return UCRT_EACCES;
    case 6: return UCRT_EBADF;
    case 7: case 8: case 9: return UCRT_ENOMEM;
    case 10: return UCRT_E2BIG;
    case 11: return UCRT_ENOEXEC;
    case 12: case 13: return UCRT_EINVAL;
    case 15: return UCRT_ENOENT;
    case 16: return UCRT_EACCES;
    case 17: return UCRT_EXDEV;
    case 18: return UCRT_ENOENT;
    case 33: return UCRT_EACCES;
    case 53: return UCRT_ENOENT;
    case 65: return UCRT_EACCES;
    case 67: return UCRT_ENOENT;
    case 80: return UCRT_EEXIST;
    case 82: case 83: return UCRT_EACCES;
    case 87: return UCRT_EINVAL;
    case 89: return UCRT_EAGAIN;
    case 109: return UCRT_EPIPE;
    case 112: return UCRT_ENOSPC;
    case 114: return UCRT_EBADF;
    case 124: return UCRT_EINVAL;
    case 128: case 129: return UCRT_ECHILD;
    case 130: return UCRT_EBADF;
    case 131: return UCRT_EINVAL;
    case 132: return UCRT_EACCES;
    case 145: return UCRT_ENOTEMPTY;
    case 158: return UCRT_EACCES;
    case 161: return UCRT_ENOENT;
    case 164: return UCRT_EAGAIN;
    case 167: return UCRT_EACCES;
    case 183: return UCRT_EEXIST;
    case 206: return UCRT_ENOENT;
    case 215: return UCRT_EAGAIN;
    case 232: return UCRT_EPIPE;       /* ERROR_NO_DATA: the pipe is being closed */
    case 1816: return UCRT_ENOMEM;
    default:
        if (e >= 19 && e <= 36)
            return UCRT_EACCES;
        if (e >= 188 && e <= 202)
            return UCRT_ENOEXEC;
        return UCRT_EINVAL;
    }
}

/* Sets errno and _doserrno from a Win32 error. */
static inline void ucrt_dosmaperr(unsigned e)
{
    g_ucrt_state.doserrno_value = e;
    ucrt_set_errno(ucrt_dosmaperr_code(e));
}

#endif /* UCRT_OS_H */
