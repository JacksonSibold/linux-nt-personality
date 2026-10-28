/* ucrtbase_fs.inc - files and directories: stat, access, the current directory, full paths, directory searches, temporary
 * names (included by ucrtbase.c). Narrow paths become wide ones through ucrt_widen_path, and names come back narrow
 * with characters outside Latin-1 as '?'. */

typedef struct uct_filetime {
    DWORD low, high;
} uct_filetime;

typedef struct uct_file_attr_data {
    DWORD attrs;
    uct_filetime create, access, write;
    DWORD size_high, size_low;
} uct_file_attr_data;

typedef struct uct_by_handle_info {
    DWORD attrs;
    uct_filetime create, access, write;
    DWORD volume, size_high, size_low, nlinks, index_high, index_low;
} uct_by_handle_info;

typedef struct uct_find_data {
    DWORD attrs;
    uct_filetime create, access, write;
    DWORD size_high, size_low, reserved0, reserved1;
    ucrt_wchar name[260];
    ucrt_wchar alt[14];
} uct_find_data;

DWORD WINAPI GetFileAttributesW(const ucrt_wchar *);
BOOL WINAPI SetFileAttributesW(const ucrt_wchar *, DWORD);
BOOL WINAPI GetFileAttributesExW(const ucrt_wchar *, int, void *);
BOOL WINAPI GetFileInformationByHandle(HANDLE, void *);
HANDLE WINAPI FindFirstFileW(const ucrt_wchar *, void *);
BOOL WINAPI FindNextFileW(HANDLE, void *);
BOOL WINAPI FindClose(HANDLE);
DWORD WINAPI GetFullPathNameW(const ucrt_wchar *, DWORD, ucrt_wchar *, ucrt_wchar **);
DWORD WINAPI GetCurrentDirectoryW(DWORD, ucrt_wchar *);
BOOL WINAPI SetCurrentDirectoryW(const ucrt_wchar *);
BOOL WINAPI CreateDirectoryW(const ucrt_wchar *, void *);
BOOL WINAPI RemoveDirectoryW(const ucrt_wchar *);
DWORD WINAPI GetCurrentProcessId(void);
DWORD WINAPI GetTempPathW(DWORD, ucrt_wchar *);

#define UCT_ATTR_READONLY 0x1u
#define UCT_ATTR_DIRECTORY 0x10u
#define UCT_ATTR_NORMAL 0x80u
#define UCT_INVALID_ATTRS 0xFFFFFFFFu
#define UCT_PATH_CAP 520

/* ---- Narrow and wide path text ---------------------------------------------------------------------------------------------- */

/* Narrow text for a wide name: Latin-1 characters map to their byte, anything else becomes '?'. Returns 0 when it fits. */
static int uct_narrow(const ucrt_wchar *w, char *out, size_t cap)
{
    size_t i;
    for (i = 0; w[i]; ++i) {
        if (i + 1 >= cap) {
            if (cap)
                out[0] = 0;
            return -1;
        }
        out[i] = w[i] < 0x100 ? (char)w[i] : '?';
    }
    if (cap)
        out[i] = 0;
    return 0;
}

/* The path of a narrow argument as wide text; -1 (with ENAMETOOLONG) when it does not fit. */
static int uct_widen(const char *path, ucrt_wchar *w)
{
    if (ucrt_widen_path(path, w, UCT_PATH_CAP) >= UCT_PATH_CAP - 1 && path[UCT_PATH_CAP - 1]) {
        ucrt_set_errno(UCRT_ENAMETOOLONG);
        return -1;
    }
    return 0;
}

static int uct_fail_os(DWORD e)
{
    ucrt_dosmaperr(e);
    return -1;
}

/* ---- stat ------------------------------------------------------------------------------------------------------------------------ */

typedef struct uct_stat_info {
    unsigned mode, nlink, dev;
    int64_t size, atime, mtime, ctime;
} uct_stat_info;

#define UCT_S_IFDIR 0x4000u
#define UCT_S_IFCHR 0x2000u
#define UCT_S_IFIFO 0x1000u
#define UCT_S_IFREG 0x8000u
#define UCT_S_IREAD 0x0100u
#define UCT_S_IWRITE 0x0080u
#define UCT_S_IEXEC 0x0040u

static int64_t uct_ft_to_time(uct_filetime ft)
{
    uint64_t v = ((uint64_t)ft.high << 32) | ft.low;
    if (v < UCRT_EPOCH_DIFF_100NS)
        return 0;
    return (int64_t)((v - UCRT_EPOCH_DIFF_100NS) / 10000000ull);
}

