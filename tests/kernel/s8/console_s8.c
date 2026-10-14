/*
 * console_s8.c - S8 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * Checks that the exe sees the command line and environment it was started
 * with, through the PEB (PEB->ProcessParameters, filled by pe_build_process_parameters
 * in src/kernel/fs/binfmt_pe.c), and that kernel32's GetCommandLineW and
 * GetEnvironmentStringsW return the same buffers.
 *
 * The step (tools/linux-env/step-initramfs.sh) starts this exe with
 *   STEP_ARGS="alpha 'beta gamma' 3 'say\"hi'"    (see the step's comment)
 *   STEP_ENV="NTPERS_S8=alpha-env"
 * so argv is exactly five strings. Those values are the golden-independent
 * assertions; nothing is read from a recorded output.
 *
 * Steps (each failure exits with its own code, exit status 0 means all passed):
 *   10  no ProcessParameters
 *   11  CommandLine is empty
 *   12  the command line does not split into five arguments
 *   13  argv[0] does not name console_s8.exe
 *   14  argv[1] is not "alpha"
 *   15  argv[2] is not "beta gamma" (quoted, contains a space)
 *   16  argv[3] is not "3"
 *   17  argv[4] is not say"hi (an embedded quote, escaped as \" by the kernel)
 *   18  the environment has no NTPERS_S8=alpha-env entry
 *   19  EnvironmentSize does not match the block up to its double NUL
 *   20  ImagePathName does not name console_s8.exe
 *   21  GetCommandLineW does not return the PEB's CommandLine buffer
 *   22  GetEnvironmentStringsW does not return the PEB's Environment block
 * It writes one line to stdout on success.
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned short WCHAR;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) WCHAR *GetCommandLineW(void);
__declspec(dllimport) WCHAR *GetEnvironmentStringsW(void);
__declspec(dllimport) BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void ExitProcess(unsigned int code);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define EXPECT_ARGC 5
#define MAX_TOKENS 8
#define MAX_TOKEN_UNITS 128

/* RTL_USER_PROCESS_PARAMETERS offsets (binfmt_pe.c PP_OFF_*; src/ntdll/nt_types.h). */
#define PP_IMAGE_PATH  0x60   /* UNICODE_STRING: Length at +0, Buffer at +8 */
#define PP_COMMAND     0x70
#define PP_ENVIRONMENT 0x80
#define PP_ENV_SIZE    0x3F0

static const char ok_line[] = "S8 command line: 5 args ok; environment NTPERS_S8 ok\n";

static unsigned short tok[MAX_TOKENS][MAX_TOKEN_UNITS];
static unsigned tok_len[MAX_TOKENS];

static unsigned char *peb_get(void)
{
    void *p;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(p));
    return (unsigned char *)p;
}

static unsigned short u16_at(const unsigned char *b, unsigned off)
{
    return (unsigned short)(b[off] | (b[off + 1] << 8));
}

static unsigned int u32_at(const unsigned char *b, unsigned off)
{
    return (unsigned int)b[off] | ((unsigned int)b[off + 1] << 8) |
           ((unsigned int)b[off + 2] << 16) | ((unsigned int)b[off + 3] << 24);
}

static const unsigned short *ptr_at(const unsigned char *b, unsigned off)
{
    const unsigned char *p = b + off;
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return (const unsigned short *)(void *)v;
}

/* True when the UTF-16 units s[0..n) equal the ASCII string a exactly. */
static int eq_ascii(const unsigned short *s, unsigned n, const char *a)
{
    unsigned i = 0;
    for (; a[i]; i++) {
        if (i >= n || s[i] != (unsigned char)a[i])
            return 0;
    }
    return i == n;
}

/* True when the units end with the ASCII suffix a. */
static int ends_with_ascii(const unsigned short *s, unsigned n, const char *a)
{
    unsigned m = 0;
    while (a[m]) m++;
    if (n < m)
        return 0;
    for (unsigned i = 0; i < m; i++) {
        if (s[n - m + i] != (unsigned char)a[i])
            return 0;
    }
    return 1;
}

