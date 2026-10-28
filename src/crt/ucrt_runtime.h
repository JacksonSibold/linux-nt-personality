/*
 * ucrt_runtime.h - process-level runtime: command line parsing (argv rules of the Microsoft runtime), the
 * environment (_environ, getenv, _putenv and the _s forms, mirrored to the OS), exit, atexit, abort and the
 * handler hooks (invalid parameter, purecall, signal). Host tests: tests/crt-host/test_runtime.c.
 */
#ifndef UCRT_RUNTIME_H
#define UCRT_RUNTIME_H

#include "ucrt_locale.h"
#include "ucrt_stdlib.h"
#include "ucrt_stdio.h"

typedef struct ucrt_proc_ops {
    void (*terminate)(unsigned code);                          /* ends the process; does not return */
    const ucrt_wchar *(*cmdline)(void);                        /* GetCommandLineW */
    const ucrt_wchar *(*env_block)(void);                      /* "A=B\0C=D\0\0" */
    unsigned (*set_env)(const ucrt_wchar *name, const ucrt_wchar *value); /* value NULL deletes; Win32 error */
} ucrt_proc_ops;

static const ucrt_proc_ops *g_ucrt_proc;

/* ---- Command line ---------------------------------------------------------------------------------------- */

/* Splits a command line into arguments with the rules of the C runtime: the program name ends at the first space or
 * tab, or is a quoted string with no escapes; after that, 2n backslashes before a quote are n backslashes, 2n+1 are n
 * and a literal quote, a quote toggles quoting, and a doubled quote inside quotes is one literal quote.
 * argv receives pointers into buf (both may be NULL to count). Returns the number of arguments; *chars gets the
 * characters (including terminators) needed in buf. */
static inline int ucrt_parse_cmdline(const char *cmd, char **argv, char *buf, size_t *chars)
{
    int argc = 0;
    size_t n = 0;
    const char *p = cmd;
#define UCRT_EMIT(ch) do { char emit_c_ = (char)(ch); if (buf) buf[n] = emit_c_; ++n; } while (0)
    if (!cmd)
        cmd = p = "";
    /* the program name */
    if (argv)
        argv[argc] = buf ? buf + n : NULL;
    ++argc;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"')
            UCRT_EMIT(*p++);
        if (*p == '"')
            ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t')
            UCRT_EMIT(*p++);
    }
    UCRT_EMIT(0);
    for (;;) {
        int inq = 0;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        if (argv)
            argv[argc] = buf ? buf + n : NULL;
        ++argc;
        for (;;) {
            int bs = 0;
            while (*p == '\\') {
                ++bs;
                ++p;
            }
            if (*p == '"') {
                for (int i = 0; i < bs / 2; ++i)
                    UCRT_EMIT('\\');
                if (bs & 1) {
                    UCRT_EMIT('"');
                    ++p;
                } else if (inq && p[1] == '"') {
                    UCRT_EMIT('"');
                    p += 2;
                } else {
                    inq = !inq;
                    ++p;
                }
                continue;
            }
            for (int i = 0; i < bs; ++i)
                UCRT_EMIT('\\');
            if (!*p || (!inq && (*p == ' ' || *p == '\t')))
                break;
            UCRT_EMIT(*p++);
        }
        UCRT_EMIT(0);
    }
    if (argv)
        argv[argc] = NULL;
    if (chars)
        *chars = n;
#undef UCRT_EMIT
    return argc;
}

