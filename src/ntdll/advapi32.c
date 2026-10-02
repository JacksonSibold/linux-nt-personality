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
#ifndef HKEY
typedef HANDLE HKEY;
#endif
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, PVOID v){ (void)h;(void)r;(void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v){ return DllMain(h,r,v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v){ return DllMain(h,r,v); }
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
LONG WINAPI RegOpenKeyExA(HKEY k, const char *sub, DWORD opt, DWORD access, HKEY *out){
    (void)k;(void)sub;(void)opt;(void)access;(void)out;
    return 2; // not found
}
LONG WINAPI RegQueryValueExA(HKEY k, const char *val, DWORD *res, DWORD *type, BYTE *data, DWORD *cb){
    (void)k;(void)val;(void)res;(void)type;(void)data;(void)cb;
    return 2;
}
LONG WINAPI RegCloseKey(HKEY k){ (void)k; return 0; }
LONG WINAPI RegOpenKeyExW(HKEY k, const WCHAR *sub, DWORD opt, DWORD access, HKEY *out){
    (void)k;(void)sub;(void)opt;(void)access;(void)out; return 2;
}
LONG WINAPI RegQueryValueExW(HKEY k, const WCHAR *val, DWORD *res, DWORD *type, BYTE *data, DWORD *cb){
    (void)k;(void)val;(void)res;(void)type;(void)data;(void)cb; return 2;
}
#endif
