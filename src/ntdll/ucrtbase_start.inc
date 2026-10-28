/* ucrtbase_start.inc - process startup and shutdown support (included by ucrtbase.c): argc/argv, the environment tables,
 * the atexit tables an executable registers through, the error accessors, threads started with _beginthreadex and the
 * invalid-parameter entry points. */

HANDLE WINAPI CreateThread(void *, size_t, unsigned long (*)(void *), void *, DWORD, DWORD *);
void WINAPI ExitThread(DWORD);
DWORD WINAPI GetModuleFileNameW(HANDLE, ucrt_wchar *, DWORD);
BOOL WINAPI QueryPerformanceCounter(int64_t *);
DWORD WINAPI GetCurrentThreadId(void);

/* ---- errno ------------------------------------------------------------------------------------------------------------- */

int *w__errno(void) { return &uct_thr()->err; }
unsigned long *w___doserrno(void) { return &uct_thr()->dos; }

int w__get_errno(int *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = ucrt_get_errno();
    return 0;
}

int w__set_errno(int e)
{
    ucrt_set_errno(e);
    return 0;
}

int w__get_doserrno(unsigned long *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = *ucrt_doserrno_ptr();
    return 0;
}

int w__set_doserrno(unsigned long e)
{
    ucrt_set_doserrno(e);
    return 0;
}

/* ---- Arguments and the environment ---------------------------------------------------------------------------------------- */

static int g_uct_argc;
static char **g_uct_argv;
static ucrt_wchar **g_uct_wargv;
static char *g_uct_acmdln;
static const ucrt_wchar *g_uct_wcmdln;
static char **g_uct_environ_var;
static ucrt_wchar **g_uct_wenviron_var;
static int g_uct_args_ready;

/* Splits GetCommandLineW once: the wide argument vector, and a narrow one converted from it. */
static void uct_args_init(void)
{
    size_t chars = 0;
    ucrt_wchar *wbuf;
    int argc, i;
    if (g_uct_args_ready)
        return;
    g_uct_args_ready = 1;
    g_uct_wcmdln = GetCommandLineW();
    argc = ucrt_parse_cmdline_w(g_uct_wcmdln, NULL, NULL, &chars);
    g_uct_wargv = (ucrt_wchar **)ucrt_alloc((size_t)(argc + 1) * sizeof(ucrt_wchar *));
    wbuf = (ucrt_wchar *)ucrt_alloc(chars * sizeof(ucrt_wchar));
    g_uct_argv = (char **)ucrt_alloc((size_t)(argc + 1) * sizeof(char *));
    if (!g_uct_wargv || !wbuf || !g_uct_argv) {
        g_uct_argc = 0;
        return;
    }
    ucrt_parse_cmdline_w(g_uct_wcmdln, g_uct_wargv, wbuf, &chars);
    for (i = 0; i < argc; ++i)
        g_uct_argv[i] = ucrt_env_narrow(g_uct_wargv[i]);
    g_uct_argv[argc] = NULL;
    g_uct_argc = argc;
    g_uct_acmdln = ucrt_env_narrow(g_uct_wcmdln);
}

int *w___p___argc(void)
{
    uct_args_init();
    return &g_uct_argc;
}

char ***w___p___argv(void)
{
    uct_args_init();
    return &g_uct_argv;
}

ucrt_wchar ***w___p___wargv(void)
{
    uct_args_init();
    return &g_uct_wargv;
}

char **w___p__acmdln(void)
{
    uct_args_init();
    return &g_uct_acmdln;
}

ucrt_wchar **w___p__wcmdln(void)
{
    uct_args_init();
    return (ucrt_wchar **)(void *)&g_uct_wcmdln;
}

/* _environ and _wenviron are variables the program reads through these accessors; refresh them at each call because the
 * tables are reallocated when the environment grows. */
char ***w___p__environ(void)
{
    g_uct_environ_var = ucrt_environ();
    return &g_uct_environ_var;
}

ucrt_wchar ***w___p__wenviron(void)
{
    g_uct_wenviron_var = ucrt_wenviron();
    return &g_uct_wenviron_var;
}

