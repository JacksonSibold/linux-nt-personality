/*
 * console_s4.c - S4 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * Imports printf and puts from msvcrt.dll (src/ntdll/msvcrt.c built as
 * msvcrt.dll; msvcrt.dll imports kernel32.dll, which imports ntdll.dll). The
 * shim's write_stdout goes through kernel32 GetStdHandle and WriteFile to the
 * StandardOutput handle that binfmt_pe installs (S1).
 *
 * Returning from _start hands control back to LdrInitializeThunk, which calls
 * NtTerminateProcess(-1, 0), so the exit status is 0 when nothing fails.
 */

__declspec(dllimport) int printf(const char *fmt, ...);
__declspec(dllimport) int puts(const char *s);

void _start(void)
{
    printf("hello from msvcrt %d %s\n", 4, "printf");
    puts("hello from msvcrt puts");
}
