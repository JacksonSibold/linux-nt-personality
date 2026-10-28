/* ucrtbase_more.inc - further exports (included by ucrtbase.c): thread-locale and stream limits, terminate handlers, collation
 * in the "C" locale, the in-place case functions with sizes, 32-bit time variants, disk space, single-precision helpers built
 * on the double functions, assert, and setjmp/longjmp. */

/* UCT_EXPORTS _configthreadlocale _getmaxstdio _setmaxstdio _lock_locales _unlock_locales */

static int g_uct_thread_locale = 2; /* _DISABLE_PER_THREAD_LOCALE: the locale is the process's */

int w__configthreadlocale(int mode)
{
    int old = g_uct_thread_locale;
    if (mode == 0)
        return old;
    if (mode == 1 || mode == 2) {
        g_uct_thread_locale = mode;
        return old;
    }
    if (mode == -1) { /* _ENABLE_PER_THREAD_LOCALE_NEW: later threads get their own locale; this runtime keeps one */
        g_uct_thread_locale = 2;
        return old;
    }
    ucrt_invalid(UCRT_EINVAL);
    return -1;
}

int w__getmaxstdio(void) { return 512; }

int w__setmaxstdio(int n)
{
    if (n < 32 || n > 8192) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    return n < UCRT_NFILE ? n : UCRT_NFILE;
}

void w__lock_locales(void) {}
void w__unlock_locales(void) {}

/* ---- C++ terminate and unexpected ------------------------------------------------------------------------------------------------------ */

typedef void (*uct_terminate_fn)(void);
static uct_terminate_fn g_uct_terminate_handler, g_uct_unexpected_handler;

uct_terminate_fn w_set_terminate(uct_terminate_fn f)
{
    uct_terminate_fn old = g_uct_terminate_handler;
    g_uct_terminate_handler = f;
    return old;
}

uct_terminate_fn w__get_terminate(void) { return g_uct_terminate_handler; }

uct_terminate_fn w_set_unexpected(uct_terminate_fn f)
{
    uct_terminate_fn old = g_uct_unexpected_handler;
    g_uct_unexpected_handler = f;
    return old;
}

uct_terminate_fn w__get_unexpected(void) { return g_uct_unexpected_handler; }

/* The runtime's terminate: the installed handler, then abort. (Redefines the earlier simple form.) */
void w_unexpected(void)
{
    if (g_uct_unexpected_handler)
        g_uct_unexpected_handler();
    if (g_uct_terminate_handler)
        g_uct_terminate_handler();
    ucrt_abort();
}

/* ---- Collation in the "C" locale ------------------------------------------------------------------------------------------------------- */

static int uct_sign(int v) { return v < 0 ? -1 : v > 0; }

int w_strcoll(const char *a, const char *b) { return uct_sign(ucrt_strcmp(a, b)); }
int w__stricoll(const char *a, const char *b) { return uct_sign(ucrt_stricmp(a, b)); }
int w__strncoll(const char *a, const char *b, size_t n) { return uct_sign(ucrt_strncmp(a, b, n)); }
int w__strnicoll(const char *a, const char *b, size_t n) { return uct_sign(ucrt_strnicmp(a, b, n)); }
int w_wcscoll(const ucrt_wchar *a, const ucrt_wchar *b) { return uct_sign(ucrt_wcscmp(a, b)); }
int w__wcsicoll(const ucrt_wchar *a, const ucrt_wchar *b) { return uct_sign(ucrt_wcsicmp(a, b)); }
int w__wcsncoll(const ucrt_wchar *a, const ucrt_wchar *b, size_t n) { return uct_sign(ucrt_wcsncmp(a, b, n)); }
int w__wcsnicoll(const ucrt_wchar *a, const ucrt_wchar *b, size_t n) { return uct_sign(ucrt_wcsnicmp(a, b, n)); }

