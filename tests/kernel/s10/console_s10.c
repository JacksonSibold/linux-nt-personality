/*
 * console_s10.c - S10 of the console milestone: the standard handles.
 *
 * The step (tools/linux-env/step-initramfs.sh, STEP_STDIN) runs this exe with its
 * standard input redirected from a fixed file, tests/kernel/s10/console_s10.stdin,
 * whose bytes are "alpha\nbeta gamma\n3\n" (19 bytes, three lines). Standard
 * output and standard error are the console. Every expected value below is
 * written in this source; nothing is read from a recorded output.
 *
 * For each input line the exe writes "S10 out: <line>" to standard output and
 * "S10 err: <line>" to standard error, so the round trip is visible on the serial
 * log. Each failed check exits with its own code (10 to 25). Exit status 0 means
 * all checks passed; the success line is written to standard output.
 *
 *  10  GetStdHandle(STD_INPUT_HANDLE) is NULL
 *  11  GetFileType(stdin) is not FILE_TYPE_DISK (1): the input is a regular file
 *  12  GetFileType(stdout) is not FILE_TYPE_CHAR (2): the output is the console
 *  13  ReadFile fails before the end of the input
 *  14  the input is not 19 bytes
 *  15  the input does not have three lines
 *  16  line 1 is not "alpha"
 *  17  line 2 is not "beta gamma"
 *  18  line 3 is not "3"
 *  19  ReadFile at end of input is not TRUE with zero bytes
 *  20  WriteFile on the standard input handle succeeded (read-only handle)
 *  21  WriteFile to standard output failed
 *  22  WriteFile to standard error failed
 *  23  SetStdHandle(STD_ERROR) then GetStdHandle(STD_ERROR) does not round-trip
 *  24  GetStdHandle(99) is not INVALID_HANDLE_VALUE with ERROR_INVALID_HANDLE (6)
 *  25  the standard error handle was not restored
 * It writes one line to standard output on success.
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL SetStdHandle(DWORD which, HANDLE h);
__declspec(dllimport) BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, void *ov);
__declspec(dllimport) BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) DWORD GetFileType(HANDLE h);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD e);
__declspec(dllimport) void ExitProcess(unsigned int code);

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define FILE_TYPE_DISK 1u
#define FILE_TYPE_CHAR 2u
#define ERROR_INVALID_HANDLE 6u
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

#define INPUT_BYTES 19u
#define MAX_LINES 8u

static const char in_want[INPUT_BYTES] = "alpha\nbeta gamma\n3\n";
static const char out_pfx[] = "S10 out: ";
static const char err_pfx[] = "S10 err: ";
static const char ok_line[] = "S10 stdin/stdout/stderr round trip ok\n";

static unsigned alen(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int same(const char *a, unsigned na, const char *b)
{
    if (alen(b) != na) return 0;
    for (unsigned i = 0; i < na; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void put(HANDLE h, const char *s, unsigned n, int *ok)
{
    DWORD written = 0;
    if (!WriteFile(h, s, n, &written, 0) || written != n) *ok = 0;
}

void _start(void)
{
    static char in[64];
    static unsigned line_start[MAX_LINES];
    static unsigned line_len[MAX_LINES];
    static char line_buf[128];
    unsigned n = 0, lines = 0;
    int wok = 1;
    HANDLE in_h, out_h, err_h;
    DWORD r = 0;

    /* 10: stdin is set. */
    in_h = GetStdHandle(STD_INPUT_HANDLE);
    if (!in_h) ExitProcess(10);
    out_h = GetStdHandle(STD_OUTPUT_HANDLE);
    err_h = GetStdHandle(STD_ERROR_HANDLE);

    /* 11, 12: file types. */
    if (GetFileType(in_h) != FILE_TYPE_DISK) ExitProcess(11);
    if (GetFileType(out_h) != FILE_TYPE_CHAR) ExitProcess(12);

    /* 13, 14: read to end of input in chunks of 7 bytes (short reads are fine). */
    for (;;) {
        DWORD want = sizeof(in) - n < 7 ? sizeof(in) - n : 7;
        if (want == 0) break;
        r = 0;
        if (!ReadFile(in_h, in + n, want, &r, 0)) ExitProcess(13);
        if (r == 0) break;
        n += r;
    }
    if (n != INPUT_BYTES) ExitProcess(14);
    if (!same(in, n, in_want)) ExitProcess(15);

    /* 15: split into lines on '\n'; each line excludes its newline. */
    {
        unsigned s = 0;
        for (unsigned i = 0; i < n; i++) {
            if (in[i] != '\n') continue;
            if (lines >= MAX_LINES) ExitProcess(15);
            line_start[lines] = s;
            line_len[lines] = i - s;
            lines++;
            s = i + 1;
        }
    }
    if (lines != 3) ExitProcess(15);

    /* 16 to 18: each line is what the source says. */
    {
        static const char *want[3] = { "alpha", "beta gamma", "3" };
        static const unsigned code[3] = { 16, 17, 18 };
        for (unsigned k = 0; k < 3; k++) {
            if (!same(in + line_start[k], line_len[k], want[k])) ExitProcess(code[k]);
        }
    }

    /* 19: at end of input a read is TRUE with zero bytes. */
    r = 99;
    SetLastError(0);
    if (!ReadFile(in_h, line_buf, 8, &r, 0) || r != 0) ExitProcess(19);

    /* 20: stdin is read-only, so a write to it fails. */
    r = 0;
    if (WriteFile(in_h, "x", 1, &r, 0)) ExitProcess(20);

    /* Echo each line to stdout and stderr. */
    for (unsigned k = 0; k < lines; k++) {
        unsigned pn = alen(out_pfx), en = alen(err_pfx);
        put(out_h, out_pfx, pn, &wok);
        put(out_h, in + line_start[k], line_len[k], &wok);
        put(out_h, "\n", 1, &wok);
        if (!wok) ExitProcess(21);
        put(err_h, err_pfx, en, &wok);
        put(err_h, in + line_start[k], line_len[k], &wok);
        put(err_h, "\n", 1, &wok);
        if (!wok) ExitProcess(22);
    }

    /* 23, 25: SetStdHandle(STD_ERROR) round-trips, then the original is restored. */
    if (!SetStdHandle(STD_ERROR_HANDLE, out_h)) ExitProcess(23);
    if (GetStdHandle(STD_ERROR_HANDLE) != out_h) ExitProcess(23);
    if (!SetStdHandle(STD_ERROR_HANDLE, err_h)) ExitProcess(25);
    if (GetStdHandle(STD_ERROR_HANDLE) != err_h) ExitProcess(25);

    /* 24: an unknown standard handle index is refused. */
    SetLastError(0);
    if (GetStdHandle(99) != INVALID_HANDLE_VALUE || GetLastError() != ERROR_INVALID_HANDLE) ExitProcess(24);

    put(out_h, ok_line, sizeof(ok_line) - 1, &wok);
    if (!wok) ExitProcess(21);
    ExitProcess(0);
    for (;;) {
    }
}