char **w__get_initial_narrow_environment(void) { return ucrt_environ(); }
ucrt_wchar **w__get_initial_wide_environment(void) { return ucrt_wenviron(); }

int w__initialize_narrow_environment(void)
{
    ucrt_environ();
    return 0;
}

int w__initialize_wide_environment(void)
{
    ucrt_wenviron();
    return 0;
}

int w__configure_narrow_argv(int mode)
{
    (void)mode; /* wildcard expansion (mode 2) is not performed */
    uct_args_init();
    return 0;
}

int w__configure_wide_argv(int mode)
{
    (void)mode;
    uct_args_init();
    return 0;
}

/* The older entry point of msvcrt.dll, for programs that link to it by name. */
int w___getmainargs(int *argc, char ***argv, char ***env, int wildcard, void *startinfo)
{
    (void)wildcard;
    (void)startinfo;
    uct_args_init();
    *argc = g_uct_argc;
    *argv = g_uct_argv;
    *env = ucrt_environ();
    return 0;
}

int w___wgetmainargs(int *argc, ucrt_wchar ***argv, ucrt_wchar ***env, int wildcard, void *startinfo)
{
    (void)wildcard;
    (void)startinfo;
    uct_args_init();
    *argc = g_uct_argc;
    *argv = g_uct_wargv;
    *env = ucrt_wenviron();
    return 0;
}

static ucrt_wchar g_uct_wpgm[520];
static char *g_uct_pgm;
static char *g_uct_pgm_ptr;
static ucrt_wchar *g_uct_wpgm_ptr = g_uct_wpgm;

static void uct_pgm_init(void)
{
    if (g_uct_pgm)
        return;
    GetModuleFileNameW(NULL, g_uct_wpgm, 520);
    g_uct_pgm = ucrt_env_narrow(g_uct_wpgm);
    g_uct_pgm_ptr = g_uct_pgm;
}

char **w___p__pgmptr(void)
{
    uct_pgm_init();
    return &g_uct_pgm_ptr;
}

ucrt_wchar **w___p__wpgmptr(void)
{
    uct_pgm_init();
    return &g_uct_wpgm_ptr;
}

int w__get_pgmptr(char **out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    uct_pgm_init();
    *out = g_uct_pgm_ptr;
    return 0;
}

int w__get_wpgmptr(ucrt_wchar **out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    uct_pgm_init();
    *out = g_uct_wpgm_ptr;
    return 0;
}

/* ---- Application mode ----------------------------------------------------------------------------------------------------------- */

static int g_uct_commode;

void w__set_app_type(int type) { g_ucrt_pr.app_type = type; }
int w__get_app_type(void) { return g_ucrt_pr.app_type; }
int *w___p__commode(void) { return &g_uct_commode; }

int w__set_commode(int mode)
{
    g_uct_commode = mode;
    return 0;
}

int w__set_fmode(int mode)
{
    if (mode != UCRT_O_TEXT && mode != UCRT_O_BINARY) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    g_ucrt_io.fmode = mode;
    return 0;
}

int w__get_fmode(int *out)
{
    if (!out)
        return ucrt_invalid(UCRT_EINVAL);
    *out = g_ucrt_io.fmode;
    return 0;
}

int *w___p__fmode(void) { return &g_ucrt_io.fmode; }

int w__set_error_mode(int mode)
{
    int old = g_ucrt_pr.error_mode;
    if (mode >= 0 && mode <= 2)
        g_ucrt_pr.error_mode = mode;
    return old;
}

/* ---- Exit handlers -------------------------------------------------------------------------------------------------------------- */

static ucrt_onexit_table g_uct_quick_table;

int w__crt_atexit(ucrt_onexit_fn fn) { return ucrt_atexit(fn); }

int w__crt_at_quick_exit(ucrt_onexit_fn fn)
{
    return ucrt_register_onexit(&g_uct_quick_table, fn, ucrt_realloc_tracked) == 0 ? 0 : -1;
}

int w_at_quick_exit(ucrt_onexit_fn fn) { return w__crt_at_quick_exit(fn); }

