/*
 * test_runtime.c - tests for src/crt/ucrt_runtime.h: command line splitting (against the documented rules and
 * the cases of CommandLineToArgvW), the environment, exit and atexit ordering, signals, handlers.
 */
#include "minitest.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix_os.h"
#include "ucrt_runtime.h"

#define MT_FAIL(...) do { char fb_[900]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

/* ---- the process backend of the tests ---- */
static jmp_buf exit_jmp;
static unsigned exit_code;
static int exited;
static ucrt_wchar env_block[512];
static struct { char name[64]; char value[128]; int deleted; } env_log[512];
static int env_log_n;

static void t_terminate(unsigned code)
{
    exit_code = code;
    exited = 1;
    longjmp(exit_jmp, 1);
}

static const ucrt_wchar *t_cmdline(void) { return NULL; }
static const ucrt_wchar *t_env_block(void) { return env_block; }

static unsigned t_set_env(const ucrt_wchar *name, const ucrt_wchar *value)
{
    size_t i = 0;
    for (; name[i]; ++i)
        env_log[env_log_n].name[i] = (char)name[i];
    env_log[env_log_n].name[i] = 0;
    env_log[env_log_n].deleted = value == NULL;
    if (value) {
        for (i = 0; value[i]; ++i)
            env_log[env_log_n].value[i] = (char)value[i];
        env_log[env_log_n].value[i] = 0;
    }
    ++env_log_n;
    return 0;
}

static const ucrt_proc_ops proc_ops = {t_terminate, t_cmdline, t_env_block, t_set_env};

static void set_env_block(const char *const *entries)
{
    size_t n = 0;
    for (; *entries; ++entries) {
        for (const char *p = *entries; *p; ++p)
            env_block[n++] = (unsigned char)*p;
        env_block[n++] = 0;
    }
    env_block[n++] = 0;
}

static void reset_env(void)
{
    for (int i = 0; i < g_ucrt_env.n; ++i) {
        free(g_ucrt_env.narrow[i]);
        free(g_ucrt_env.wide[i]);
    }
    free(g_ucrt_env.narrow);
    free(g_ucrt_env.wide);
    memset(&g_ucrt_env, 0, sizeof g_ucrt_env);
    env_log_n = 0;
}

static void setup(void)
{
    g_ucrt_proc = &proc_ops;
    g_ucrt_malloc = malloc;
    g_ucrt_free = free;
    g_ucrt_os = &posix_os;
    g_ucrt_loc.initialised = 0;
    ucrt_setlocale(UCRT_LC_ALL, "C");
    exited = 0;
}

/* ---- command line ---- */
static void expect_args(const char *cmd, int n, ...)
{
    char *argv[32];
    char buf[512];
    size_t chars = 0;
    int argc = ucrt_parse_cmdline(cmd, argv, buf, &chars);
    va_list ap;
    int ok = argc == n;
    va_start(ap, n);
    for (int i = 0; i < n && ok; ++i) {
        const char *want = va_arg(ap, const char *);
        if (strcmp(argv[i], want) != 0)
            ok = 0;
    }
    va_end(ap);
    if (!ok) {
        char got[400];
        size_t k = 0;
        for (int i = 0; i < argc && k < 300; ++i)
            k += (size_t)snprintf(got + k, sizeof got - k, "[%s]", argv[i]);
        MT_FAIL("cmdline <%s>: got %d args %s", cmd, argc, got);
    }
    /* counting mode agrees */
    {
        size_t c2 = 0;
        MT_EXPECT_EQ(ucrt_parse_cmdline(cmd, NULL, NULL, &c2), n);
        MT_EXPECT(c2 == chars);
    }
}

