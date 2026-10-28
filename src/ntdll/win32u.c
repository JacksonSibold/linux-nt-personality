/* win32u.dll - hosts the win32k model (the win32k sources) for the process.
 *
 * user32.dll and gdi32.dll import the model from here, so there is one window tree,
 * one handle table and one set of DCs per process (decision 0076, addendum). The
 * exports are the w32k_* prototypes of win32k/w32k.h, listed in out/win32u.def by
 * build/gen_win32u_def.py. They are a private interface, not the NtUser/NtGdi system
 * call surface of Windows' win32u.dll.
 */
#ifdef __MINGW32__
#include "win32k/w32k.h"
#include <stdint.h>

typedef int32_t BOOL_T;

BOOL_T DllMain(void *h, uint32_t reason, void *reserved)
{
    (void)h;
    (void)reserved;
    if (reason == 1)
        w32k_init_once();
    return 1;
}

BOOL_T __stdcall _DllMainCRTStartup(void *h, uint32_t reason, void *reserved)
{
    return DllMain(h, reason, reserved);
}

BOOL_T __stdcall DllMainCRTStartup(void *h, uint32_t reason, void *reserved)
{
    return DllMain(h, reason, reserved);
}
#endif
