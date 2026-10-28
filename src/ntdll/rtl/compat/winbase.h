/* winbase.h - stand-in for the parts of Wine winbase.h that the ported ntdll sources and their unit tests use. */
#ifndef RTL_COMPAT_WINBASE_H
#define RTL_COMPAT_WINBASE_H
#include "windef.h"
#include "winnt.h"
#include "winerror.h"
#include "minwinbase.h"

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#endif

/* from Wine include/timezoneapi.h */
typedef struct _TIME_ZONE_INFORMATION {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
} TIME_ZONE_INFORMATION, *PTIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;

/* Provided by tests/rtl-host/host_support.c (the unit tests find ntdll functions by name). */
typedef INT_PTR (WINAPI *FARPROC)(void);
HMODULE WINAPI GetModuleHandleA(LPCSTR);
HMODULE WINAPI LoadLibraryA(LPCSTR);
FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR);
DWORD WINAPI GetLastError(void);
DWORD WINAPI GetTickCount(void);
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *);
void WINAPI SetLastError(DWORD);
UINT WINAPI GetACP(void);
UINT WINAPI GetOEMCP(void);
#ifndef GetProcessHeap
HANDLE WINAPI GetProcessHeap(void);
#endif
LPVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID);
int WINAPI MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, BOOL *);
int WINAPI lstrcmpW(LPCWSTR, LPCWSTR);
int WINAPI lstrlenW(LPCWSTR);
int WINAPI lstrlenA(LPCSTR);
int WINAPI lstrcmpiW(LPCWSTR, LPCWSTR);
LPWSTR WINAPI lstrcpyW(LPWSTR, LPCWSTR);
#ifndef HEAP_ZERO_MEMORY
#define HEAP_ZERO_MEMORY 0x00000008
#endif
int WINAPI lstrcmpA(LPCSTR, LPCSTR);
int WINAPI lstrcmpiA(LPCSTR, LPCSTR);
#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001)
#endif
#endif