static void command_line_rules(void)
{
    expect_args("prog.exe", 1, "prog.exe");
    expect_args("prog.exe a b  c", 4, "prog.exe", "a", "b", "c");
    expect_args("\"C:\\Program Files\\x.exe\" arg", 2, "C:\\Program Files\\x.exe", "arg");
    expect_args("prog \"a b\" c", 3, "prog", "a b", "c");
    expect_args("prog a\"b c\"d", 2, "prog", "ab cd");
    expect_args("prog \"a\\\"b\"", 2, "prog", "a\"b");           /* \" is a literal quote */
    expect_args("prog a\\\\\\\"b", 2, "prog", "a\\\"b");         /* three backslashes and a quote: one and a quote */
    expect_args("prog a\\\\\"b c\"", 2, "prog", "a\\b c");        /* two backslashes then a quote: one backslash, quoting */
    expect_args("prog a\\b\\c", 2, "prog", "a\\b\\c");            /* backslashes not before a quote are literal */
    expect_args("prog \"a \"\"quoted\"\" b\"", 2, "prog", "a \"quoted\" b"); /* a doubled quote inside quotes */
    expect_args("prog \"\"", 2, "prog", "");                       /* an empty quoted argument */
    expect_args("prog \"unterminated arg", 2, "prog", "unterminated arg");
    expect_args("  prog", 2, "", "prog");                          /* leading blanks end the (empty) program name */
    expect_args("prog\ta\tb", 3, "prog", "a", "b");
    expect_args("\"prog\"a", 2, "prog", "a");
    expect_args("", 1, "");
}

/* ---- environment ---- */
static void environment_basics(void)
{
    static const char *const entries[] = {"=C:=C:\\work", "PATH=C:\\bin;C:\\tools", "Home=D:\\me", "EMPTY=", NULL};
    setup();
    reset_env();
    set_env_block(entries);
    MT_EXPECT(strcmp(ucrt_getenv("PATH"), "C:\\bin;C:\\tools") == 0);
    MT_EXPECT(strcmp(ucrt_getenv("path"), "C:\\bin;C:\\tools") == 0); /* case does not matter */
    MT_EXPECT(strcmp(ucrt_getenv("HOME"), "D:\\me") == 0);
    MT_EXPECT(strcmp(ucrt_getenv("EMPTY"), "") == 0);
    MT_EXPECT(ucrt_getenv("NOPE") == NULL);
    MT_EXPECT(ucrt_getenv("=C:") == NULL); /* the per-drive entries are not part of the C environment */
    MT_EXPECT(ucrt_getenv("PAT") == NULL && ucrt_getenv("PATHX") == NULL);
    MT_EXPECT_EQ(g_ucrt_env.n, 3);
    MT_EXPECT(ucrt_environ()[3] == NULL && ucrt_wenviron()[3] == NULL);
    MT_EXPECT(ucrt_wenviron()[1][0] == 'H' && ucrt_wenviron()[1][4] == '=');
    {
        static const ucrt_wchar wn[] = {'p', 'a', 't', 'h', 0};
        ucrt_wchar *v = ucrt_wgetenv(wn);
        MT_ASSERT(v != NULL);
        MT_EXPECT(v[0] == 'C' && v[1] == ':');
    }
    reset_env();
}

