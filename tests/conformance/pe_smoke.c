/* pe_smoke.c - minimal PE64 exercising the Win32 -> NT path.
 * Built with mingw-w64. Used to validate the native PE loader and
 * NT syscall dispatch. Exit code 42 is the success sentinel.
 */
#include <windows.h>

int main(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    const char msg[] = "PE-OK\n";

    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return 1;

    if (!WriteFile(h, msg, (DWORD)(sizeof(msg) - 1), &written, NULL))
        return 2;

    if (written != sizeof(msg) - 1)
        return 3;

    return 42;
}