/* The same rules over a UTF-16 command line (GetCommandLineW), for __wgetmainargs. */
static inline int ucrt_parse_cmdline_w(const ucrt_wchar *cmd, ucrt_wchar **argv, ucrt_wchar *buf, size_t *chars)
{
    static const ucrt_wchar empty_[1] = {0};
    int argc = 0;
    size_t n = 0;
    const ucrt_wchar *p = cmd;
#define UCRT_EMIT(ch) do { ucrt_wchar emit_c_ = (ucrt_wchar)(ch); if (buf) buf[n] = emit_c_; ++n; } while (0)
    if (!cmd)
        cmd = p = empty_;
    /* the program name */
    if (argv)
        argv[argc] = buf ? buf + n : NULL;
    ++argc;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"')
            UCRT_EMIT(*p++);
        if (*p == '"')
            ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t')
            UCRT_EMIT(*p++);
    }
    UCRT_EMIT(0);
    for (;;) {
        int inq = 0;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        if (argv)
            argv[argc] = buf ? buf + n : NULL;
        ++argc;
        for (;;) {
            int bs = 0;
            while (*p == '\\') {
                ++bs;
                ++p;
            }
            if (*p == '"') {
                for (int i = 0; i < bs / 2; ++i)
                    UCRT_EMIT('\\');
                if (bs & 1) {
                    UCRT_EMIT('"');
                    ++p;
                } else if (inq && p[1] == '"') {
                    UCRT_EMIT('"');
                    p += 2;
                } else {
                    inq = !inq;
                    ++p;
                }
                continue;
            }
            for (int i = 0; i < bs; ++i)
                UCRT_EMIT('\\');
            if (!*p || (!inq && (*p == ' ' || *p == '\t')))
                break;
            UCRT_EMIT(*p++);
        }
        UCRT_EMIT(0);
    }
    if (argv)
        argv[argc] = NULL;
    if (chars)
        *chars = n;
#undef UCRT_EMIT
    return argc;
}

/* ---- Environment ----------------------------------------------------------------------------------------- */

typedef struct ucrt_env_state {
    char **narrow;      /* NULL-terminated "NAME=value" strings (what _environ points at) */
    ucrt_wchar **wide;  /* the same, wide */
    int n, cap;
    int loaded;
} ucrt_env_state;

static ucrt_env_state g_ucrt_env;

