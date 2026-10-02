#ifdef __MINGW32__
#include "nt_types.h"
#include <stdint.h>
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI WINAPI
#endif
#ifndef HINSTANCE
typedef PVOID HINSTANCE;
#endif
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, PVOID v){ (void)h;(void)r;(void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v){ return DllMain(h,r,v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v){ return DllMain(h,r,v); }
/* kernelbase is forwarder to kernel32 - for minimal we just export same stubs as kernel32 via forwarding */
/* Actual forwarding is done via def file with forward strings, but for build we need dummy exports that forward */
/* Provide a few common forwarders as real functions that just return error */
void* __stdcall dummy(void){ return 0; }
#endif
