/*
 * advapi32.c - advapi32.dll: the entry point and the few functions that need no module of their own. The registry functions
 * are in advapi32_reg.c.
 *
 * ImpersonateNamedPipeClient has no NT service behind it yet and fails with ERROR_CALL_NOT_IMPLEMENTED.
 */
#ifdef __MINGW32__
#include "nt_types.h"
#include <stddef.h>
#include <stdint.h>
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef HINSTANCE
typedef PVOID HINSTANCE;
#endif

#define ADV_ERROR_CALL_NOT_IMPLEMENTED 120UL

/* DllMain: no per-process state. */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, PVOID v) { (void)h; (void)r; (void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v) { return DllMain(h, r, v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v) { return DllMain(h, r, v); }

/* TEB LastErrorValue, as kernel32.c writes it. */
static void adv_set_last_error(DWORD e)
{
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    if (teb) teb->LastErrorValue = e;
}

/* No NT service impersonates a named-pipe client yet. Win32 reports failure
 * with ERROR_CALL_NOT_IMPLEMENTED, so the caller gets an explicit error. */
BOOL WINAPI ImpersonateNamedPipeClient(HANDLE hNamedPipe)
{
    (void)hNamedPipe;
    adv_set_last_error((DWORD)ADV_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}


/* No thread can hold an impersonation token here: ImpersonateNamedPipeClient
 * always fails, so there is nothing to revert. Win32 returns TRUE in that case. */
BOOL WINAPI RevertToSelf(void)
{
    return 1;
}

#endif /* __MINGW32__ */