static void environment_changes(void)
{
    static const char *const entries[] = {"A=1", "B=2", NULL};
    setup();
    reset_env();
    set_env_block(entries);
    MT_EXPECT_EQ(ucrt_putenv("C=3"), 0);
    MT_EXPECT(strcmp(ucrt_getenv("C"), "3") == 0);
    MT_EXPECT_EQ(g_ucrt_env.n, 3);
    MT_EXPECT_EQ(ucrt_putenv("a=changed"), 0); /* replaces A, whatever the case */
    MT_EXPECT(strcmp(ucrt_getenv("A"), "changed") == 0);
    MT_EXPECT_EQ(g_ucrt_env.n, 3);
    MT_EXPECT_EQ(ucrt_putenv("B="), 0); /* removal */
    MT_EXPECT(ucrt_getenv("B") == NULL);
    MT_EXPECT_EQ(g_ucrt_env.n, 2);
    MT_EXPECT_EQ(ucrt_putenv("NOSUCH="), 0); /* removing what is not there is fine */
    MT_EXPECT_EQ(ucrt_putenv("noequals"), -1);
    MT_EXPECT_EQ(ucrt_putenv("=x"), -1);
    MT_EXPECT_EQ(ucrt_putenv(NULL), -1);
    /* the OS is told */
    MT_EXPECT_EQ(env_log_n, 4);
    MT_EXPECT(strcmp(env_log[0].name, "C") == 0 && strcmp(env_log[0].value, "3") == 0 && !env_log[0].deleted);
    MT_EXPECT(strcmp(env_log[2].name, "B") == 0 && env_log[2].deleted);
    /* many variables: the table grows */
    for (int i = 0; i < 200; ++i) {
        char e[32];
        snprintf(e, sizeof e, "VAR%d=%d", i, i * 2);
        MT_ASSERT(ucrt_putenv(e) == 0);
    }
    MT_EXPECT(strcmp(ucrt_getenv("VAR150"), "300") == 0);
    MT_EXPECT_EQ(g_ucrt_env.n, 202);
    /* the secure forms */
    {
        size_t need;
        char buf[8];
        char *dup;
        MT_EXPECT_EQ(ucrt_getenv_s(&need, buf, sizeof buf, "VAR7"), 0);
        MT_EXPECT(need == 3 && strcmp(buf, "14") == 0);
        MT_EXPECT_EQ(ucrt_getenv_s(&need, buf, 2, "VAR150"), UCRT_ERANGE);
        MT_EXPECT_EQ((long)need, 4);
        MT_EXPECT_EQ(ucrt_getenv_s(&need, buf, sizeof buf, "NONE"), 0);
        MT_EXPECT(need == 0 && buf[0] == 0);
        MT_EXPECT_EQ(ucrt_dupenv_s(&dup, &need, "VAR9"), 0);
        MT_EXPECT(strcmp(dup, "18") == 0 && need == 3);
        free(dup);
        MT_EXPECT_EQ(ucrt_putenv_s("SEC", "val"), 0);
        MT_EXPECT(strcmp(ucrt_getenv("SEC"), "val") == 0);
        MT_EXPECT_EQ(ucrt_putenv_s("BAD=NAME", "x"), UCRT_EINVAL);
    }
    reset_env();
}

/* ---- exit ---- */
static char order[64];
static int order_n;
static void a1(void) { order[order_n++] = '1'; }
static void a2(void) { order[order_n++] = '2'; }
static void a3(void) { order[order_n++] = '3'; }

static void exit_runs_handlers_in_reverse(void)
{
    setup();
    g_ucrt_pr.atexit_table = (ucrt_onexit_table){0, 0, 0};
    g_ucrt_pr.exiting = 0;
    order_n = 0;
    MT_EXPECT_EQ(ucrt_atexit(a1), 0);
    MT_EXPECT_EQ(ucrt_atexit(a2), 0);
    MT_EXPECT(ucrt_onexit(a3) == a3);
    for (int i = 0; i < 30; ++i)
        ucrt_atexit(a1);
    if (setjmp(exit_jmp) == 0)
        ucrt_exit(7);
    MT_EXPECT(exited && exit_code == 7);
    MT_EXPECT_EQ(order_n, 33);
    MT_EXPECT(order[30] == '3' && order[31] == '2' && order[32] == '1');
    /* a second exit (from a handler or after) does not run them again */
    order_n = 0;
    if (setjmp(exit_jmp) == 0)
        ucrt_exit(8);
    MT_EXPECT_EQ(order_n, 0);
    MT_EXPECT_EQ(exit_code, 8);
    g_ucrt_pr.exiting = 0;
}

static void exit_flushes_streams(void)
{
    char dir[64], path[128];
    ucrt_file *f;
    FILE *raw;
    char buf[16] = {0};
    snprintf(dir, sizeof dir, "/tmp/crt_rt_%d", (int)getpid());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/f", dir);
    setup();
    g_ucrt_io.initialised = 0;
    g_ucrt_stdio.initialised = 0;
    ucrt_stdio_init();
    g_ucrt_pr.atexit_table = (ucrt_onexit_table){0, 0, 0};
    g_ucrt_pr.exiting = 0;
    f = ucrt_fopen(path, "wb");
    MT_ASSERT(f != NULL);
    ucrt_fputs("pending", f);
    if (setjmp(exit_jmp) == 0)
        ucrt_exit(0);
    raw = fopen(path, "rb");
    MT_ASSERT(raw != NULL);
    MT_EXPECT_EQ((long)fread(buf, 1, 15, raw), 7);
    fclose(raw);
    ucrt_fcloseall();
    unlink(path);
    rmdir(dir);
    g_ucrt_pr.exiting = 0;
}

static int sig_seen;
static void on_sig(int s) { sig_seen = s; }