size_t w_strxfrm(char *dst, const char *src, size_t n)
{
    size_t len = ucrt_strlen(src);
    if (dst && n > len)
        ucrt_memcpy(dst, src, len + 1);
    else if (dst && n)
        dst[0] = 0;
    return len;
}

size_t w_wcsxfrm(ucrt_wchar *dst, const ucrt_wchar *src, size_t n)
{
    size_t len = ucrt_wcslen(src);
    if (dst && n > len)
        ucrt_memcpy(dst, src, (len + 1) * sizeof(ucrt_wchar));
    else if (dst && n)
        dst[0] = 0;
    return len;
}

/* UCT_EXPORTS _strcoll_l _stricoll_l _strncoll_l _strnicoll_l _wcscoll_l _wcsicoll_l _wcsncoll_l _wcsnicoll_l _strxfrm_l _wcsxfrm_l */
int w__strcoll_l(const char *a, const char *b, void *l) { (void)l; return w_strcoll(a, b); }
int w__stricoll_l(const char *a, const char *b, void *l) { (void)l; return w__stricoll(a, b); }
int w__strncoll_l(const char *a, const char *b, size_t n, void *l) { (void)l; return w__strncoll(a, b, n); }
int w__strnicoll_l(const char *a, const char *b, size_t n, void *l) { (void)l; return w__strnicoll(a, b, n); }
int w__wcscoll_l(const ucrt_wchar *a, const ucrt_wchar *b, void *l) { (void)l; return w_wcscoll(a, b); }
int w__wcsicoll_l(const ucrt_wchar *a, const ucrt_wchar *b, void *l) { (void)l; return w__wcsicoll(a, b); }
int w__wcsncoll_l(const ucrt_wchar *a, const ucrt_wchar *b, size_t n, void *l) { (void)l; return w__wcsncoll(a, b, n); }
int w__wcsnicoll_l(const ucrt_wchar *a, const ucrt_wchar *b, size_t n, void *l) { (void)l; return w__wcsnicoll(a, b, n); }
size_t w__strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return w_strxfrm(d, s, n); }
size_t w__wcsxfrm_l(ucrt_wchar *d, const ucrt_wchar *s, size_t n, void *l) { (void)l; return w_wcsxfrm(d, s, n); }

/* ---- In-place case and fill functions with sizes ---------------------------------------------------------------------------------------- */

/* UCT_EXPORTS _strlwr_s _strupr_s _strset_s _strnset_s _wcslwr_s _wcsupr_s _wcsset_s _wcsnset_s _strlwr_s_l _strupr_s_l _wcslwr_s_l _wcsupr_s_l */

static int uct_s_check(void *buf, size_t size, size_t unit, int wide)
{
    size_t n = 0;
    if (!buf || !size)
        return UCRT_EINVAL;
    if (wide) {
        const ucrt_wchar *w = (const ucrt_wchar *)buf;
        while (n < size && w[n])
            ++n;
    } else {
        const char *s = (const char *)buf;
        while (n < size && s[n])
            ++n;
    }
    (void)unit;
    return n == size ? UCRT_EINVAL : 0;
}

int w__strlwr_s(char *s, size_t size)
{
    if (uct_s_check(s, size, 1, 0))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_strlwr(s);
    return 0;
}

int w__strupr_s(char *s, size_t size)
{
    if (uct_s_check(s, size, 1, 0))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_strupr(s);
    return 0;
}

int w__strset_s(char *s, size_t size, int c)
{
    if (uct_s_check(s, size, 1, 0))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_strset(s, c);
    return 0;
}

int w__strnset_s(char *s, size_t size, int c, size_t n)
{
    if (uct_s_check(s, size, 1, 0))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_strnset(s, c, n);
    return 0;
}

int w__wcslwr_s(ucrt_wchar *s, size_t size)
{
    if (uct_s_check(s, size, 2, 1))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_wcslwr(s);
    return 0;
}

