/*
 * kernel32_misc.c - vectored exception handlers, named pipes and startup
 * information (K32-X1).
 *
 * The vectored handler calls forward to the same-named ntdll functions
 * (src/ntdll/except.c), which keep the handler list. The named pipe calls are
 * not implemented: the kernel has no named pipe file system (NPFS) service
 * yet, so each fails with ERROR_CALL_NOT_IMPLEMENTED and the documented
 * failure value. GetStartupInfoA fills the cb field and zeroes the rest; the
 * process does not yet receive the creator's STARTUPINFO.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef PVOID LPVOID;

#define K32X_INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define K32X_ERROR_CALL_NOT_IMPLEMENTED 120u

/* ntdll's vectored handler list (src/ntdll/except.c). The imports carry the
 * ntdll names through asm labels, because the kernel32 exports below use the
 * same names. */
__declspec(dllimport) extern PVOID ntdll_AddVectoredExceptionHandler(ULONG First, PVOID Handler) __asm__("AddVectoredExceptionHandler");
__declspec(dllimport) extern ULONG ntdll_RemoveVectoredExceptionHandler(PVOID Handle) __asm__("RemoveVectoredExceptionHandler");
__declspec(dllimport) extern PVOID ntdll_AddVectoredContinueHandler(ULONG First, PVOID Handler) __asm__("AddVectoredContinueHandler");
__declspec(dllimport) extern ULONG ntdll_RemoveVectoredContinueHandler(PVOID Handle) __asm__("RemoveVectoredContinueHandler");

/* K32-X1: the handler lists are ntdll's. The results are passed through: a
 * handle (or non-NULL) on success, NULL or zero when the list refuses it. */
PVOID WINAPI K32AddVectoredExceptionHandler(ULONG first, PVOID handler)
{
    return ntdll_AddVectoredExceptionHandler(first, handler);
}

ULONG WINAPI K32RemoveVectoredExceptionHandler(PVOID handle)
{
    return ntdll_RemoveVectoredExceptionHandler(handle);
}

PVOID WINAPI K32AddVectoredContinueHandler(ULONG first, PVOID handler)
{
    return ntdll_AddVectoredContinueHandler(first, handler);
}

ULONG WINAPI K32RemoveVectoredContinueHandler(PVOID handle)
{
    return ntdll_RemoveVectoredContinueHandler(handle);
}

/* Defined in kernel32.c. */
void SetLastError(DWORD e);

/* K32-X2: named pipes are not implemented (see the file comment). */
HANDLE WINAPI CreateNamedPipeA(const char *name, DWORD open, DWORD mode, DWORD max, DWORD out, DWORD in, DWORD timeout, void *sa)
{
    (void)name;(void)open;(void)mode;(void)max;(void)out;(void)in;(void)timeout;(void)sa;
    SetLastError(K32X_ERROR_CALL_NOT_IMPLEMENTED);
    return K32X_INVALID_HANDLE_VALUE;
}

BOOL WINAPI ConnectNamedPipe(HANDLE h, void *overlapped)
{
    (void)h;(void)overlapped;
    SetLastError(K32X_ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI DisconnectNamedPipe(HANDLE h)
{
    (void)h;
    SetLastError(K32X_ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI PeekNamedPipe(HANDLE h, void *buf, DWORD size, DWORD *read, DWORD *avail, DWORD *left)
{
    (void)h;(void)buf;(void)size;(void)read;(void)avail;(void)left;
    SetLastError(K32X_ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI WaitNamedPipeA(const char *name, DWORD timeout)
{
    (void)name;(void)timeout;
    SetLastError(K32X_ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* STARTUPINFOA: cb is the size of the structure (104 on x64); the rest is zero. */
typedef struct {
    DWORD cb;
    char *lpReserved;
    char *lpDesktop;
    char *lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} K32X_STARTUPINFOA;

void WINAPI GetStartupInfoA(K32X_STARTUPINFOA *si)
{
    if (!si)
        return;
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
    SetLastError(0);
}

#endif /* __MINGW32__ */
