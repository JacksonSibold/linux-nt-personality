/* pe_fwd.c - tiny DLL used to test export-table parsing, including a
 * forwarded export.  It is never executed, only parsed.
 */
#include <windows.h>

__declspec(dllexport) int RealExport(int x)
{
    return x + 1;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)fdwReason; (void)lpvReserved;
    return TRUE;
}
