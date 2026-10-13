/*
 * console_s3.c - S3 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * Imports only from kernel32.dll (src/ntdll/kernel32.c built as kernel32.dll;
 * kernel32.dll imports ntdll.dll). Steps:
 *   1. GetStdHandle(STD_OUTPUT_HANDLE) and WriteFile a banner.
 *   2. CreateFileA(C:\tmp\s3.txt, CREATE_ALWAYS) and WriteFile a payload.
 *   3. CloseHandle, then reopen with OPEN_EXISTING and ReadFile it back.
 *   4. Check the bytes read match the payload and WriteFile them to stdout.
 * Exit status 0 means every step succeeded. C:\tmp is the guest's tmpfs
 * (step-initramfs.sh mounts it at /tmp).
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, void *ov);
__declspec(dllimport) HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa,
                                         DWORD disp, DWORD flags, HANDLE templ);
__declspec(dllimport) BOOL CloseHandle(HANDLE h);
__declspec(dllimport) void ExitProcess(unsigned int code);

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define STD_OUTPUT_HANDLE ((DWORD)-11)

static const char banner[] = "hello from kernel32\n";
static const char payload[] = "file payload from S3\n";

static DWORD slen(const char *s)
{
    DWORD n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void)
{
    DWORD n = 0, nr = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char buf[64];
    DWORD plen = slen(payload);
    DWORD i;

    if (!WriteFile(out, banner, slen(banner), &n, 0))
        ExitProcess(10);

    HANDLE f = CreateFileA("C:\\tmp\\s3.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == (HANDLE)(long long)-1)
        ExitProcess(11);
    if (!WriteFile(f, payload, plen, &n, 0) || n != plen)
        ExitProcess(12);
    CloseHandle(f);

    f = CreateFileA("C:\\tmp\\s3.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == (HANDLE)(long long)-1)
        ExitProcess(13);
    if (!ReadFile(f, buf, sizeof(buf), &nr, 0))
        ExitProcess(14);
    CloseHandle(f);

    if (nr != plen)
        ExitProcess(15);
    for (i = 0; i < plen; i++)
        if (buf[i] != payload[i])
            ExitProcess(16);

    if (!WriteFile(out, buf, nr, &n, 0))
        ExitProcess(17);
    ExitProcess(0);
    for (;;) {
    }
}