int w__wcsupr_s(ucrt_wchar *s, size_t size)
{
    if (uct_s_check(s, size, 2, 1))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_wcsupr(s);
    return 0;
}

int w__wcsset_s(ucrt_wchar *s, size_t size, ucrt_wchar c)
{
    if (uct_s_check(s, size, 2, 1))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_wcsset(s, c);
    return 0;
}

int w__wcsnset_s(ucrt_wchar *s, size_t size, ucrt_wchar c, size_t n)
{
    if (uct_s_check(s, size, 2, 1))
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_wcsnset(s, c, n);
    return 0;
}

int w__strlwr_s_l(char *s, size_t size, void *l) { (void)l; return w__strlwr_s(s, size); }
int w__strupr_s_l(char *s, size_t size, void *l) { (void)l; return w__strupr_s(s, size); }
int w__wcslwr_s_l(ucrt_wchar *s, size_t size, void *l) { (void)l; return w__wcslwr_s(s, size); }
int w__wcsupr_s_l(ucrt_wchar *s, size_t size, void *l) { (void)l; return w__wcsupr_s(s, size); }

/* ---- Identifier characters --------------------------------------------------------------------------------------------------------------- */

/* UCT_EXPORTS __iscsym __iscsymf __iswcsym __iswcsymf */
int w___iscsym(int c) { return ucrt_isalnum(c) || c == '_'; }
int w___iscsymf(int c) { return ucrt_isalpha(c) || c == '_'; }
int w___iswcsym(ucrt_wchar c) { return ucrt_iswalnum(c) || c == '_'; }
int w___iswcsymf(ucrt_wchar c) { return ucrt_iswalpha(c) || c == '_'; }

/* ---- Streams ------------------------------------------------------------------------------------------------------------------------------ */

/* UCT_EXPORTS fread_s _fgetchar _fputchar _wperror _wcserror_s __wcserror __wcserror_s __wcsncnt _assert _wassert */

size_t w_fread_s(void *buf, size_t bufsize, size_t elsize, size_t count, ucrt_file *f)
{
    if (elsize == 0 || count == 0)
        return 0;
    if (!buf || (size_t)-1 / elsize < count || elsize * count > bufsize) {
        if (buf && bufsize)
            ucrt_memset(buf, 0, bufsize);
        ucrt_invalid(buf ? UCRT_ERANGE : UCRT_EINVAL);
        return 0;
    }
    return ucrt_fread(buf, elsize, count, f);
}

int w__fgetchar(void) { return ucrt_fgetc(ucrt_iob_func(0)); }
int w__fputchar(int c) { return ucrt_fputc(c, ucrt_iob_func(1)); }

void w__wperror(const ucrt_wchar *msg)
{
    ucrt_file *err = ucrt_iob_func(2);
    const char *text = ucrt_strerror(ucrt_get_errno());
    if (msg && *msg) {
        ucrt_fputws(msg, err);
        ucrt_fputc(':', err);
        ucrt_fputc(' ', err);
    }
    ucrt_fputs(text, err);
    ucrt_fputc('\n', err);
}

int w__wcserror_s(ucrt_wchar *buf, size_t size, int e)
{
    const char *text = ucrt_strerror(e);
    size_t n = ucrt_strlen(text), i;
    if (!buf || !size)
        return ucrt_invalid(UCRT_EINVAL);
    for (i = 0; i < n && i + 1 < size; ++i)
        buf[i] = (unsigned char)text[i];
    buf[i] = 0;
    return i < n ? UCRT_ERANGE : 0;
}