void w_quick_exit(int code)
{
    ucrt_execute_onexit(&g_uct_quick_table, ucrt_free_tracked);
    ucrt_exit_now(code);
}

/* The tables a module keeps for its own atexit functions: the module owns the three-pointer structure. */
int w__initialize_onexit_table(ucrt_onexit_table *t)
{
    if (!t)
        return -1;
    return ucrt_initialize_onexit_table(t);
}

int w__register_onexit_function(ucrt_onexit_table *t, ucrt_onexit_fn fn)
{
    if (!t)
        return -1;
    return ucrt_register_onexit(t, fn, ucrt_realloc_tracked) == 0 ? 0 : -1;
}

int w__execute_onexit_table(ucrt_onexit_table *t)
{
    if (!t)
        return -1;
    return ucrt_execute_onexit(t, ucrt_free_tracked);
}

int w__register_thread_local_exe_atexit_callback(void *cb)
{
    (void)cb;
    return 0;
}

/* ---- Invalid parameters and fatal errors ---------------------------------------------------------------------------------------- */

void w__invalid_parameter(const ucrt_wchar *expr, const ucrt_wchar *func, const ucrt_wchar *file, unsigned line, uintptr_t reserved)
{
    if (g_ucrt_pr.invalid_handler)
        g_ucrt_pr.invalid_handler(expr, func, file, line, reserved);
    else
        ucrt_exit_process(0xC000000Du);
}

void w__invalid_parameter_noinfo(void) { w__invalid_parameter(NULL, NULL, NULL, 0, 0); }

void w__invalid_parameter_noinfo_noreturn(void)
{
    w__invalid_parameter(NULL, NULL, NULL, 0, 0);
    ucrt_exit_process(0xC000000Du);
}

void w__invoke_watson(const ucrt_wchar *expr, const ucrt_wchar *func, const ucrt_wchar *file, unsigned line, uintptr_t reserved)
{
    w__invalid_parameter(expr, func, file, line, reserved);
    ucrt_exit_process(0xC000000Du);
}

ucrt_invalid_param_fn w__get_invalid_parameter_handler(void) { return g_ucrt_pr.invalid_handler; }
ucrt_invalid_param_fn w__set_thread_local_invalid_parameter_handler(ucrt_invalid_param_fn h)
{
    return ucrt_set_invalid_parameter_handler(h);
}
ucrt_invalid_param_fn w__get_thread_local_invalid_parameter_handler(void) { return g_ucrt_pr.invalid_handler; }
ucrt_purecall_fn w__get_purecall_handler(void) { return g_ucrt_pr.purecall; }

void w_terminate(void) { ucrt_abort(); }
void w__crt_debugger_hook(int reserved) { (void)reserved; }
int w__seh_filter_exe(unsigned long code, void *pointers)
{
    (void)code;
    (void)pointers;
    return 0; /* EXCEPTION_CONTINUE_SEARCH: the unhandled-exception path takes it */
}
int w__seh_filter_dll(unsigned long code, void *pointers) { return w__seh_filter_exe(code, pointers); }

/* ---- Threads -------------------------------------------------------------------------------------------------------------------- */

typedef struct uct_thread_start {
    unsigned (*fn)(void *);
    void (*fn_void)(void *);
    void *arg;
} uct_thread_start;

static unsigned long uct_thread_main(void *p)
{
    uct_thread_start s = *(uct_thread_start *)p;
    unsigned r = 0;
    HeapFree(GetProcessHeap(), 0, p);
    if (s.fn)
        r = s.fn(s.arg);
    else
        s.fn_void(s.arg);
    return r;
}

uintptr_t w__beginthreadex(void *security, unsigned stack, unsigned (*fn)(void *), void *arg, unsigned flags, unsigned *tid)
{
    uct_thread_start *s;
    HANDLE h;
    DWORD id = 0;
    if (!fn) {
        ucrt_set_errno(UCRT_EINVAL);
        return 0;
    }
    s = (uct_thread_start *)HeapAlloc(GetProcessHeap(), 0, sizeof *s);
    if (!s) {
        ucrt_set_errno(UCRT_EAGAIN);
        return 0;
    }
    s->fn = fn;
    s->fn_void = NULL;
    s->arg = arg;
    h = CreateThread(security, stack, uct_thread_main, s, flags, &id);
    if (!h) {
        HeapFree(GetProcessHeap(), 0, s);
        ucrt_dosmaperr(GetLastError());
        return 0;
    }
    if (tid)
        *tid = id;
    return (uintptr_t)h;
}