/* The mode bits of a file with these attributes; the owner permissions are copied to group and others. */
static unsigned uct_mode_of(DWORD attrs, const ucrt_wchar *path)
{
    unsigned m = (attrs & UCT_ATTR_DIRECTORY) ? (UCT_S_IFDIR | UCT_S_IREAD | UCT_S_IEXEC) : (UCT_S_IFREG | UCT_S_IREAD);
    if (!(attrs & UCT_ATTR_READONLY))
        m |= UCT_S_IWRITE;
    if (path && !(attrs & UCT_ATTR_DIRECTORY)) {
        size_t n = ucrt_wcslen(path);
        if (n >= 4 && path[n - 4] == '.') {
            ucrt_wchar e[4];
            for (int i = 0; i < 3; ++i)
                e[i] = ucrt_towlower(path[n - 3 + i]);
            if ((e[0] == 'e' && e[1] == 'x' && e[2] == 'e') || (e[0] == 'c' && e[1] == 'o' && e[2] == 'm') ||
                (e[0] == 'b' && e[1] == 'a' && e[2] == 't') || (e[0] == 'c' && e[1] == 'm' && e[2] == 'd'))
                m |= UCT_S_IEXEC;
        }
    }
    m |= (m & 0x1C0u) >> 3;
    m |= (m & 0x1C0u) >> 6;
    return m;
}

static int uct_drive_of(const ucrt_wchar *path)
{
    ucrt_wchar cwd[UCT_PATH_CAP];
    if (path[0] && path[1] == ':')
        return ucrt_towupper(path[0]) - 'A';
    if (GetCurrentDirectoryW(UCT_PATH_CAP, cwd) && cwd[1] == ':')
        return ucrt_towupper(cwd[0]) - 'A';
    return 2;
}

static int uct_stat_wide(const ucrt_wchar *path, uct_stat_info *si)
{
    uct_file_attr_data d;
    ucrt_wchar fixed[UCT_PATH_CAP];
    size_t n;
    if (!path || !*path) {
        ucrt_set_errno(UCRT_EINVAL);
        return -1;
    }
    for (n = 0; path[n]; ++n)
        if (path[n] == '*' || path[n] == '?') {
            ucrt_set_errno(UCRT_ENOENT);
            return -1;
        }
    /* "C:" alone and a trailing separator name the directory itself; the system call wants neither form */
    ucrt_wcsncpy_s(fixed, UCT_PATH_CAP, path, (size_t)-1);
    n = ucrt_wcslen(fixed);
    if (n >= 2 && (fixed[n - 1] == '\\' || fixed[n - 1] == '/') && fixed[n - 2] != ':')
        fixed[n - 1] = 0;
    if (!GetFileAttributesExW(fixed, 0, &d))
        return uct_fail_os(GetLastError());
    si->mode = uct_mode_of(d.attrs, path);
    si->nlink = 1;
    si->dev = (unsigned)uct_drive_of(path);
    si->size = ((int64_t)d.size_high << 32) | d.size_low;
    si->mtime = uct_ft_to_time(d.write);
    si->atime = (d.access.low || d.access.high) ? uct_ft_to_time(d.access) : si->mtime;
    si->ctime = (d.create.low || d.create.high) ? uct_ft_to_time(d.create) : si->mtime;
    return 0;
}

static int uct_stat_fd(int fd, uct_stat_info *si)
{
    void *h = ucrt_get_osfhandle(fd);
    unsigned type;
    if (!h || h == INVALID_HANDLE_VALUE) {
        ucrt_set_errno(UCRT_EBADF);
        return -1;
    }
    type = GetFileType(h);
    ucrt_memset(si, 0, sizeof *si);
    si->nlink = 1;
    si->dev = (unsigned)fd;
    if (type == 2) { /* FILE_TYPE_CHAR */
        si->mode = UCT_S_IFCHR;
        return 0;
    }
    if (type == 3) { /* FILE_TYPE_PIPE */
        si->mode = UCT_S_IFIFO;
        return 0;
    }
    if (type == 1) {
        uct_by_handle_info b;
        if (!GetFileInformationByHandle(h, &b))
            return uct_fail_os(GetLastError());
        si->mode = uct_mode_of(b.attrs, NULL);
        si->nlink = b.nlinks;
        si->size = ((int64_t)b.size_high << 32) | b.size_low;
        si->mtime = uct_ft_to_time(b.write);
        si->atime = (b.access.low || b.access.high) ? uct_ft_to_time(b.access) : si->mtime;
        si->ctime = (b.create.low || b.create.high) ? uct_ft_to_time(b.create) : si->mtime;
        return 0;
    }
    ucrt_set_errno(UCRT_EBADF);
    return -1;
}