ucrt_wchar *w___wcserror(const ucrt_wchar *msg)
{
    uct_thread *t = uct_thr();
    size_t i = 0, k;
    const char *text = ucrt_strerror(ucrt_get_errno());
    if (msg && *msg) {
        for (; msg[i] && i < 90; ++i)
            t->werrbuf[i] = msg[i];
        t->werrbuf[i++] = ':';
        t->werrbuf[i++] = ' ';
    }
    for (k = 0; text[k] && i < 156; ++k, ++i)
        t->werrbuf[i] = (unsigned char)text[k];
    t->werrbuf[i++] = '\n';
    t->werrbuf[i] = 0;
    return t->werrbuf;
}

int w___wcserror_s(ucrt_wchar *buf, size_t size, const ucrt_wchar *msg)
{
    ucrt_wchar *s = w___wcserror(msg);
    size_t n = ucrt_wcslen(s);
    if (!buf || !size)
        return ucrt_invalid(UCRT_EINVAL);
    if (n + 1 > size) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memcpy(buf, s, (n + 1) * sizeof(ucrt_wchar));
    return 0;
}

size_t w___wcsncnt(const ucrt_wchar *s, size_t n) { return ucrt_wcsnlen(s, n); }

void w__assert(const char *msg, const char *file, unsigned line)
{
    ucrt_file *err = ucrt_iob_func(2);
    char num[16];
    ucrt_fputs("Assertion failed: ", err);
    ucrt_fputs(msg ? msg : "", err);
    ucrt_fputs(", file ", err);
    ucrt_fputs(file ? file : "", err);
    ucrt_fputs(", line ", err);
    ucrt_stri64toa_s(line, num, sizeof num, 10, 1);
    ucrt_fputs(num, err);
    ucrt_fputs("\n", err);
    ucrt_fflush(err);
    ucrt_abort();
}

void w__wassert(const ucrt_wchar *msg, const ucrt_wchar *file, unsigned line)
{
    ucrt_file *err = ucrt_iob_func(2);
    char num[16];
    ucrt_fputs("Assertion failed: ", err);
    if (msg)
        ucrt_fputws(msg, err);
    ucrt_fputs(", file ", err);
    if (file)
        ucrt_fputws(file, err);
    ucrt_fputs(", line ", err);
    ucrt_stri64toa_s(line, num, sizeof num, 10, 1);
    ucrt_fputs(num, err);
    ucrt_fputs("\n", err);
    ucrt_fflush(err);
    ucrt_abort();
}

/* ---- 32-bit time variants --------------------------------------------------------------------------------------------------------------------- */

/* UCT_EXPORTS _gmtime32 _localtime32 _ctime32 _difftime32 _ftime32_s _mkgmtime32 _wctime32 _wasctime_s _wctime64_s */

ucrt_tm *w__gmtime32(const int *t)
{
    int64_t v;
    if (!t) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    v = *t;
    return w__gmtime64(&v);
}

ucrt_tm *w__localtime32(const int *t)
{
    int64_t v;
    if (!t) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    v = *t;
    return w__localtime64(&v);
}

char *w__ctime32(const int *t)
{
    int64_t v;
    if (!t) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    v = *t;
    return w__ctime64(&v);
}

ucrt_wchar *w__wctime32(const int *t)
{
    int64_t v;
    if (!t) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    v = *t;
    return w__wctime64(&v);
}

double w__difftime32(int a, int b) { return (double)a - (double)b; }

int w__mkgmtime32(ucrt_tm *tm)
{
    int64_t v = ucrt_mkgmtime64(tm);
    return v < 0 || v > 0x7FFFFFFF ? -1 : (int)v;
}

typedef struct uct_timeb32 {
    int time;
    unsigned short millitm;
    short timezone, dstflag;
} uct_timeb32;

int w__ftime32_s(uct_timeb32 *tb)
{
    ucrt_timeb t;
    if (!tb)
        return ucrt_invalid(UCRT_EINVAL);
    ucrt_ftime64(&t);
    tb->time = (int)t.time;
    tb->millitm = t.millitm;
    tb->timezone = t.timezone;
    tb->dstflag = t.dstflag;
    return 0;
}

