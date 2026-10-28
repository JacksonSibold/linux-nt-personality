/* ucrtbase_extra.inc - the smaller exports: word I/O, console characters, _sopen, file times, temporary names in a chosen
 * directory, date and time strings, and version queries (included by ucrtbase.c). */

#include "os_identity.h"

BOOL WINAPI SetFileTime(HANDLE, const uct_filetime *, const uct_filetime *, const uct_filetime *);

/* ---- Words and wide characters of the standard streams ----------------------------------------------------------------------------- */

int w__getw(ucrt_file *f)
{
    int v = 0;
    if (ucrt_fread(&v, sizeof v, 1, f) != 1)
        return -1;
    return v;
}

int w__putw(int v, ucrt_file *f) { return ucrt_fwrite(&v, sizeof v, 1, f) == 1 ? v : -1; }
int w__fgetwchar(void) { return ucrt_fgetwc(ucrt_iob_func(0)); }
int w__fputwchar(ucrt_wchar c) { return ucrt_fputwc(c, ucrt_iob_func(1)); }

/* ---- Console characters --------------------------------------------------------------------------------------------------------------- */

/* There are no console modes behind this runtime, so _getch reads the next byte of standard input as it is delivered (a line at
 * a time on a terminal) and does not echo it; _getche echoes it. */
static int g_uct_ungot = -1;

int w__getch(void)
{
    unsigned char c;
    if (g_uct_ungot >= 0) {
        int v = g_uct_ungot;
        g_uct_ungot = -1;
        return v;
    }
    return ucrt_read(0, &c, 1) == 1 ? c : -1;
}

int w__putch(int c)
{
    unsigned char b = (unsigned char)c;
    return ucrt_write(1, &b, 1) == 1 ? c & 0xFF : -1;
}

int w__getche(void)
{
    int c = w__getch();
    if (c >= 0)
        w__putch(c);
    return c;
}

int w__ungetch(int c)
{
    if (c == -1 || g_uct_ungot >= 0)
        return -1;
    g_uct_ungot = c & 0xFF;
    return c & 0xFF;
}

/* ---- _sopen and _wsopen ------------------------------------------------------------------------------------------------------------------ */

#define UCT_O_CREAT 0x0100

int w__sopen(const char *path, int oflag, int shflag, ...)
{
    int pmode = 0, fd = -1;
    if (oflag & UCT_O_CREAT) {
        va_list ap;
        va_start(ap, shflag);
        pmode = va_arg(ap, int);
        va_end(ap);
    }
    return ucrt_sopen_s(&fd, path, oflag, shflag, pmode) ? -1 : fd;
}

int w__wsopen(const ucrt_wchar *path, int oflag, int shflag, ...)
{
    int pmode = 0, fd = -1;
    if (oflag & UCT_O_CREAT) {
        va_list ap;
        va_start(ap, shflag);
        pmode = va_arg(ap, int);
        va_end(ap);
    }
    return ucrt_wsopen_s(&fd, path, oflag, shflag, pmode) ? -1 : fd;
}

/* ---- File times ---------------------------------------------------------------------------------------------------------------------------- */

typedef struct uct_utimbuf64 {
    int64_t actime, modtime;
} uct_utimbuf64;

static uct_filetime uct_time_to_ft(int64_t t)
{
    uint64_t v = (uint64_t)t * 10000000ull + UCRT_EPOCH_DIFF_100NS;
    uct_filetime ft;
    ft.low = (DWORD)v;
    ft.high = (DWORD)(v >> 32);
    return ft;
}

static int uct_set_times(HANDLE h, const uct_utimbuf64 *t)
{
    uct_filetime a, m;
    if (t) {
        a = uct_time_to_ft(t->actime);
        m = uct_time_to_ft(t->modtime);
    } else {
        uint64_t now = uct_now();
        a.low = m.low = (DWORD)now;
        a.high = m.high = (DWORD)(now >> 32);
    }
    return SetFileTime(h, NULL, &a, &m) ? 0 : uct_fail_os(GetLastError());
}

int w__futime64(int fd, const uct_utimbuf64 *t)
{
    void *h = ucrt_get_osfhandle(fd);
    if (!h || h == INVALID_HANDLE_VALUE) {
        ucrt_set_errno(UCRT_EBADF);
        return -1;
    }
    return uct_set_times(h, t);
}