/* The three layouts of the stat structure: 64-bit time and size, 64-bit time and 32-bit size, 32-bit time and size. */
typedef struct uct_stat64 {
    uint32_t dev;
    uint16_t ino, mode;
    int16_t nlink, uid, gid;
    uint32_t rdev;
    int64_t size, atime, mtime, ctime;
} uct_stat64;

typedef struct uct_stat64i32 {
    uint32_t dev;
    uint16_t ino, mode;
    int16_t nlink, uid, gid;
    uint32_t rdev;
    int32_t size;
    int64_t atime, mtime, ctime;
} uct_stat64i32;

typedef struct uct_stat32 {
    uint32_t dev;
    uint16_t ino, mode;
    int16_t nlink, uid, gid;
    uint32_t rdev;
    int32_t size;
    int32_t atime, mtime, ctime;
} uct_stat32;

#define UCT_EOVERFLOW 75

/* The layouts programs compiled against the Windows headers expect (x64). */
_Static_assert(sizeof(uct_stat64) == 56 && offsetof(uct_stat64, rdev) == 16 && offsetof(uct_stat64, size) == 24 &&
                   offsetof(uct_stat64, atime) == 32 && offsetof(uct_stat64, ctime) == 48,
               "struct _stat64");
_Static_assert(sizeof(uct_stat64i32) == 48 && offsetof(uct_stat64i32, size) == 20 && offsetof(uct_stat64i32, atime) == 24,
               "struct _stat64i32");
_Static_assert(sizeof(uct_stat32) == 36 && offsetof(uct_stat32, size) == 20 && offsetof(uct_stat32, atime) == 24 &&
                   offsetof(uct_stat32, ctime) == 32,
               "struct _stat32");

static int uct_pack_stat(const uct_stat_info *si, int variant, void *out)
{
    if (variant == 0) {
        uct_stat64 *s = (uct_stat64 *)out;
        ucrt_memset(s, 0, sizeof *s);
        s->dev = s->rdev = si->dev;
        s->mode = (uint16_t)si->mode;
        s->nlink = (int16_t)si->nlink;
        s->size = si->size;
        s->atime = si->atime;
        s->mtime = si->mtime;
        s->ctime = si->ctime;
        return 0;
    }
    if (si->size > 0x7FFFFFFF) {
        ucrt_set_errno(UCT_EOVERFLOW);
        return -1;
    }
    if (variant == 1) {
        uct_stat64i32 *s = (uct_stat64i32 *)out;
        ucrt_memset(s, 0, sizeof *s);
        s->dev = s->rdev = si->dev;
        s->mode = (uint16_t)si->mode;
        s->nlink = (int16_t)si->nlink;
        s->size = (int32_t)si->size;
        s->atime = si->atime;
        s->mtime = si->mtime;
        s->ctime = si->ctime;
        return 0;
    }
    {
        uct_stat32 *s = (uct_stat32 *)out;
        if (si->atime > 0x7FFFFFFF || si->mtime > 0x7FFFFFFF || si->ctime > 0x7FFFFFFF) {
            ucrt_set_errno(UCT_EOVERFLOW);
            return -1;
        }
        ucrt_memset(s, 0, sizeof *s);
        s->dev = s->rdev = si->dev;
        s->mode = (uint16_t)si->mode;
        s->nlink = (int16_t)si->nlink;
        s->size = (int32_t)si->size;
        s->atime = (int32_t)si->atime;
        s->mtime = (int32_t)si->mtime;
        s->ctime = (int32_t)si->ctime;
        return 0;
    }
}

static int uct_stat_path_variant(const char *path, int variant, void *out)
{
    ucrt_wchar w[UCT_PATH_CAP];
    uct_stat_info si;
    if (!path || !out) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    if (uct_widen(path, w) || uct_stat_wide(w, &si))
        return -1;
    return uct_pack_stat(&si, variant, out);
}

static int uct_stat_wpath_variant(const ucrt_wchar *path, int variant, void *out)
{
    uct_stat_info si;
    if (!path || !out) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    if (uct_stat_wide(path, &si))
        return -1;
    return uct_pack_stat(&si, variant, out);
}

static int uct_stat_fd_variant(int fd, int variant, void *out)
{
    uct_stat_info si;
    if (!out) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    if (uct_stat_fd(fd, &si))
        return -1;
    return uct_pack_stat(&si, variant, out);
}

