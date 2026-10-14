/*
 * console_s9.c - S9 of the console milestone: the kernel32 environment API.
 *
 * The step (tools/linux-env/step-initramfs.sh) starts this exe with
 *   STEP_ENV="NTPERS_S9=nine-env"
 *   STEP_ARGS="alpha 'beta gamma'"
 * so the environment holds NTPERS_S9=nine-env, and the command line is
 * "<argv0> alpha \"beta gamma\"" under the Windows quoting rules (a space
 * inside an argument makes it quoted; argv0 has no space).
 *
 * Every expected value below is written in this source from the Windows rules.
 * Nothing is read from a recorded output. Each failed check exits with its own
 * code (10 to 30). Exit status 0 means all checks passed; the success line is
 * written to stdout.
 *
 *  10  GetEnvironmentVariableW(NTPERS_S9) is not "nine-env"
 *  11  GetEnvironmentVariableA(NTPERS_S9) is not "nine-env"
 *  12  the lookup is not case-insensitive (ntpers_s9)
 *  13  a missing name does not return 0 with ERROR_ENVVAR_NOT_FOUND (203)
 *  14  the size query (NULL buffer, size 0) is not 9 (8 units and the NUL)
 *  15  a too-small buffer does not return 9, or was written
 *  16  the ANSI size query is not 9
 *  17  SetEnvironmentVariableW(NTPERS_S9_NEW, "new value") fails
 *  18  GetEnvironmentVariableA does not return "new value" for the new name
 *  19  GetEnvironmentStringsA has no "NTPERS_S9_NEW=new value" entry
 *  20  FreeEnvironmentStringsA does not accept the block it returned
 *  21  ExpandEnvironmentStringsW gives the wrong text or count
 *  22  ExpandEnvironmentStringsA gives the wrong text or count
 *  23  GetCommandLineA is NULL, or does not match GetCommandLineW
 *  24  GetCommandLineA does not end with ' alpha "beta gamma"'
 *  25  a name containing '=' is accepted (or not ERROR_INVALID_PARAMETER)
 *  26  SetEnvironmentVariableW(name, NULL) on the new name fails
 *  27  the deleted name is still found, or its error is not 203
 *  28  deleting an unset name does not fail with ERROR_ENVVAR_NOT_FOUND
 *  29  FreeEnvironmentStringsW(NULL) is accepted, or the live block is refused
 *  30  NTPERS_S9 changed or disappeared after the other calls
 * It writes one line to stdout on success.
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned short WCHAR;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void ExitProcess(unsigned int code);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) WCHAR *GetCommandLineW(void);
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD size);
__declspec(dllimport) DWORD GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
__declspec(dllimport) BOOL SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value);
__declspec(dllimport) DWORD ExpandEnvironmentStringsA(const char *src, char *dst, DWORD size);
__declspec(dllimport) DWORD ExpandEnvironmentStringsW(const WCHAR *src, WCHAR *dst, DWORD size);
__declspec(dllimport) char *GetEnvironmentStringsA(void);
__declspec(dllimport) WCHAR *GetEnvironmentStringsW(void);
__declspec(dllimport) BOOL FreeEnvironmentStringsA(char *p);
__declspec(dllimport) BOOL FreeEnvironmentStringsW(WCHAR *p);
__declspec(dllimport) void SetLastError(DWORD e);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ENVVAR_NOT_FOUND 203u

static void SetLastErrorZero(void)
{
    SetLastError(0);
}

static const char ok_line[] = "S9 environment: get, set, delete, expand, command line ok\n";

static unsigned alen(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static unsigned wlen(const WCHAR *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void wz(WCHAR *d, const char *s)
{
    unsigned i = 0;
    for (; s[i]; i++) d[i] = (WCHAR)(unsigned char)s[i];
    d[i] = 0;
}

/* True when the n units of w equal the ASCII string a exactly. */
static int weq(const WCHAR *w, unsigned n, const char *a)
{
    if (alen(a) != n) return 0;
    for (unsigned i = 0; i < n; i++) {
        if (w[i] != (unsigned char)a[i]) return 0;
    }
    return 1;
}

static int aeq(const char *s, unsigned n, const char *a)
{
    if (alen(a) != n) return 0;
    for (unsigned i = 0; i < n; i++) {
        if (s[i] != a[i]) return 0;
    }
    return 1;
}

static int ends_with(const char *s, const char *suffix)
{
    unsigned n = alen(s), m = alen(suffix);
    if (n < m) return 0;
    for (unsigned i = 0; i < m; i++) {
        if (s[n - m + i] != suffix[i]) return 0;
    }
    return 1;
}

/* True when the ANSI block (double-NUL terminated) has an entry equal to e. */
static int ansi_block_has(const char *blk, const char *e)
{
    const char *p = blk;
    while (*p) {
        unsigned n = alen(p);
        if (aeq(p, n, e)) return 1;
        p += n + 1;
    }
    return 0;
}

static void write_line(const char *s, unsigned n)
{
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, s, n, &written, 0);
}

