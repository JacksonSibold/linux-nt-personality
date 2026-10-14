/*
 * console_s6.c - S6 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * Imports only from kernel32.dll. It finds kernel32.dll by name with
 * GetModuleHandleA and resolves GetCurrentProcessId with GetProcAddress, which
 * needs kernel32.dll in the loader's module list (ntdll_main.c registers the
 * startup modules, winldr_register_mapped). Steps:
 *   1. GetModuleHandleA("kernel32.dll") must return a module handle.
 *   2. GetModuleHandleA(NULL) must return the exe's own base (non-NULL).
 *   3. GetProcAddress(kernel32, "GetCurrentProcessId") must return the address
 *      the direct import binds to, and calling it must give a non-zero PID.
 * It writes one line to stdout for the result. Each failed step exits with a
 * distinct code (10 to 13). Exit status 0 means every step passed.
 */

typedef void *HANDLE;
typedef void *HMODULE;
typedef void *FARPROC;
typedef unsigned long DWORD;
typedef int BOOL;

__declspec(dllimport) HMODULE GetModuleHandleA(const char *name);
__declspec(dllimport) FARPROC GetProcAddress(HMODULE h, const char *name);
__declspec(dllimport) DWORD GetCurrentProcessId(void);
__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void ExitProcess(unsigned int code);

#define STD_OUTPUT_HANDLE ((DWORD)-11)

static const char ok_line[] = "S6 GetProcAddress kernel32 GetCurrentProcessId: match\n";

static DWORD slen(const char *s)
{
    DWORD n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void)
{
    DWORD n = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC p;
    DWORD (*get_pid)(void);

    if (!k32)
        ExitProcess(10);
    if (!GetModuleHandleA((const char *)0))
        ExitProcess(11);

    p = GetProcAddress(k32, "GetCurrentProcessId");
    if (!p || p != (FARPROC)GetCurrentProcessId)
        ExitProcess(12);

    get_pid = (DWORD (*)(void))p;
    if (get_pid() == 0)
        ExitProcess(13);

    WriteFile(out, ok_line, slen(ok_line), &n, 0);
    ExitProcess(0);
    for (;;) {
    }
}