int w__stat64(const char *p, void *out) { return uct_stat_path_variant(p, 0, out); }
int w__stat64i32(const char *p, void *out) { return uct_stat_path_variant(p, 1, out); }
int w__stat32(const char *p, void *out) { return uct_stat_path_variant(p, 2, out); }
int w__wstat64(const ucrt_wchar *p, void *out) { return uct_stat_wpath_variant(p, 0, out); }
int w__wstat64i32(const ucrt_wchar *p, void *out) { return uct_stat_wpath_variant(p, 1, out); }
int w__wstat32(const ucrt_wchar *p, void *out) { return uct_stat_wpath_variant(p, 2, out); }
int w__fstat64(int fd, void *out) { return uct_stat_fd_variant(fd, 0, out); }
int w__fstat64i32(int fd, void *out) { return uct_stat_fd_variant(fd, 1, out); }
int w__fstat32(int fd, void *out) { return uct_stat_fd_variant(fd, 2, out); }

/* ---- access, chmod, umask -------------------------------------------------------------------------------------------------------- */

static int uct_access_wide(const ucrt_wchar *path, int mode)
{
    DWORD a;
    if (!path || (mode & ~6)) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    a = GetFileAttributesW(path);
    if (a == UCT_INVALID_ATTRS) {
        ucrt_dosmaperr(GetLastError());
        return ucrt_get_errno();
    }
    if ((mode & 2) && (a & UCT_ATTR_READONLY) && !(a & UCT_ATTR_DIRECTORY)) {
        ucrt_set_errno(UCRT_EACCES);
        return UCRT_EACCES;
    }
    return 0;
}

int w__waccess(const ucrt_wchar *p, int mode) { return uct_access_wide(p, mode) ? -1 : 0; }
int w__waccess_s(const ucrt_wchar *p, int mode) { return uct_access_wide(p, mode); }

int w__access(const char *p, int mode)
{
    ucrt_wchar w[UCT_PATH_CAP];
    if (!p) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    if (uct_widen(p, w))
        return -1;
    return uct_access_wide(w, mode) ? -1 : 0;
}

int w__access_s(const char *p, int mode)
{
    ucrt_wchar w[UCT_PATH_CAP];
    if (!p)
        return ucrt_invalid(UCRT_EINVAL);
    if (uct_widen(p, w))
        return UCRT_ENAMETOOLONG;
    return uct_access_wide(w, mode);
}

int w__wchmod(const ucrt_wchar *path, int mode)
{
    DWORD a;
    if (!path || !*path) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    a = GetFileAttributesW(path);
    if (a == UCT_INVALID_ATTRS)
        return uct_fail_os(GetLastError());
    if (mode & UCT_S_IWRITE)
        a &= ~UCT_ATTR_READONLY;
    else
        a |= UCT_ATTR_READONLY;
    if (!SetFileAttributesW(path, a))
        return uct_fail_os(GetLastError());
    return 0;
}

int w__chmod(const char *path, int mode)
{
    ucrt_wchar w[UCT_PATH_CAP];
    if (!path) {
        return ucrt_invalid(UCRT_EINVAL), -1;
    }
    if (uct_widen(path, w))
        return -1;
    return w__wchmod(w, mode);
}

static int g_uct_umask;
int w__umask(int mask)
{
    int old = g_uct_umask;
    g_uct_umask = mask & 0x180;
    return old;
}

int w__umask_s(int mask, int *old)
{
    if (!old)
        return ucrt_invalid(UCRT_EINVAL);
    *old = w__umask(mask);
    return 0;
}

/* ---- Directories ------------------------------------------------------------------------------------------------------------------- */

char *w__getcwd(char *buf, int size)
{
    ucrt_wchar w[UCT_PATH_CAP];
    DWORD n = GetCurrentDirectoryW(UCT_PATH_CAP, w);
    size_t need;
    if (!n || n >= UCT_PATH_CAP) {
        ucrt_dosmaperr(GetLastError());
        return NULL;
    }
    need = n + 1;
    if (!buf) {
        size_t cap = size > 0 && (size_t)size > need ? (size_t)size : need;
        buf = (char *)w_malloc(cap);
        if (!buf)
            return NULL;
        size = (int)cap;
    } else if (size <= 0) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    } else if ((size_t)size < need) {
        ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    uct_narrow(w, buf, (size_t)size);
    return buf;
}

ucrt_wchar *w__wgetcwd(ucrt_wchar *buf, int size)
{
    DWORD n;
    size_t need;
    ucrt_wchar w[UCT_PATH_CAP];
    n = GetCurrentDirectoryW(UCT_PATH_CAP, w);
    if (!n || n >= UCT_PATH_CAP) {
        ucrt_dosmaperr(GetLastError());
        return NULL;
    }
    need = n + 1;
    if (!buf) {
        size_t cap = size > 0 && (size_t)size > need ? (size_t)size : need;
        buf = (ucrt_wchar *)w_malloc(cap * sizeof(ucrt_wchar));
        if (!buf)
            return NULL;
        size = (int)cap;
    } else if (size <= 0) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    } else if ((size_t)size < need) {
        ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    ucrt_memcpy(buf, w, need * sizeof(ucrt_wchar));
    return buf;
}