uintptr_t w__beginthread(void (*fn)(void *), unsigned stack, void *arg)
{
    uct_thread_start *s;
    HANDLE h;
    if (!fn) {
        ucrt_set_errno(UCRT_EINVAL);
        return (uintptr_t)-1;
    }
    s = (uct_thread_start *)HeapAlloc(GetProcessHeap(), 0, sizeof *s);
    if (!s) {
        ucrt_set_errno(UCRT_EAGAIN);
        return (uintptr_t)-1;
    }
    s->fn = NULL;
    s->fn_void = fn;
    s->arg = arg;
    h = CreateThread(NULL, stack, uct_thread_main, s, 0, NULL);
    if (!h) {
        HeapFree(GetProcessHeap(), 0, s);
        ucrt_dosmaperr(GetLastError());
        return (uintptr_t)-1;
    }
    return (uintptr_t)h;
}

void w__endthreadex(unsigned code) { ExitThread(code); }
void w__endthread(void) { ExitThread(0); }

/* ---- Random numbers ------------------------------------------------------------------------------------------------------------- */

__attribute__((target("rdrnd"))) static int uct_rdrand32(unsigned *out)
{
    for (int i = 0; i < 10; ++i)
        if (__builtin_ia32_rdrand32_step(out))
            return 1;
    return 0;
}

static int uct_has_rdrand(void)
{
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (c >> 30) & 1;
}

/* rand_s: the hardware generator when there is one, else a xorshift stream seeded from the performance counter and
 * the thread, which is not cryptographic. */
int w_rand_s(unsigned *out)
{
    static uint64_t state;
    if (!out) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_EINVAL;
    }
    if (uct_has_rdrand() && uct_rdrand32(out))
        return 0;
    if (!state) {
        int64_t q = 0;
        QueryPerformanceCounter(&q);
        state = (uint64_t)q * 0x9E3779B97F4A7C15ull ^ ((uint64_t)GetCurrentThreadId() << 32) ^ 0x2545F4914F6CDD1Dull;
        if (!state)
            state = 1;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    *out = (unsigned)(state >> 16);
    return 0;
}

/* ---- Bit rotation ------------------------------------------------------------------------------------------------------------ */

unsigned w__rotl(unsigned v, int s)
{
    s &= 31;
    return s ? (v << s) | (v >> (32 - s)) : v;
}

unsigned w__rotr(unsigned v, int s)
{
    s &= 31;
    return s ? (v >> s) | (v << (32 - s)) : v;
}

unsigned long w__lrotl(unsigned long v, int s) { return w__rotl(v, s); }
unsigned long w__lrotr(unsigned long v, int s) { return w__rotr(v, s); }

unsigned long long w__rotl64(unsigned long long v, int s)
{
    s &= 63;
    return s ? (v << s) | (v >> (64 - s)) : v;
}

unsigned long long w__rotr64(unsigned long long v, int s)
{
    s &= 63;
    return s ? (v >> s) | (v << (64 - s)) : v;
}

unsigned char w__rotl8(unsigned char v, unsigned char s)
{
    s &= 7;
    return s ? (unsigned char)((v << s) | (v >> (8 - s))) : v;
}

unsigned char w__rotr8(unsigned char v, unsigned char s)
{
    s &= 7;
    return s ? (unsigned char)((v >> s) | (v << (8 - s))) : v;
}

unsigned short w__rotl16(unsigned short v, unsigned char s)
{
    s &= 15;
    return s ? (unsigned short)((v << s) | (v >> (16 - s))) : v;
}

unsigned short w__rotr16(unsigned short v, unsigned char s)
{
    s &= 15;
    return s ? (unsigned short)((v >> s) | (v << (16 - s))) : v;
}