void _start(void)
{
    static const char name[] = "NTPERS_S9";
    static const char want_env[] = "nine-env";
    static const char new_name[] = "NTPERS_S9_NEW";
    static const char new_value[] = "new value";
    static const char new_entry[] = "NTPERS_S9_NEW=new value";
    static const char missing_name[] = "NTPERS_S9_MISSING";
    static const char tmpl[] = "[%NTPERS_S9%]-%NTPERS_S9_NOPE%";
    static const char tmpl_want[] = "[nine-env]-%NTPERS_S9_NOPE%";
    static const char cmd_suffix[] = " alpha \"beta gamma\"";

    WCHAR w_name[64], w_new[64], w_missing[64], w_lower[64], w_bad[64];
    WCHAR wbuf[64];
    char abuf[128];
    DWORD n;

    wz(w_name, name);
    wz(w_new, new_name);
    wz(w_missing, missing_name);
    wz(w_lower, "ntpers_s9");
    wz(w_bad, "NTPERS_S9=bad");

    /* 10, 11, 12: lookup by W, by A, and case-insensitively. */
    n = GetEnvironmentVariableW(w_name, wbuf, 64);
    if (n != 8 || !weq(wbuf, n, want_env)) return 10;
    n = GetEnvironmentVariableA(name, abuf, sizeof(abuf));
    if (n != 8 || !aeq(abuf, n, want_env)) return 11;
    n = GetEnvironmentVariableW(w_lower, wbuf, 64);
    if (n != 8 || !weq(wbuf, n, want_env)) return 12;

    /* 13: a missing name. */
    SetLastErrorZero();
    n = GetEnvironmentVariableW(w_missing, wbuf, 64);
    if (n != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) return 13;

    /* 14, 15: the size rules. The buffer is written only when it fits. */
    n = GetEnvironmentVariableW(w_name, 0, 0);
    if (n != 9) return 14;
    wbuf[0] = 0xAAAA;
    n = GetEnvironmentVariableW(w_name, wbuf, 4);
    if (n != 9 || wbuf[0] != 0xAAAA) return 15;

    /* 16: the ANSI size query. */
    n = GetEnvironmentVariableA(name, 0, 0);
    if (n != 9) return 16;

    /* 17, 18: set a new variable and read it back as ANSI. */
    wz(wbuf, new_value);
    if (!SetEnvironmentVariableW(w_new, wbuf)) return 17;
    n = GetEnvironmentVariableA(new_name, abuf, sizeof(abuf));
    if (n != 9 || !aeq(abuf, n, new_value)) return 18;

    /* 19, 20: the ANSI environment block holds the entry, and it frees. */
    {
        char *blk = GetEnvironmentStringsA();
        if (!blk || !ansi_block_has(blk, new_entry)) return 19;
        if (!FreeEnvironmentStringsA(blk)) return 20;
    }

    /* 21, 22: expansion. The expected text and count are 27 units and the NUL. */
    {
        WCHAR wsrc[64], wdst[64];
        char adst[64];
        unsigned want_units = alen(tmpl_want);
        wz(wsrc, tmpl);
        n = ExpandEnvironmentStringsW(wsrc, wdst, 64);
        if (n != want_units + 1 || !weq(wdst, want_units, tmpl_want)) return 21;
        n = ExpandEnvironmentStringsA(tmpl, adst, sizeof(adst));
        if (n != want_units + 1 || !aeq(adst, want_units, tmpl_want)) return 22;
    }

    /* 23, 24: the command line, in both forms. */
    {
        char *cmd_a = GetCommandLineA();
        WCHAR *cmd_w = GetCommandLineW();
        unsigned cw;
        if (!cmd_a || !cmd_w) return 23;
        cw = wlen(cmd_w);
        if (alen(cmd_a) != cw) return 23;
        for (unsigned i = 0; i < cw; i++) {
            if (cmd_w[i] != (unsigned char)cmd_a[i]) return 23;
        }
        if (!ends_with(cmd_a, cmd_suffix)) return 24;
    }

    /* 25: a name with '=' is refused. */
    SetLastErrorZero();
    if (SetEnvironmentVariableW(w_bad, w_new) != 0 || GetLastError() != ERROR_INVALID_PARAMETER) return 25;

    /* 26, 27: delete the new name, then it is gone. */
    if (!SetEnvironmentVariableW(w_new, 0)) return 26;
    SetLastErrorZero();
    n = GetEnvironmentVariableW(w_new, 0, 0);
    if (n != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) return 27;

    /* 28: deleting a name that is not set fails with ERROR_ENVVAR_NOT_FOUND. */
    SetLastErrorZero();
    if (SetEnvironmentVariableW(w_missing, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) return 28;

    /* 29: the live block is accepted, NULL is refused. */
    {
        WCHAR *live = GetEnvironmentStringsW();
        SetLastErrorZero();
        if (!live || !FreeEnvironmentStringsW(live)) return 29;
        if (FreeEnvironmentStringsW(0) != 0 || GetLastError() != ERROR_INVALID_PARAMETER) return 29;
    }

    /* 30: the variable from the step is unchanged. */
    n = GetEnvironmentVariableW(w_name, wbuf, 64);
    if (n != 8 || !weq(wbuf, n, want_env)) return 30;

    write_line(ok_line, sizeof(ok_line) - 1);
    ExitProcess(0);
    for (;;) {
    }
}