/* The current directory of a drive (1 = A:, 0 = the current drive). */
static DWORD uct_dcwd(int drive, ucrt_wchar *out)
{
    ucrt_wchar spec[3];
    if (drive == 0)
        return GetCurrentDirectoryW(UCT_PATH_CAP, out);
    if (drive < 1 || drive > 26) {
        ucrt_set_errno(UCRT_EACCES);
        return 0;
    }
    spec[0] = (ucrt_wchar)('A' + drive - 1);
    spec[1] = ':';
    spec[2] = 0;
    return GetFullPathNameW(spec, UCT_PATH_CAP, out, NULL);
}

char *w__getdcwd(int drive, char *buf, int size)
{
    ucrt_wchar w[UCT_PATH_CAP];
    DWORD n = uct_dcwd(drive, w);
    size_t need;
    if (!n || n >= UCT_PATH_CAP) {
        if (!n && drive >= 1 && drive <= 26)
            ucrt_dosmaperr(GetLastError());
        return NULL;
    }
    need = n + 1;
    if (!buf) {
        size_t cap = size > 0 && (size_t)size > need ? (size_t)size : need;
        buf = (char *)w_malloc(cap);
        if (!buf)
            return NULL;
        size = (int)cap;
    } else if (size <= 0) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    } else if ((size_t)size < need) {
        ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    uct_narrow(w, buf, (size_t)size);
    return buf;
}

ucrt_wchar *w__wgetdcwd(int drive, ucrt_wchar *buf, int size)
{
    ucrt_wchar w[UCT_PATH_CAP];
    DWORD n = uct_dcwd(drive, w);
    size_t need;
    if (!n || n >= UCT_PATH_CAP) {
        if (!n && drive >= 1 && drive <= 26)
            ucrt_dosmaperr(GetLastError());
        return NULL;
    }
    need = n + 1;
    if (!buf) {
        size_t cap = size > 0 && (size_t)size > need ? (size_t)size : need;
        buf = (ucrt_wchar *)w_malloc(cap * sizeof(ucrt_wchar));
        if (!buf)
            return NULL;
        size = (int)cap;
    } else if (size <= 0) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    } else if ((size_t)size < need) {
        ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    ucrt_memcpy(buf, w, need * sizeof(ucrt_wchar));
    return buf;
}

int w__getdrive(void)
{
    ucrt_wchar w[UCT_PATH_CAP];
    if (GetCurrentDirectoryW(UCT_PATH_CAP, w) && w[1] == ':')
        return ucrt_towupper(w[0]) - 'A' + 1;
    return 0;
}

int w__chdrive(int drive)
{
    ucrt_wchar spec[3];
    if (drive < 1 || drive > 26) {
        ucrt_set_errno(UCRT_EACCES);
        return -1;
    }
    spec[0] = (ucrt_wchar)('A' + drive - 1);
    spec[1] = ':';
    spec[2] = 0;
    return SetCurrentDirectoryW(spec) ? 0 : uct_fail_os(GetLastError());
}

int w__wchdir(const ucrt_wchar *p)
{
    if (!p)
        return ucrt_invalid(UCRT_EINVAL), -1;
    return SetCurrentDirectoryW(p) ? 0 : uct_fail_os(GetLastError());
}

int w__wmkdir(const ucrt_wchar *p)
{
    if (!p)
        return ucrt_invalid(UCRT_EINVAL), -1;
    return CreateDirectoryW(p, NULL) ? 0 : uct_fail_os(GetLastError());
}

int w__wrmdir(const ucrt_wchar *p)
{
    if (!p)
        return ucrt_invalid(UCRT_EINVAL), -1;
    return RemoveDirectoryW(p) ? 0 : uct_fail_os(GetLastError());
}

#define UCT_NARROW_PATH_CALL(name, wide)                                                                                \
    int name(const char *p)                                                                                             \
    {                                                                                                                   \
        ucrt_wchar w[UCT_PATH_CAP];                                                                                     \
        if (!p)                                                                                                         \
            return ucrt_invalid(UCRT_EINVAL), -1;                                                                       \
        if (uct_widen(p, w))                                                                                            \
            return -1;                                                                                                  \
        return wide(w);                                                                                                 \
    }

UCT_NARROW_PATH_CALL(w__chdir, w__wchdir)
UCT_NARROW_PATH_CALL(w__mkdir, w__wmkdir)
UCT_NARROW_PATH_CALL(w__rmdir, w__wrmdir)

