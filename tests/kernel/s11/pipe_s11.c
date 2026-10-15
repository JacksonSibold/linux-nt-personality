/*
 * pipe_s11.c - S11 runtime test of a pipe standard input.
 *
 * The step runs this exe as "echo pipe-payload | pipe_s11.exe", so standard input is
 * a pipe (a FIFO in the guest). Standard output is the console. Each failed check
 * exits with its own code; 0 means all checks passed.
 *
 *  10  GetStdHandle(STD_INPUT_HANDLE) is NULL or INVALID_HANDLE_VALUE
 *  11  GetFileType(stdin) is not FILE_TYPE_PIPE (3)
 *  12  ReadFile on stdin failed with an error other than ERROR_BROKEN_PIPE (109)
 *  13  the input is not "pipe-payload\n" (13 bytes)
 *  14  the input did not end: a read after the data returned more bytes
 *
 * End of a pipe is reported as TRUE with zero bytes, or as FALSE with
 * ERROR_BROKEN_PIPE, and both are accepted here.
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, void *ov);
__declspec(dllimport) DWORD GetFileType(HANDLE h);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void ExitProcess(unsigned int code);

#define STD_INPUT_HANDLE ((DWORD)-10)
#define FILE_TYPE_PIPE 3u
#define ERROR_BROKEN_PIPE 109u
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

#define WANT_BYTES 13u
#define BUF_BYTES 64u

static const char want[WANT_BYTES] = "pipe-payload\n";

void _start(void)
{
    char buf[BUF_BYTES];
    unsigned got = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD n = 0;
    unsigned i;

    if (in == ((HANDLE)0) || in == INVALID_HANDLE_VALUE) ExitProcess(10);
    if (GetFileType(in) != FILE_TYPE_PIPE) ExitProcess(11);

    /* Read to the end of the pipe. A short read is normal on a pipe. */
    for (;;) {
        n = 0;
        if (!ReadFile(in, buf + got, BUF_BYTES - got, &n, 0)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            ExitProcess(12);
        }
        if (n == 0) break;
        got += n;
        if (got >= BUF_BYTES) ExitProcess(14);
    }

    if (got != WANT_BYTES) ExitProcess(13);
    for (i = 0; i < WANT_BYTES; i++) {
        if (buf[i] != want[i]) ExitProcess(13);
    }

    /* The pipe is at its end: a further read returns nothing. */
    n = 0;
    if (ReadFile(in, buf, BUF_BYTES, &n, 0) && n != 0) ExitProcess(14);

    ExitProcess(0);
    for (;;) {
    }
}