static inline size_t ucrt_wcslen_(const ucrt_wchar *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static inline char *ucrt_env_narrow(const ucrt_wchar *w)
{
    size_t n = ucrt_wcstombs(NULL, w, 0), len;
    char *s;
    if (n == (size_t)-1) { /* characters the code page cannot hold: keep what converts, as best-fit would */
        len = ucrt_wcslen_(w);
        s = (char *)ucrt_alloc(len + 1);
        if (!s)
            return NULL;
        for (size_t i = 0; i < len; ++i)
            s[i] = w[i] < 0x100 ? (char)w[i] : '?';
        s[len] = 0;
        return s;
    }
    s = (char *)ucrt_alloc(n + 1);
    if (s)
        ucrt_wcstombs(s, w, n + 1);
    return s;
}

static inline ucrt_wchar *ucrt_env_widen(const char *m)
{
    size_t n = ucrt_mbstowcs(NULL, m, 0);
    ucrt_wchar *w;
    if (n == (size_t)-1) {
        size_t len = ucrt_strlen(m);
        w = (ucrt_wchar *)ucrt_alloc((len + 1) * sizeof(ucrt_wchar));
        if (!w)
            return NULL;
        for (size_t i = 0; i <= len; ++i)
            w[i] = (unsigned char)m[i];
        return w;
    }
    w = (ucrt_wchar *)ucrt_alloc((n + 1) * sizeof(ucrt_wchar));
    if (w)
        ucrt_mbstowcs(w, m, n + 1);
    return w;
}

static inline int ucrt_env_grow(void)
{
    if (g_ucrt_env.n + 2 <= g_ucrt_env.cap)
        return 0;
    {
        int ncap = g_ucrt_env.cap ? g_ucrt_env.cap * 2 : 32;
        char **nn = (char **)ucrt_alloc((size_t)ncap * sizeof(char *));
        ucrt_wchar **nw = (ucrt_wchar **)ucrt_alloc((size_t)ncap * sizeof(ucrt_wchar *));
        if (!nn || !nw) {
            ucrt_release(nn);
            ucrt_release(nw);
            return -1;
        }
        for (int i = 0; i < g_ucrt_env.n; ++i) {
            nn[i] = g_ucrt_env.narrow[i];
            nw[i] = g_ucrt_env.wide[i];
        }
        nn[g_ucrt_env.n] = NULL;
        nw[g_ucrt_env.n] = NULL;
        ucrt_release(g_ucrt_env.narrow);
        ucrt_release(g_ucrt_env.wide);
        g_ucrt_env.narrow = nn;
        g_ucrt_env.wide = nw;
        g_ucrt_env.cap = ncap;
    }
    return 0;
}

/* Reads the environment block of the process (once). */
static inline void ucrt_env_load(void)
{
    const ucrt_wchar *b;
    if (g_ucrt_env.loaded)
        return;
    g_ucrt_env.loaded = 1;
    if (ucrt_env_grow())
        return;
    g_ucrt_env.narrow[0] = NULL;
    g_ucrt_env.wide[0] = NULL;
    b = g_ucrt_proc && g_ucrt_proc->env_block ? g_ucrt_proc->env_block() : NULL;
    while (b && *b) {
        size_t len = ucrt_wcslen_(b);
        /* entries that start with '=' are per-drive current directories (=C:=...): not part of the C environment */
        if (b[0] != '=' && ucrt_env_grow() == 0) {
            ucrt_wchar *w = (ucrt_wchar *)ucrt_alloc((len + 1) * sizeof(ucrt_wchar));
            if (w) {
                char *m;
                for (size_t i = 0; i <= len; ++i)
                    w[i] = b[i];
                m = ucrt_env_narrow(w);
                if (m) {
                    g_ucrt_env.narrow[g_ucrt_env.n] = m;
                    g_ucrt_env.wide[g_ucrt_env.n] = w;
                    ++g_ucrt_env.n;
                    g_ucrt_env.narrow[g_ucrt_env.n] = NULL;
                    g_ucrt_env.wide[g_ucrt_env.n] = NULL;
                } else {
                    ucrt_release(w);
                }
            }
        }
        b += len + 1;
    }
}

static inline char **ucrt_environ(void)
{
    ucrt_env_load();
    return g_ucrt_env.narrow;
}

static inline ucrt_wchar **ucrt_wenviron(void)
{
    ucrt_env_load();
    return g_ucrt_env.wide;
}

/* Variable names compare without regard to case. */
static inline int ucrt_env_find(const char *name, size_t len)
{
    for (int i = 0; i < g_ucrt_env.n; ++i) {
        const char *e = g_ucrt_env.narrow[i];
        size_t k = 0;
        for (; k < len; ++k) {
            int a = (unsigned char)e[k], b = (unsigned char)name[k];
            if (a >= 'a' && a <= 'z')
                a -= 32;
            if (b >= 'a' && b <= 'z')
                b -= 32;
            if (a != b || a == 0)
                break;
        }
        if (k == len && e[len] == '=')
            return i;
    }
    return -1;
}

static inline char *ucrt_getenv(const char *name)
{
    int i;
    if (!name || !*name) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    ucrt_env_load();
    i = ucrt_env_find(name, ucrt_strlen(name));
    return i < 0 ? NULL : g_ucrt_env.narrow[i] + ucrt_strlen(name) + 1;
}

static inline ucrt_wchar *ucrt_wgetenv(const ucrt_wchar *name)
{
    char narrow[256];
    size_t n = 0;
    int i;
    if (!name || !*name) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    for (; name[n] && n + 1 < sizeof narrow; ++n)
        narrow[n] = name[n] < 0x100 ? (char)name[n] : '?';
    narrow[n] = 0;
    ucrt_env_load();
    i = ucrt_env_find(narrow, n);
    return i < 0 ? NULL : g_ucrt_env.wide[i] + ucrt_wcslen_(name) + 1;
}

/* _putenv("NAME=value"); "NAME=" removes NAME. Returns 0, or -1 with errno set. */
static inline int ucrt_putenv(const char *entry)
{
    const char *eq;
    size_t namelen;
    int i;
    char *m;
    ucrt_wchar *w;
    ucrt_wchar wname[256];
    if (!entry || !(eq = ucrt_strchr(entry, '=')) || eq == entry) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    namelen = (size_t)(eq - entry);
    if (namelen >= 255) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    ucrt_env_load();
    i = ucrt_env_find(entry, namelen);
    for (size_t k = 0; k < namelen; ++k)
        wname[k] = (unsigned char)entry[k];
    wname[namelen] = 0;
    if (eq[1] == 0) { /* removal */
        if (i >= 0) {
            ucrt_release(g_ucrt_env.narrow[i]);
            ucrt_release(g_ucrt_env.wide[i]);
            for (int k = i; k < g_ucrt_env.n; ++k) {
                g_ucrt_env.narrow[k] = g_ucrt_env.narrow[k + 1];
                g_ucrt_env.wide[k] = g_ucrt_env.wide[k + 1];
            }
            --g_ucrt_env.n;
        }
        if (g_ucrt_proc && g_ucrt_proc->set_env)
            g_ucrt_proc->set_env(wname, NULL);
        return 0;
    }
    m = (char *)ucrt_alloc(ucrt_strlen(entry) + 1);
    if (!m) {
        ucrt_set_errno(UCRT_ENOMEM);
        return -1;
    }
    ucrt_strcpy(m, entry);
    w = ucrt_env_widen(m);
    if (!w) {
        ucrt_release(m);
        ucrt_set_errno(UCRT_ENOMEM);
        return -1;
    }
    if (i >= 0) {
        ucrt_release(g_ucrt_env.narrow[i]);
        ucrt_release(g_ucrt_env.wide[i]);
        g_ucrt_env.narrow[i] = m;
        g_ucrt_env.wide[i] = w;
    } else {
        if (ucrt_env_grow()) {
            ucrt_release(m);
            ucrt_release(w);
            ucrt_set_errno(UCRT_ENOMEM);
            return -1;
        }
        g_ucrt_env.narrow[g_ucrt_env.n] = m;
        g_ucrt_env.wide[g_ucrt_env.n] = w;
        ++g_ucrt_env.n;
        g_ucrt_env.narrow[g_ucrt_env.n] = NULL;
        g_ucrt_env.wide[g_ucrt_env.n] = NULL;
    }
    if (g_ucrt_proc && g_ucrt_proc->set_env)
        g_ucrt_proc->set_env(wname, w + namelen + 1);
    return 0;
}

static inline ucrt_errno_t ucrt_putenv_s(const char *name, const char *value)
{
    size_t nl, vl;
    char *tmp;
    int r;
    if (!name || !*name || !value || ucrt_strchr(name, '=')) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    nl = ucrt_strlen(name);
    vl = ucrt_strlen(value);
    tmp = (char *)ucrt_alloc(nl + vl + 2);
    if (!tmp)
        return UCRT_ENOMEM;
    ucrt_memcpy(tmp, name, nl);
    tmp[nl] = '=';
    ucrt_memcpy(tmp + nl + 1, value, vl + 1);
    r = ucrt_putenv(tmp);
    ucrt_release(tmp);
    return r ? ucrt_get_errno() : 0;
}

/* getenv_s: copies the value into buf; *ret gets the length needed including the NUL. */
static inline ucrt_errno_t ucrt_getenv_s(size_t *ret, char *buf, size_t size, const char *name)
{
    const char *v;
    size_t need;
    if (!ret || (!buf && size) || !name) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    *ret = 0;
    if (buf && size)
        buf[0] = 0;
    v = ucrt_getenv(name);
    if (!v)
        return 0;
    need = ucrt_strlen(v) + 1;
    *ret = need;
    if (!buf || size == 0)
        return 0;
    if (need > size)
        return ucrt_invalid(UCRT_ERANGE);
    ucrt_memcpy(buf, v, need);
    return 0;
}

/* _dupenv_s: allocates a copy of the value (to be released with free). */
static inline ucrt_errno_t ucrt_dupenv_s(char **buf, size_t *len, const char *name)
{
    const char *v;
    if (!buf || !name)
        return ucrt_invalid(UCRT_EINVAL);
    *buf = NULL;
    if (len)
        *len = 0;
    v = ucrt_getenv(name);
    if (!v)
        return 0;
    *buf = (char *)ucrt_alloc(ucrt_strlen(v) + 1);
    if (!*buf)
        return UCRT_ENOMEM;
    ucrt_strcpy(*buf, v);
    if (len)
        *len = ucrt_strlen(v) + 1;
    return 0;
}

/* ---- Exit and handlers ------------------------------------------------------------------------------------- */

typedef void (*ucrt_signal_fn)(int);
typedef void (*ucrt_purecall_fn)(void);
typedef void (*ucrt_invalid_param_fn)(const ucrt_wchar *expr, const ucrt_wchar *func, const ucrt_wchar *file, unsigned line, uintptr_t reserved);

#define UCRT_SIGINT 2
#define UCRT_SIGILL 4
#define UCRT_SIGFPE 8
#define UCRT_SIGSEGV 11
#define UCRT_SIGTERM 15
#define UCRT_SIGBREAK 21
#define UCRT_SIGABRT 22
#define UCRT_SIGABRT_COMPAT 6
#define UCRT_NSIG 23
#define UCRT_SIG_DFL ((ucrt_signal_fn)0)
#define UCRT_SIG_IGN ((ucrt_signal_fn)1)
#define UCRT_SIG_ERR ((ucrt_signal_fn)-1)

#define UCRT_ABORT_CALL_REPORTFAULT 0x2
#define UCRT_ABORT_WRITE_MSG 0x1

typedef struct ucrt_proc_state {
    ucrt_onexit_table atexit_table;
    int exiting;
    ucrt_signal_fn signals[UCRT_NSIG];
    ucrt_purecall_fn purecall;
    ucrt_invalid_param_fn invalid_handler;
    unsigned abort_behavior;
    int app_type;
    int error_mode;
    int fmode;
} ucrt_proc_state;

static ucrt_proc_state g_ucrt_pr = {{0, 0, 0}, 0, {0}, 0, 0, 3, 0, 0, 0};

/* atexit table registration needs realloc; build it on the allocator with a size header. */
typedef struct ucrt_atexit_blk {
    size_t cap_bytes;
} ucrt_atexit_blk;

static inline void *ucrt_realloc_tracked(void *p, size_t n)
{
    ucrt_atexit_blk *old = p ? (ucrt_atexit_blk *)p - 1 : NULL, *nb;
    nb = (ucrt_atexit_blk *)ucrt_alloc(n + sizeof(ucrt_atexit_blk));
    if (!nb)
        return NULL;
    nb->cap_bytes = n;
    if (old) {
        size_t keep = old->cap_bytes < n ? old->cap_bytes : n;
        ucrt_memcpy(nb + 1, p, keep);
        ucrt_release(old);
    }
    return nb + 1;
}

static inline void ucrt_free_tracked(void *p)
{
    if (p)
        ucrt_release((ucrt_atexit_blk *)p - 1);
}

static inline int ucrt_atexit(ucrt_onexit_fn fn)
{
    return ucrt_register_onexit(&g_ucrt_pr.atexit_table, fn, ucrt_realloc_tracked) == 0 ? 0 : -1;
}

/* _onexit returns the function on success and NULL on failure. */
static inline ucrt_onexit_fn ucrt_onexit(ucrt_onexit_fn fn)
{
    return ucrt_atexit(fn) == 0 ? fn : NULL;
}

static inline void ucrt_exit_process(unsigned code)
{
    if (g_ucrt_proc && g_ucrt_proc->terminate)
        g_ucrt_proc->terminate(code);
    for (;;) {
    }
}

/* _exit and _Exit: end the process at once, no handlers, no flushing. */
static inline void ucrt_exit_now(int code)
{
    ucrt_exit_process((unsigned)code);
}

/* exit: run the atexit functions (last registered first), flush and close the streams, end the process. */
static inline void ucrt_exit(int code)
{
    if (!g_ucrt_pr.exiting) {
        g_ucrt_pr.exiting = 1;
        ucrt_execute_onexit(&g_ucrt_pr.atexit_table, ucrt_free_tracked);
        ucrt_flushall();
    }
    ucrt_exit_process((unsigned)code);
}

/* _cexit: the cleanup of exit without ending the process. */
static inline void ucrt_cexit(void)
{
    ucrt_execute_onexit(&g_ucrt_pr.atexit_table, ucrt_free_tracked);
    ucrt_flushall();
}

static inline ucrt_signal_fn ucrt_signal(int sig, ucrt_signal_fn fn)
{
    ucrt_signal_fn old;
    if (sig == UCRT_SIGABRT_COMPAT)
        sig = UCRT_SIGABRT;
    if (!(sig == UCRT_SIGINT || sig == UCRT_SIGILL || sig == UCRT_SIGFPE || sig == UCRT_SIGSEGV || sig == UCRT_SIGTERM ||
          sig == UCRT_SIGBREAK || sig == UCRT_SIGABRT) ||
        fn == UCRT_SIG_ERR) {
        ucrt_invalid(UCRT_EINVAL);
        return UCRT_SIG_ERR;
    }
    old = g_ucrt_pr.signals[sig];
    g_ucrt_pr.signals[sig] = fn;
    return old;
}

/* The message abort and the default signal actions write, and the process exit code 3. */
static inline int ucrt_raise(int sig)
{
    ucrt_signal_fn h;
    if (sig == UCRT_SIGABRT_COMPAT)
        sig = UCRT_SIGABRT;
    if (!(sig == UCRT_SIGINT || sig == UCRT_SIGILL || sig == UCRT_SIGFPE || sig == UCRT_SIGSEGV || sig == UCRT_SIGTERM ||
          sig == UCRT_SIGBREAK || sig == UCRT_SIGABRT)) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    h = g_ucrt_pr.signals[sig];
    if (h == UCRT_SIG_IGN)
        return 0;
    if (h == UCRT_SIG_DFL) {
        ucrt_exit_process(sig == UCRT_SIGABRT ? 3u : 3u);
        return 0;
    }
    g_ucrt_pr.signals[sig] = UCRT_SIG_DFL; /* a handler is reset before it runs, as on Windows */
    h(sig);
    return 0;
}

static inline void ucrt_abort(void)
{
    if (g_ucrt_pr.abort_behavior & UCRT_ABORT_WRITE_MSG) {
        static const char msg[] = "\nThis application has requested the Runtime to terminate it in an unusual way.\nPlease contact the application's support team for more information.\n";
        ucrt_write(2, msg, (unsigned)(sizeof msg - 1));
    }
    ucrt_raise(UCRT_SIGABRT);
    ucrt_exit_process(3);
}

static inline unsigned ucrt_set_abort_behavior(unsigned flags, unsigned mask)
{
    unsigned old = g_ucrt_pr.abort_behavior;
    g_ucrt_pr.abort_behavior = (old & ~mask) | (flags & mask);
    return old;
}

static inline ucrt_invalid_param_fn ucrt_set_invalid_parameter_handler(ucrt_invalid_param_fn fn)
{
    ucrt_invalid_param_fn old = g_ucrt_pr.invalid_handler;
    g_ucrt_pr.invalid_handler = fn;
    return old;
}

static inline ucrt_purecall_fn ucrt_set_purecall_handler(ucrt_purecall_fn fn)
{
    ucrt_purecall_fn old = g_ucrt_pr.purecall;
    g_ucrt_pr.purecall = fn;
    return old;
}

static inline int ucrt_purecall(void)
{
    if (g_ucrt_pr.purecall)
        g_ucrt_pr.purecall();
    ucrt_abort();
    return 0;
}

/* The handler the invalid-parameter path of every runtime function runs: the user's, or the default that ends the
 * process (the Windows default raises a fatal exception; with no installed handler the process exits with 0xC000000D). */
static inline void ucrt_invalid_dispatch(void)
{
    if (g_ucrt_pr.invalid_handler)
        g_ucrt_pr.invalid_handler(NULL, NULL, NULL, 0, 0);
    else
        ucrt_exit_process(0xC000000Du);
}

static inline void ucrt_runtime_init(void)
{
    g_ucrt_state.invalid_handler = ucrt_invalid_dispatch;
}

/* ---- Startup helpers ------------------------------------------------------------------------------------------ */

typedef void (*ucrt_initterm_fn)(void);
typedef int (*ucrt_inittermE_fn)(void);

static inline void ucrt_initterm(ucrt_initterm_fn *first, ucrt_initterm_fn *last)
{
    for (; first < last; ++first)
        if (*first)
            (*first)();
}

static inline int ucrt_initterm_e(ucrt_inittermE_fn *first, ucrt_inittermE_fn *last)
{
    for (; first < last; ++first) {
        if (*first) {
            int r = (*first)();
            if (r)
                return r;
        }
    }
    return 0;
}

#endif /* UCRT_RUNTIME_H */