int w__wasctime_s(ucrt_wchar *buf, size_t size, const ucrt_tm *tm)
{
    char tmp[32];
    int e;
    if (!buf || size < 26) {
        if (buf && size)
            buf[0] = 0;
        return ucrt_invalid(buf ? UCRT_ERANGE : UCRT_EINVAL);
    }
    e = ucrt_asctime_s(tmp, sizeof tmp, tm);
    if (e) {
        buf[0] = 0;
        return e;
    }
    for (size_t i = 0;; ++i) {
        buf[i] = (unsigned char)tmp[i];
        if (!tmp[i])
            break;
    }
    return 0;
}

int w__wctime64_s(ucrt_wchar *buf, size_t size, const int64_t *t)
{
    char tmp[32];
    int e;
    if (!buf || size < 26) {
        if (buf && size)
            buf[0] = 0;
        return ucrt_invalid(buf ? UCRT_ERANGE : UCRT_EINVAL);
    }
    e = ucrt_ctime64_s(tmp, sizeof tmp, t);
    if (e) {
        buf[0] = 0;
        return e;
    }
    for (size_t i = 0;; ++i) {
        buf[i] = (unsigned char)tmp[i];
        if (!tmp[i])
            break;
    }
    return 0;
}

/* ---- Disk space ---------------------------------------------------------------------------------------------------------------------------- */

BOOL WINAPI GetDiskFreeSpaceW(const ucrt_wchar *, DWORD *, DWORD *, DWORD *, DWORD *);
DWORD WINAPI GetLogicalDrives(void);

typedef struct uct_diskfree {
    unsigned total_clusters, avail_clusters, sectors_per_cluster, bytes_per_sector;
} uct_diskfree;

/* UCT_EXPORTS _getdiskfree _getdrives */
unsigned w__getdiskfree(unsigned drive, uct_diskfree *out)
{
    ucrt_wchar root[4];
    DWORD spc = 0, bps = 0, free_c = 0, total = 0;
    if (!out || drive > 26) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    if (drive == 0) {
        drive = (unsigned)w__getdrive();
        if (!drive)
            return UCRT_ENOENT;
    }
    root[0] = (ucrt_wchar)('A' + drive - 1);
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &free_c, &total)) {
        DWORD e = GetLastError();
        ucrt_dosmaperr(e);
        return e;
    }
    out->total_clusters = total;
    out->avail_clusters = free_c;
    out->sectors_per_cluster = spc;
    out->bytes_per_sector = bps;
    return 0;
}

unsigned long w__getdrives(void) { return GetLogicalDrives(); }

/* ---- Single precision on the double functions ---------------------------------------------------------------------------------------------- */

/* The results of these are exact in double precision for single-precision arguments, so one rounding to float is the
 * correctly rounded result. */
/* UCT_EXPORTS nextafterf lrintf lroundf llrintf llroundf ilogbf scalbnf scalblnf nanf */

float w_nextafterf(float x, float y)
{
    uint32_t b;
    if (x != x || y != y)
        return x + y;
    if (x == y)
        return y;
    if (x == 0.0f) {
        b = 1;
        float r;
        ucrt_memcpy(&r, &b, 4);
        return y > 0 ? r : -r;
    }
    ucrt_memcpy(&b, &x, 4);
    if ((y > x) == (x > 0))
        ++b;
    else
        --b;
    ucrt_memcpy(&x, &b, 4);
    return x;
}

long w_lrintf(float x) { return ucrt_lrint(x); }
long w_lroundf(float x) { return ucrt_lround(x); }
long long w_llrintf(float x) { return ucrt_llrint(x); }
long long w_llroundf(float x) { return ucrt_llround(x); }
int w_ilogbf(float x) { return ucrt_ilogb(x); }
float w_scalbnf(float x, int n) { return (float)ucrt_scalbn(x, n); }
float w_scalblnf(float x, long n) { return (float)ucrt_scalbn(x, n > 100000 ? 100000 : n < -100000 ? -100000 : (int)n); }
float w_nanf(const char *tag) { return (float)ucrt_nan(tag); }