int w__wutime64(const ucrt_wchar *path, const uct_utimbuf64 *t)
{
    HANDLE h;
    int r;
    if (!path) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    /* FILE_WRITE_ATTRIBUTES, shared, and backup semantics so a directory opens too */
    h = CreateFileW(path, 0x100, 7, NULL, UCRT_OPEN_EXISTING, 0x02000000, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return uct_fail_os(GetLastError());
    r = uct_set_times(h, t);
    CloseHandle(h);
    return r;
}

int w__utime64(const char *path, const uct_utimbuf64 *t)
{
    ucrt_wchar w[UCT_PATH_CAP];
    if (!path) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    if (uct_widen(path, w))
        return -1;
    return w__wutime64(w, t);
}

/* ---- Temporary names in a chosen directory ----------------------------------------------------------------------------------------------- */

static ucrt_wchar *uct_tempnam_wide(const ucrt_wchar *dir, const ucrt_wchar *prefix)
{
    ucrt_wchar base[UCT_PATH_CAP], path[UCT_PATH_CAP], *out;
    size_t n = 0, p = 0;
    DWORD a;
    if (dir && *dir && (a = GetFileAttributesW(dir)) != UCT_INVALID_ATTRS && (a & UCT_ATTR_DIRECTORY)) {
        for (; dir[n] && n < UCT_PATH_CAP - 24; ++n)
            base[n] = dir[n];
    } else {
        DWORD t = GetTempPathW(UCT_PATH_CAP - 24, base);
        if (!t || t >= UCT_PATH_CAP - 24) {
            base[0] = '\\';
            t = 1;
        }
        n = t;
    }
    if (n && base[n - 1] != '\\' && base[n - 1] != '/')
        base[n++] = '\\';
    for (; prefix && prefix[p] && p < 8; ++p)
        base[n + p] = prefix[p];
    n += p;
    for (int i = 0; i < 6; ++i)
        base[n + i] = 'X';
    base[n + 6] = 0;
    ucrt_memcpy(path, base, (n + 7) * sizeof(ucrt_wchar));
    if (uct_mktemp_wide(path, n + 6)) {
        ucrt_set_errno(UCRT_EEXIST);
        return NULL;
    }
    out = (ucrt_wchar *)w_malloc((n + 7) * sizeof(ucrt_wchar));
    if (out)
        ucrt_memcpy(out, path, (n + 7) * sizeof(ucrt_wchar));
    return out;
}

ucrt_wchar *w__wtempnam(const ucrt_wchar *dir, const ucrt_wchar *prefix) { return uct_tempnam_wide(dir, prefix); }

char *w__tempnam(const char *dir, const char *prefix)
{
    ucrt_wchar wd[UCT_PATH_CAP], wp[16], *r;
    char *out;
    if (dir && *dir) {
        if (uct_widen(dir, wd))
            return NULL;
    } else {
        wd[0] = 0;
    }
    wp[0] = 0;
    if (prefix)
        ucrt_widen_path(prefix, wp, 16);
    r = uct_tempnam_wide(wd, wp);
    if (!r)
        return NULL;
    out = (char *)w_malloc(ucrt_wcslen(r) + 1);
    if (out)
        uct_narrow(r, out, ucrt_wcslen(r) + 1);
    w_free(r);
    return out;
}

/* ---- Date and time strings ------------------------------------------------------------------------------------------------------------------ */

static int uct_now_tm(ucrt_tm *tm)
{
    int64_t t = ucrt_time64(NULL);
    return ucrt_localtime64_s(tm, &t);
}

static void uct_two(char *p, int v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

int w__strdate_s(char *buf, size_t size)
{
    ucrt_tm tm;
    if (!buf || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    if (size < 9) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    if (uct_now_tm(&tm))
        return ucrt_get_errno();
    uct_two(buf, tm.tm_mon + 1);
    buf[2] = '/';
    uct_two(buf + 3, tm.tm_mday);
    buf[5] = '/';
    uct_two(buf + 6, tm.tm_year);
    buf[8] = 0;
    return 0;
}

int w__strtime_s(char *buf, size_t size)
{
    ucrt_tm tm;
    if (!buf || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    if (size < 9) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    if (uct_now_tm(&tm))
        return ucrt_get_errno();
    uct_two(buf, tm.tm_hour);
    buf[2] = ':';
    uct_two(buf + 3, tm.tm_min);
    buf[5] = ':';
    uct_two(buf + 6, tm.tm_sec);
    buf[8] = 0;
    return 0;
}

char *w__strdate(char *buf) { return w__strdate_s(buf, 9) ? NULL : buf; }
char *w__strtime(char *buf) { return w__strtime_s(buf, 9) ? NULL : buf; }

static ucrt_wchar *uct_widen_timebuf(const char *s)
{
    ucrt_wchar *w = uct_thr()->wtimebuf;
    size_t i;
    if (!s)
        return NULL;
    for (i = 0; s[i] && i < 31; ++i)
        w[i] = (unsigned char)s[i];
    w[i] = 0;
    return w;
}

ucrt_wchar *w__wasctime(const ucrt_tm *tm) { return uct_widen_timebuf(w_asctime(tm)); }
ucrt_wchar *w__wctime64(const int64_t *t) { return uct_widen_timebuf(w__ctime64(t)); }

/* ---- Odds and ends ----------------------------------------------------------------------------------------------------------------------------- */

typedef void (*uct_se_translator)(unsigned, void *);
static uct_se_translator g_uct_se_translator;

/* Structured exceptions are not translated to C++ ones by this runtime; the function is stored so a caller can read it back. */
uct_se_translator w__set_se_translator(uct_se_translator f)
{
    uct_se_translator old = g_uct_se_translator;
    g_uct_se_translator = f;
    return old;
}

void w___acrt_errno_map_os_error(unsigned long e) { ucrt_dosmaperr((unsigned)e); }

int w__get_osplatform(unsigned *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = NT_OS_PLATFORM_ID;
    return 0;
}

int w__get_osver(unsigned *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = NT_OS_BUILD_NUMBER;
    return 0;
}

int w__get_winver(unsigned *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = (NT_OS_MAJOR_VERSION << 8) | NT_OS_MINOR_VERSION;
    return 0;
}

int w__get_winmajor(unsigned *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = NT_OS_MAJOR_VERSION;
    return 0;
}

int w__get_winminor(unsigned *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = NT_OS_MINOR_VERSION;
    return 0;
}