/* _fullpath: the absolute form of a path, written into abs (or a new block when abs is NULL). */
char *w__fullpath(char *abs, const char *rel, size_t max)
{
    ucrt_wchar w[UCT_PATH_CAP], full[UCT_PATH_CAP];
    DWORD n;
    if (!rel || !*rel) {
        if (abs && max)
            return w__getcwd(abs, (int)(max > 0x7FFFFFFF ? 0x7FFFFFFF : max));
        return w__getcwd(abs, (int)max);
    }
    if (uct_widen(rel, w))
        return NULL;
    n = GetFullPathNameW(w, UCT_PATH_CAP, full, NULL);
    if (!n || n >= UCT_PATH_CAP) {
        if (!n)
            ucrt_dosmaperr(GetLastError());
        else
            ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    if (!abs) {
        size_t cap = max > (size_t)n + 1 ? max : (size_t)n + 1;
        abs = (char *)w_malloc(cap);
        if (!abs)
            return NULL;
        max = cap;
    } else if (max < (size_t)n + 1) {
        ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    uct_narrow(full, abs, max);
    return abs;
}

ucrt_wchar *w__wfullpath(ucrt_wchar *abs, const ucrt_wchar *rel, size_t max)
{
    ucrt_wchar full[UCT_PATH_CAP];
    DWORD n;
    if (!rel || !*rel)
        return w__wgetcwd(abs, (int)(max > 0x7FFFFFFF ? 0x7FFFFFFF : max));
    n = GetFullPathNameW(rel, UCT_PATH_CAP, full, NULL);
    if (!n || n >= UCT_PATH_CAP) {
        if (!n)
            ucrt_dosmaperr(GetLastError());
        else
            ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    if (!abs) {
        size_t cap = max > (size_t)n + 1 ? max : (size_t)n + 1;
        abs = (ucrt_wchar *)w_malloc(cap * sizeof(ucrt_wchar));
        if (!abs)
            return NULL;
        max = cap;
    } else if (max < (size_t)n + 1) {
        ucrt_set_errno(UCRT_ERANGE);
        return NULL;
    }
    ucrt_memcpy(abs, full, ((size_t)n + 1) * sizeof(ucrt_wchar));
    return abs;
}

/* ---- _splitpath and _makepath -------------------------------------------------------------------------------------------------------- */

#define UCT_T char
#define UCT_P w__
#include "ucrtbase_path_t.inc"
#undef UCT_T
#undef UCT_P

#define UCT_T ucrt_wchar
#define UCT_P w__w
#include "ucrtbase_path_t.inc"
#undef UCT_T
#undef UCT_P

/* ---- Directory searches ------------------------------------------------------------------------------------------------------------------ */

/* __finddata64_t and _finddata64i32_t (and the wide forms): attributes, three times, the size, the name. */
typedef struct uct_finddata64 {
    unsigned attrib;
    int64_t time_create, time_access, time_write;
    int64_t size;
    char name[260];
} uct_finddata64;

typedef struct uct_finddata64i32 {
    unsigned attrib;
    int64_t time_create, time_access, time_write;
    uint32_t size;
    char name[260];
} uct_finddata64i32;

typedef struct uct_wfinddata64 {
    unsigned attrib;
    int64_t time_create, time_access, time_write;
    int64_t size;
    ucrt_wchar name[260];
} uct_wfinddata64;

typedef struct uct_wfinddata64i32 {
    unsigned attrib;
    int64_t time_create, time_access, time_write;
    uint32_t size;
    ucrt_wchar name[260];
} uct_wfinddata64i32;

_Static_assert(sizeof(uct_finddata64) == 304 && offsetof(uct_finddata64, size) == 32 && offsetof(uct_finddata64, name) == 40,
               "struct __finddata64_t");
_Static_assert(sizeof(uct_finddata64i32) == 296 && offsetof(uct_finddata64i32, size) == 32 && offsetof(uct_finddata64i32, name) == 36,
               "struct _finddata64i32_t");
_Static_assert(sizeof(uct_wfinddata64) == 560 && offsetof(uct_wfinddata64, name) == 40, "struct _wfinddata64_t");
_Static_assert(sizeof(uct_wfinddata64i32) == 560 && offsetof(uct_wfinddata64i32, name) == 36, "struct _wfinddata64i32_t");
_Static_assert(sizeof(uct_find_data) == 592 && offsetof(uct_find_data, name) == 44, "WIN32_FIND_DATAW");
_Static_assert(sizeof(uct_file_attr_data) == 36, "WIN32_FILE_ATTRIBUTE_DATA");
_Static_assert(sizeof(uct_by_handle_info) == 52, "BY_HANDLE_FILE_INFORMATION");

static unsigned uct_find_attrib(DWORD a) { return a == UCT_ATTR_NORMAL ? 0 : a; }

static void uct_find_times(const uct_find_data *f, int64_t *create, int64_t *access, int64_t *write)
{
    *write = uct_ft_to_time(f->write);
    *access = (f->access.low || f->access.high) ? uct_ft_to_time(f->access) : *write;
    *create = (f->create.low || f->create.high) ? uct_ft_to_time(f->create) : *write;
}

/* variant: 0 narrow/64, 1 narrow/32 size, 2 wide/64, 3 wide/32 size */
static int uct_find_fill(const uct_find_data *f, int variant, void *out)
{
    int64_t size = ((int64_t)f->size_high << 32) | f->size_low;
    switch (variant) {
    case 0: {
        uct_finddata64 *d = (uct_finddata64 *)out;
        d->attrib = uct_find_attrib(f->attrs);
        uct_find_times(f, &d->time_create, &d->time_access, &d->time_write);
        d->size = size;
        uct_narrow(f->name, d->name, sizeof d->name);
        return 0;
    }
    case 1: {
        uct_finddata64i32 *d = (uct_finddata64i32 *)out;
        d->attrib = uct_find_attrib(f->attrs);
        uct_find_times(f, &d->time_create, &d->time_access, &d->time_write);
        d->size = (uint32_t)size;
        uct_narrow(f->name, d->name, sizeof d->name);
        return 0;
    }
    case 2: {
        uct_wfinddata64 *d = (uct_wfinddata64 *)out;
        d->attrib = uct_find_attrib(f->attrs);
        uct_find_times(f, &d->time_create, &d->time_access, &d->time_write);
        d->size = size;
        ucrt_memcpy(d->name, f->name, sizeof d->name);
        return 0;
    }
    default: {
        uct_wfinddata64i32 *d = (uct_wfinddata64i32 *)out;
        d->attrib = uct_find_attrib(f->attrs);
        uct_find_times(f, &d->time_create, &d->time_access, &d->time_write);
        d->size = (uint32_t)size;
        ucrt_memcpy(d->name, f->name, sizeof d->name);
        return 0;
    }
    }
}

static intptr_t uct_findfirst_wide(const ucrt_wchar *spec, int variant, void *out)
{
    uct_find_data f;
    HANDLE h;
    if (!spec || !out) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    h = FindFirstFileW(spec, &f);
    if (h == INVALID_HANDLE_VALUE) {
        ucrt_dosmaperr(GetLastError());
        return -1;
    }
    uct_find_fill(&f, variant, out);
    return (intptr_t)h;
}

static int uct_findnext(intptr_t h, int variant, void *out)
{
    uct_find_data f;
    if (!out || h == -1 || h == 0) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    if (!FindNextFileW((HANDLE)h, &f)) {
        ucrt_dosmaperr(GetLastError());
        return -1;
    }
    uct_find_fill(&f, variant, out);
    return 0;
}

static intptr_t uct_findfirst_narrow(const char *spec, int variant, void *out)
{
    ucrt_wchar w[UCT_PATH_CAP];
    if (!spec) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    if (uct_widen(spec, w))
        return -1;
    return uct_findfirst_wide(w, variant, out);
}

intptr_t w__findfirst64(const char *spec, void *out) { return uct_findfirst_narrow(spec, 0, out); }
intptr_t w__findfirst64i32(const char *spec, void *out) { return uct_findfirst_narrow(spec, 1, out); }
intptr_t w__wfindfirst64(const ucrt_wchar *spec, void *out) { return uct_findfirst_wide(spec, 2, out); }
intptr_t w__wfindfirst64i32(const ucrt_wchar *spec, void *out) { return uct_findfirst_wide(spec, 3, out); }
int w__findnext64(intptr_t h, void *out) { return uct_findnext(h, 0, out); }
int w__findnext64i32(intptr_t h, void *out) { return uct_findnext(h, 1, out); }
int w__wfindnext64(intptr_t h, void *out) { return uct_findnext(h, 2, out); }
int w__wfindnext64i32(intptr_t h, void *out) { return uct_findnext(h, 3, out); }

int w__findclose(intptr_t h)
{
    if (h == -1 || h == 0) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    return FindClose((HANDLE)h) ? 0 : uct_fail_os(GetLastError());
}

/* ---- Temporary names ------------------------------------------------------------------------------------------------------------------------ */

static unsigned g_uct_tmp_counter;

/* Replaces the trailing XXXXXX of a template with letters and the process number until the name is free. */
static int uct_mktemp_wide(ucrt_wchar *t, size_t len)
{
    unsigned seed = GetCurrentProcessId() * 2654435761u + (++g_uct_tmp_counter) * 40503u;
    if (len < 6)
        return UCRT_EINVAL;
    for (size_t k = len - 6; k < len; ++k)
        if (t[k] != 'X')
            return UCRT_EINVAL;
    for (int attempt = 0; attempt < 26 * 26 * 26; ++attempt) {
        unsigned v = seed + (unsigned)attempt * 2891336453u;
        for (int i = 0; i < 6; ++i) {
            t[len - 6 + i] = (ucrt_wchar)('a' + v % 26);
            v = v / 26 + (unsigned)(i + 1) * 7919u;
        }
        if (GetFileAttributesW(t) == UCT_INVALID_ATTRS)
            return 0;
    }
    return UCRT_EEXIST;
}

char *w__mktemp(char *t)
{
    ucrt_wchar w[UCT_PATH_CAP];
    int e;
    if (!t) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    if (uct_widen(t, w))
        return NULL;
    e = uct_mktemp_wide(w, ucrt_wcslen(w));
    if (e) {
        ucrt_set_errno(e);
        return NULL;
    }
    uct_narrow(w, t, ucrt_strlen(t) + 1);
    return t;
}

int w__mktemp_s(char *t, size_t size)
{
    ucrt_wchar w[UCT_PATH_CAP];
    size_t len;
    int e;
    if (!t || !size || (len = ucrt_strnlen(t, size)) == size)
        return ucrt_invalid(UCRT_EINVAL);
    if (uct_widen(t, w))
        return UCRT_ENAMETOOLONG;
    e = uct_mktemp_wide(w, ucrt_wcslen(w));
    if (e) {
        t[0] = 0;
        return ucrt_invalid(e);
    }
    uct_narrow(w, t, len + 1);
    return 0;
}

ucrt_wchar *w__wmktemp(ucrt_wchar *t)
{
    int e;
    if (!t) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    e = uct_mktemp_wide(t, ucrt_wcslen(t));
    if (e) {
        ucrt_set_errno(e);
        return NULL;
    }
    return t;
}

/* tmpnam: names in the root of the current drive, "\s<digits>.<digit>", that do not exist. */
static char g_uct_tmpnam[16];

char *w_tmpnam(char *buf)
{
    ucrt_wchar w[24];
    char *out = buf ? buf : g_uct_tmpnam;
    for (int attempt = 0; attempt < 100000; ++attempt) {
        unsigned n = ++g_uct_tmp_counter + GetCurrentProcessId() * 7u;
        char name[16];
        int len = 0;
        name[len++] = '\\';
        name[len++] = 's';
        for (int shift = 0; shift < 4; ++shift)
            name[len++] = "0123456789abcdefghijklmnopqrstuv"[(n >> (5 * shift)) & 31];
        name[len++] = '.';
        name[len++] = "0123456789abcdefghijklmnopqrstuv"[(n >> 20) & 31];
        name[len] = 0;
        ucrt_widen_path(name, w, 24);
        if (GetFileAttributesW(w) == UCT_INVALID_ATTRS) {
            ucrt_memcpy(out, name, (size_t)len + 1);
            return out;
        }
    }
    ucrt_set_errno(UCRT_EEXIST);
    return NULL;
}

/* tmpfile: a binary file in the temporary directory that goes away when it is closed. */
ucrt_file *w_tmpfile(void)
{
    ucrt_wchar dir[UCT_PATH_CAP], path[UCT_PATH_CAP];
    DWORD n = GetTempPathW(UCT_PATH_CAP - 16, dir);
    if (!n || n >= UCT_PATH_CAP - 16) {
        ucrt_set_errno(UCRT_ENOENT);
        return NULL;
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        ucrt_file *f;
        static const ucrt_wchar mode[] = {'w', '+', 'b', 'D', 0};
        ucrt_memcpy(path, dir, ((size_t)n + 1) * sizeof(ucrt_wchar));
        path[n] = 's';
        for (int i = 0; i < 6; ++i)
            path[n + 1 + i] = 'X';
        path[n + 7] = 0;
        if (uct_mktemp_wide(path, n + 7))
            continue;
        f = ucrt_wfopen(path, mode);
        if (f)
            return f;
    }
    return NULL;
}

/* ---- Process identity ------------------------------------------------------------------------------------------------------------------------- */

int w__getpid(void) { return (int)GetCurrentProcessId(); }
unsigned long w___threadid(void) { return GetCurrentThreadId(); }

/* There is no keyboard device behind this runtime's console input, so nothing is ever waiting. */
int w__kbhit(void) { return 0; }