/* UCT_EXPORTS _cabs */
double w__cabs(const double *z) { return ucrt_hypot_g(z[0], z[1]); }

/* ---- setjmp and longjmp ------------------------------------------------------------------------------------------------------------------------- */

/* The _JUMP_BUFFER of x64 Windows (256 bytes, 16-byte aligned): Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip, MxCsr, FpCsr and
 * Xmm6-Xmm15. This setjmp records the caller's context; longjmp restores it and does not run unwind handlers, which is enough
 * for C code that jumps out of its own callees. */
/* UCT_EXPORTS _setjmp longjmp */
__asm__(".text\n"
        ".globl w__setjmp\n"
        "w__setjmp:\n"
        "    movq %rcx, %r8\n"            /* the buffer */
        "    movq %rdx, 0(%r8)\n"         /* Frame (the second argument, or 0) */
        "    movq %rbx, 8(%r8)\n"
        "    leaq 8(%rsp), %rax\n"        /* the caller's rsp after the return */
        "    movq %rax, 16(%r8)\n"
        "    movq %rbp, 24(%r8)\n"
        "    movq %rsi, 32(%r8)\n"
        "    movq %rdi, 40(%r8)\n"
        "    movq %r12, 48(%r8)\n"
        "    movq %r13, 56(%r8)\n"
        "    movq %r14, 64(%r8)\n"
        "    movq %r15, 72(%r8)\n"
        "    movq (%rsp), %rax\n"         /* the return address */
        "    movq %rax, 80(%r8)\n"
        "    stmxcsr 88(%r8)\n"
        "    fnstcw 92(%r8)\n"
        "    movdqa %xmm6, 96(%r8)\n"
        "    movdqa %xmm7, 112(%r8)\n"
        "    movdqa %xmm8, 128(%r8)\n"
        "    movdqa %xmm9, 144(%r8)\n"
        "    movdqa %xmm10, 160(%r8)\n"
        "    movdqa %xmm11, 176(%r8)\n"
        "    movdqa %xmm12, 192(%r8)\n"
        "    movdqa %xmm13, 208(%r8)\n"
        "    movdqa %xmm14, 224(%r8)\n"
        "    movdqa %xmm15, 240(%r8)\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        ".globl w_longjmp\n"
        "w_longjmp:\n"
        "    movq %rcx, %r8\n"
        "    movl %edx, %eax\n"
        "    testl %eax, %eax\n"
        "    jnz 1f\n"
        "    movl $1, %eax\n"             /* longjmp(buf, 0) returns 1 */
        "1:  movq 8(%r8), %rbx\n"
        "    movq 24(%r8), %rbp\n"
        "    movq 32(%r8), %rsi\n"
        "    movq 40(%r8), %rdi\n"
        "    movq 48(%r8), %r12\n"
        "    movq 56(%r8), %r13\n"
        "    movq 64(%r8), %r14\n"
        "    movq 72(%r8), %r15\n"
        "    ldmxcsr 88(%r8)\n"
        "    fldcw 92(%r8)\n"
        "    movdqa 96(%r8), %xmm6\n"
        "    movdqa 112(%r8), %xmm7\n"
        "    movdqa 128(%r8), %xmm8\n"
        "    movdqa 144(%r8), %xmm9\n"
        "    movdqa 160(%r8), %xmm10\n"
        "    movdqa 176(%r8), %xmm11\n"
        "    movdqa 192(%r8), %xmm12\n"
        "    movdqa 208(%r8), %xmm13\n"
        "    movdqa 224(%r8), %xmm14\n"
        "    movdqa 240(%r8), %xmm15\n"
        "    movq 16(%r8), %rsp\n"
        "    jmpq *80(%r8)\n");