/* Splits a command line by the Windows rules: spaces and tabs separate
 * arguments outside quotes, quotes are dropped, and backslash-quote is a
 * literal quote. Returns the argument count, or -1 if one does not fit. */
static int split_command_line(const unsigned short *s, unsigned n)
{
    unsigned i = 0;
    int count = 0;

    while (i < n) {
        int inq = 0;
        unsigned len = 0;

        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            i++;
        if (i >= n)
            break;
        if (count >= MAX_TOKENS)
            return -1;
        while (i < n) {
            unsigned short c = s[i];
            if (!inq && (c == ' ' || c == '\t'))
                break;
            if (c == '\\' && i + 1 < n && s[i + 1] == '"') {
                c = '"';
                i++;
            } else if (c == '"') {
                inq = !inq;
                i++;
                continue;
            }
            if (len >= MAX_TOKEN_UNITS)
                return -1;
            tok[count][len++] = c;
            i++;
        }
        tok_len[count] = len;
        count++;
    }
    return count;
}

/* Looks for the entry NAME=VALUE in the environment block and checks that the
 * block's size field covers exactly the entries and the final NUL. */
static int env_check(const unsigned short *env, unsigned env_bytes)
{
    unsigned units = env_bytes / 2;
    unsigned i = 0;
    int found = 0;

    if (!env || env_bytes == 0)
        return 18;
    while (i < units) {
        unsigned start = i;
        if (env[i] == 0) {
            /* double NUL: the end of the block, which must be the last unit. */
            if (i + 1 != units)
                return 19;
            break;
        }
        while (i < units && env[i] != 0)
            i++;
        if (i >= units)
            return 19;
        if (eq_ascii(env + start, i - start, "NTPERS_S8=alpha-env"))
            found = 1;
        i++; /* the NUL that ends this entry */
    }
    if (i >= units)
        return 19;
    return found ? 0 : 18;
}

void _start(void)
{
    unsigned char *peb = peb_get();
    unsigned char *pp = peb ? *(unsigned char **)(peb + 0x20) : 0;
    DWORD n = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    int rc, count;
    unsigned cmd_units, img_units;
    const unsigned short *cmd, *img, *env;
    unsigned env_bytes;

    if (!pp)
        ExitProcess(10);

    cmd = ptr_at(pp, PP_COMMAND + 8);
    cmd_units = u16_at(pp, PP_COMMAND) / 2;
    if (!cmd || cmd_units == 0)
        ExitProcess(11);

    count = split_command_line(cmd, cmd_units);
    if (count != EXPECT_ARGC)
        ExitProcess(12);
    if (!ends_with_ascii(tok[0], tok_len[0], "console_s8.exe"))
        ExitProcess(13);
    if (!eq_ascii(tok[1], tok_len[1], "alpha"))
        ExitProcess(14);
    if (!eq_ascii(tok[2], tok_len[2], "beta gamma"))
        ExitProcess(15);
    if (!eq_ascii(tok[3], tok_len[3], "3"))
        ExitProcess(16);
    if (!eq_ascii(tok[4], tok_len[4], "say\"hi"))
        ExitProcess(17);

    env = ptr_at(pp, PP_ENVIRONMENT);
    env_bytes = u32_at(pp, PP_ENV_SIZE);
    rc = env_check(env, env_bytes);
    if (rc)
        ExitProcess((unsigned)rc);

    img = ptr_at(pp, PP_IMAGE_PATH + 8);
    img_units = u16_at(pp, PP_IMAGE_PATH) / 2;
    if (!img || !ends_with_ascii(img, img_units, "console_s8.exe"))
        ExitProcess(20);

    if (GetCommandLineW() != cmd)
        ExitProcess(21);
    if (GetEnvironmentStringsW() != env)
        ExitProcess(22);

    WriteFile(out, ok_line, sizeof(ok_line) - 1, &n, 0);
    ExitProcess(0);
    for (;;) {
    }
}
