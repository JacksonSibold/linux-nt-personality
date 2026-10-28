/*
 * ucrtbase.c - ucrtbase.dll: the Universal C Runtime on kernel32.
 *
 * The library code is src/crt (pure headers with host tests); this file supplies what the DLL adds: the backends
 * that put the runtime on kernel32 (files, clock, time zone, process, environment), per-thread errno, the heap
 * and the wrappers whose Windows signatures do not match a library function one to one. The simple wrappers are
 * generated (build/gen_ucrtbase.py writes ucrtbase_gen.inc and ucrtbase.def); the others are in ucrtbase_*.inc and
 * are exported when this directory defines a function named w_<export>.
 *
 * The runtime keeps its state in static variables of this translation unit, so everything here is one unit.
 */
#ifdef __MINGW32__
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "ucrt_runtime.h"
#include "ucrt_math.h"
#include "ucrt_math2.h"
#include "ucrt_time.h"
#include "ucrt_scanf.h"
#include "ucrt_conv.h"

#define WINAPI __stdcall
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *LPVOID;
typedef unsigned long long ULONGLONG;

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFul
#define HEAP_ZERO_MEMORY 8
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10

/* ---- kernel32 imports ------------------------------------------------------------------------------------- */

HANDLE WINAPI CreateFileW(const ucrt_wchar *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
BOOL WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
BOOL WINAPI SetFilePointerEx(HANDLE, int64_t, int64_t *, DWORD);
BOOL WINAPI SetEndOfFile(HANDLE);
BOOL WINAPI FlushFileBuffers(HANDLE);
BOOL WINAPI CloseHandle(HANDLE);
DWORD WINAPI GetFileType(HANDLE);
BOOL WINAPI DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
HANDLE WINAPI GetCurrentProcess(void);
BOOL WINAPI DeleteFileW(const ucrt_wchar *);
BOOL WINAPI MoveFileExW(const ucrt_wchar *, const ucrt_wchar *, DWORD);
HANDLE WINAPI GetStdHandle(DWORD);
DWORD WINAPI GetLastError(void);
void WINAPI SetLastError(DWORD);
void WINAPI GetSystemTimeAsFileTime(uint64_t *);
DWORD WINAPI GetTimeZoneInformation(void *);
DWORD WINAPI GetTickCount(void);
const ucrt_wchar *WINAPI GetCommandLineW(void);
ucrt_wchar *WINAPI GetEnvironmentStringsW(void);
BOOL WINAPI SetEnvironmentVariableW(const ucrt_wchar *, const ucrt_wchar *);
void WINAPI ExitProcess(unsigned);
HANDLE WINAPI GetProcessHeap(void);
void *WINAPI HeapAlloc(HANDLE, DWORD, size_t);
void *WINAPI HeapReAlloc(HANDLE, DWORD, void *, size_t);
BOOL WINAPI HeapFree(HANDLE, DWORD, void *);
size_t WINAPI HeapSize(HANDLE, DWORD, const void *);
DWORD WINAPI TlsAlloc(void);
BOOL WINAPI TlsFree(DWORD);
void *WINAPI TlsGetValue(DWORD);
BOOL WINAPI TlsSetValue(DWORD, void *);
void WINAPI InitializeCriticalSection(void *);
void WINAPI EnterCriticalSection(void *);
void WINAPI LeaveCriticalSection(void *);

/* ---- Per-thread state: errno, _doserrno and the static buffers of the time functions ---------------------------- */

#include "ucrtbase_thread.h"

static DWORD g_uct_tls = TLS_OUT_OF_INDEXES;
static uct_thread g_uct_main; /* before the slot exists, and when allocating a thread's record fails */

static uct_thread *uct_thr(void)
{
    uct_thread *t;
    if (g_uct_tls == TLS_OUT_OF_INDEXES)
        return &g_uct_main;
    t = (uct_thread *)TlsGetValue(g_uct_tls);
    if (!t) {
        t = (uct_thread *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
        if (!t)
            return &g_uct_main;
        TlsSetValue(g_uct_tls, t);
    }
    return t;
}

static int *uct_errno_slot(void) { return &uct_thr()->err; }
static unsigned long *uct_doserrno_slot(void) { return &uct_thr()->dos; }

/* ---- Backends ------------------------------------------------------------------------------------------------ */

static unsigned long long g_uct_cs[8]; /* the runtime's I/O lock (a CRITICAL_SECTION is 40 bytes) */

static void uct_lock(void) { EnterCriticalSection(g_uct_cs); }
static void uct_unlock(void) { LeaveCriticalSection(g_uct_cs); }

static unsigned uct_open(const ucrt_wchar *path, unsigned access, unsigned share, unsigned disp, unsigned attrs, void **h)
{
    HANDLE f = CreateFileW(path, access, share, NULL, disp, attrs, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError();
    *h = f;
    return 0;
}

static unsigned uct_read(void *h, void *buf, unsigned n, unsigned *got)
{
    DWORD g = 0;
    if (!ReadFile(h, buf, n, &g, NULL)) {
        *got = 0;
        return GetLastError();
    }
    *got = g;
    return 0;
}

static unsigned uct_write(void *h, const void *buf, unsigned n, unsigned *wrote)
{
    DWORD w = 0;
    if (!WriteFile(h, buf, n, &w, NULL)) {
        *wrote = 0;
        return GetLastError();
    }
    *wrote = w;
    return 0;
}

static unsigned uct_seek(void *h, int64_t off, int whence, int64_t *pos)
{
    int64_t np = 0;
    if (!SetFilePointerEx(h, off, &np, (DWORD)whence))
        return GetLastError();
    if (pos)
        *pos = np;
    return 0;
}

static unsigned uct_close(void *h) { return CloseHandle(h) ? 0 : GetLastError(); }
static unsigned uct_filetype(void *h) { return GetFileType(h); }

static unsigned uct_set_eof(void *h, int64_t size)
{
    int64_t cur = 0;
    unsigned e = 0;
    if (!SetFilePointerEx(h, 0, &cur, 1))
        return GetLastError();
    if (!SetFilePointerEx(h, size, NULL, 0) || !SetEndOfFile(h))
        e = GetLastError();
    SetFilePointerEx(h, cur, NULL, 0);
    return e;
}

static unsigned uct_flush(void *h) { return FlushFileBuffers(h) ? 0 : GetLastError(); }

static unsigned uct_dup(void *h, void **out)
{
    HANDLE self = GetCurrentProcess(), d = NULL;
    if (!DuplicateHandle(self, h, self, &d, 0, 0, 2 /* DUPLICATE_SAME_ACCESS */))
        return GetLastError();
    *out = d;
    return 0;
}

static unsigned uct_unlink(const ucrt_wchar *p) { return DeleteFileW(p) ? 0 : GetLastError(); }
static unsigned uct_rename(const ucrt_wchar *a, const ucrt_wchar *b) { return MoveFileExW(a, b, 0) ? 0 : GetLastError(); }

static unsigned uct_get_std(unsigned n, void **h)
{
    HANDLE s = GetStdHandle((DWORD)(-10 - (int)n)); /* STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE */
    if (!s || s == INVALID_HANDLE_VALUE)
        return 6; /* ERROR_INVALID_HANDLE */
    *h = s;
    return 0;
}

static const ucrt_os g_uct_os = {uct_open,  uct_read,   uct_write,  uct_seek,   uct_close,  uct_filetype, uct_set_eof,
                                 uct_flush, uct_dup,    uct_unlink, uct_rename, uct_get_std, uct_lock,     uct_unlock};

static DWORD g_uct_start_tick;

static uint64_t uct_now(void)
{
    uint64_t ft = 0;
    GetSystemTimeAsFileTime(&ft);
    return ft;
}

static uint64_t uct_process_ms(void) { return (DWORD)(GetTickCount() - g_uct_start_tick); }
static const char *uct_time_getenv(const char *name) { return ucrt_getenv(name); }

/* TIME_ZONE_INFORMATION: Bias, StandardName[32], StandardDate, StandardBias, DaylightName[32], DaylightDate, DaylightBias */
#pragma pack(push, 1)
typedef struct uct_systemtime {
    uint16_t year, month, dow, day, hour, minute, second, ms;
} uct_systemtime;
typedef struct uct_tzi {
    int32_t bias;
    ucrt_wchar std_name[32];
    uct_systemtime std_date;
    int32_t std_bias;
    ucrt_wchar dst_name[32];
    uct_systemtime dst_date;
    int32_t dst_bias;
} uct_tzi;
#pragma pack(pop)

static void uct_rule(ucrt_rule *r, const uct_systemtime *s)
{
    r->year = s->year;
    r->month = s->month;
    r->dow = s->dow;
    r->day = s->day;
    r->hour = s->hour;
    r->minute = s->minute;
    r->second = s->second;
}

static void uct_zone_name(char *out, const ucrt_wchar *w)
{
    size_t i;
    for (i = 0; i < 32 && w[i] && i < 63; ++i)
        out[i] = w[i] < 0x80 ? (char)w[i] : '?';
    out[i] = 0;
}

static int uct_get_tz(ucrt_tzinfo *out)
{
    uct_tzi z;
    if (GetTimeZoneInformation(&z) == 0xFFFFFFFFul)
        return -1;
    out->bias = z.bias;
    out->std_bias = z.std_bias;
    out->dst_bias = z.dst_bias;
    uct_zone_name(out->std_name, z.std_name);
    uct_zone_name(out->dst_name, z.dst_name);
    uct_rule(&out->std_date, &z.std_date);
    uct_rule(&out->dst_date, &z.dst_date);
    return 0;
}

static const ucrt_time_ops g_uct_time = {uct_now, uct_get_tz, uct_process_ms, uct_time_getenv};

static void uct_terminate(unsigned code) { ExitProcess(code); }
static const ucrt_wchar *uct_cmdline(void) { return GetCommandLineW(); }
static const ucrt_wchar *uct_env_block(void) { return GetEnvironmentStringsW(); }
static unsigned uct_set_env(const ucrt_wchar *name, const ucrt_wchar *value)
{
    return SetEnvironmentVariableW(name, value) ? 0 : GetLastError();
}

static const ucrt_proc_ops g_uct_proc = {uct_terminate, uct_cmdline, uct_env_block, uct_set_env};

/* ---- The heap ----------------------------------------------------------------------------------------------- */

static void *uct_heap_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void uct_heap_free(void *p) { HeapFree(GetProcessHeap(), 0, p); }

/* ---- Entry point ----------------------------------------------------------------------------------------------- */

static void uct_init(void)
{
    InitializeCriticalSection(g_uct_cs);
    g_uct_tls = TlsAlloc();
    g_uct_start_tick = GetTickCount();
    g_ucrt_malloc = uct_heap_alloc;
    g_ucrt_free = uct_heap_free;
    g_ucrt_os = &g_uct_os;
    g_ucrt_time_ops = &g_uct_time;
    g_ucrt_proc = &g_uct_proc;
    g_ucrt_errno_slot = uct_errno_slot;
    g_ucrt_doserrno_slot = uct_doserrno_slot;
    ucrt_runtime_init();
}

BOOL WINAPI DllMainCRTStartup(HANDLE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    switch (reason) {
    case 1: /* DLL_PROCESS_ATTACH */
        uct_init();
        break;
    case 3: /* DLL_THREAD_DETACH */
        if (g_uct_tls != TLS_OUT_OF_INDEXES) {
            uct_thread *t = (uct_thread *)TlsGetValue(g_uct_tls);
            if (t) {
                TlsSetValue(g_uct_tls, NULL);
                HeapFree(GetProcessHeap(), 0, t);
            }
        }
        break;
    }
    return 1;
}

/* ---- Wrappers with a different shape from the library function ------------------------------------------------------------- */

#define SHIM_STRTO(name, ret, impl, T, CT)                                                                           \
    static inline ret name(const T *s, T **end, int base) { return impl(s, (const T **)(void *)end, base); }

SHIM_STRTO(ucrt_strtol_x, long, ucrt_strtol, char, const char)
SHIM_STRTO(ucrt_strtoul_x, unsigned long, ucrt_strtoul, char, const char)
SHIM_STRTO(ucrt_strtoi64_x, long long, ucrt_strtoi64, char, const char)
SHIM_STRTO(ucrt_strtoui64_x, unsigned long long, ucrt_strtoui64, char, const char)
SHIM_STRTO(ucrt_wcstol_x, long, ucrt_wcstol, ucrt_wchar, const ucrt_wchar)
SHIM_STRTO(ucrt_wcstoul_x, unsigned long, ucrt_wcstoul, ucrt_wchar, const ucrt_wchar)
SHIM_STRTO(ucrt_wcstoi64_x, long long, ucrt_wcstoi64, ucrt_wchar, const ucrt_wchar)
SHIM_STRTO(ucrt_wcstoui64_x, unsigned long long, ucrt_wcstoui64, ucrt_wchar, const ucrt_wchar)

static inline double ucrt_strtod_x(const char *s, char **end) { return ucrt_strtod(s, (const char **)(void *)end); }
static inline float ucrt_strtof_x(const char *s, char **end) { return ucrt_strtof(s, (const char **)(void *)end); }
static inline double ucrt_wcstod_x(const ucrt_wchar *s, ucrt_wchar **end) { return ucrt_wcstod(s, (const ucrt_wchar **)(void *)end); }
static inline float ucrt_wcstof_x(const ucrt_wchar *s, ucrt_wchar **end) { return ucrt_wcstof(s, (const ucrt_wchar **)(void *)end); }

static inline char *ucrt_setlocale_x(int category, const char *locale) { return (char *)(uintptr_t)ucrt_setlocale(category, locale); }
static inline char *ucrt_strtok_r_l(char *s, const char *d, char **ctx, void *loc) { (void)loc; return ucrt_strtok_r(s, d, ctx); }
static inline int ucrt_loc_isblank(int c) { return (ucrt_loc_class(c) & UCRT_BLANK) != 0; }
static inline int ucrt_toascii(int c) { return c & 0x7F; }
static inline double ucrt_chgsign(double x) { return ucrt_copysign(x, -ucrt_copysign(1.0, x)); }
static inline intptr_t ucrt_get_osfhandle_i(int fd) { return (intptr_t)ucrt_get_osfhandle(fd); }
static inline int ucrt_open_osfhandle_i(intptr_t h, int flags) { return ucrt_open_osfhandle((void *)h, flags); }
static inline ucrt_file *ucrt_wfsopen_w(const ucrt_wchar *path, const char *mode, int shflag)
{
    return ucrt_wfsopen(path, mode, shflag);
}
static inline ucrt_file *ucrt_iob_base(void) { return ucrt_iob_func(0); }
static inline int ucrt_getwchar(void) { return ucrt_fgetwc(ucrt_iob_func(0)); }
static inline int ucrt_putwchar(ucrt_wchar c) { return ucrt_fputwc(c, ucrt_iob_func(1)); }

typedef int (*ucrt_cmp_fn)(const void *, const void *);
typedef int (*ucrt_cmp_s_fn)(void *, const void *, const void *);

#include "ucrtbase_gen.inc"
#include "ucrtbase_heap.inc"
#include "ucrtbase_stdio.inc"
#include "ucrtbase_start.inc"
#include "ucrtbase_misc.inc"
#include "ucrtbase_fp.inc"
#include "ucrtbase_fs.inc"

#endif /* __MINGW32__ */