static void signals_and_abort(void)
{
    setup();
    memset(g_ucrt_pr.signals, 0, sizeof g_ucrt_pr.signals);
    MT_EXPECT(ucrt_signal(UCRT_SIGINT, on_sig) == UCRT_SIG_DFL);
    MT_EXPECT(ucrt_signal(UCRT_SIGINT, UCRT_SIG_IGN) == on_sig);
    MT_EXPECT_EQ(ucrt_raise(UCRT_SIGINT), 0);
    MT_EXPECT_EQ(sig_seen, 0); /* ignored */
    ucrt_signal(UCRT_SIGTERM, on_sig);
    MT_EXPECT_EQ(ucrt_raise(UCRT_SIGTERM), 0);
    MT_EXPECT_EQ(sig_seen, UCRT_SIGTERM);
    MT_EXPECT(g_ucrt_pr.signals[UCRT_SIGTERM] == UCRT_SIG_DFL); /* reset before the handler ran */
    MT_EXPECT(ucrt_signal(99, on_sig) == UCRT_SIG_ERR);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_raise(99), -1);
    /* abort: a handler for SIGABRT runs; with none the process ends with code 3 */
    sig_seen = 0;
    ucrt_signal(UCRT_SIGABRT, on_sig);
    g_ucrt_pr.abort_behavior = 0;
    if (setjmp(exit_jmp) == 0)
        ucrt_abort();
    MT_EXPECT_EQ(sig_seen, UCRT_SIGABRT);
    MT_EXPECT_EQ(exit_code, 3);
    exited = 0;
    ucrt_signal(UCRT_SIGABRT, UCRT_SIG_DFL);
    if (setjmp(exit_jmp) == 0)
        ucrt_abort();
    MT_EXPECT(exited && exit_code == 3);
    MT_EXPECT_EQ(ucrt_set_abort_behavior(UCRT_ABORT_WRITE_MSG, UCRT_ABORT_WRITE_MSG), 0);
    g_ucrt_pr.abort_behavior = 3;
}

static int inv_called;
static void inv_handler(const ucrt_wchar *a, const ucrt_wchar *b, const ucrt_wchar *c, unsigned d, uintptr_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    ++inv_called;
}

static void invalid_parameter_dispatch(void)
{
    setup();
    ucrt_runtime_init();
    inv_called = 0;
    ucrt_set_invalid_parameter_handler(inv_handler);
    MT_EXPECT_EQ(ucrt_invalid(UCRT_EINVAL), UCRT_EINVAL);
    MT_EXPECT_EQ(inv_called, 1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    MT_EXPECT(ucrt_strlen(NULL ? "" : "x") == 1);
    /* with no handler the default ends the process with the Windows status code */
    ucrt_set_invalid_parameter_handler(NULL);
    if (setjmp(exit_jmp) == 0)
        ucrt_invalid(UCRT_ERANGE);
    MT_EXPECT(exited && exit_code == 0xC000000Du);
    g_ucrt_state.invalid_handler = NULL;
}

static int term_log[8], term_n;
static void i1(void) { term_log[term_n++] = 1; }
static void i2(void) { term_log[term_n++] = 2; }
static int e1(void) { return 0; }
static int e2(void) { return 5; }
static int e3(void) { term_log[term_n++] = 9; return 0; }

static void initterm_tables(void)
{
    ucrt_initterm_fn t[3] = {i1, NULL, i2};
    ucrt_inittermE_fn u[4] = {e1, NULL, e2, e3};
    term_n = 0;
    ucrt_initterm(t, t + 3);
    MT_EXPECT(term_n == 2 && term_log[0] == 1 && term_log[1] == 2);
    term_n = 0;
    MT_EXPECT_EQ(ucrt_initterm_e(u, u + 4), 5); /* stops at the first failure */
    MT_EXPECT_EQ(term_n, 0);
}

static const mt_case_t cases[] = {
    MT_CASE(command_line_rules), MT_CASE(environment_basics), MT_CASE(environment_changes), MT_CASE(exit_runs_handlers_in_reverse),
    MT_CASE(exit_flushes_streams), MT_CASE(signals_and_abort), MT_CASE(invalid_parameter_dispatch), MT_CASE(initterm_tables),
};

int main(void)
{
    return MT_RUN("crt_runtime", cases);
}
