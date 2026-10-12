/*
 * kernel32.c - minimal Win32 surface forwarding to ntdll
 *
 * Implements starter set required by task. Errors are translated via
 * RtlNtStatusToDosError and stored in TEB LastErrorValue for GetLastError fidelity.
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
#include "ldr.h"
#include "win32_policy.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef UINT
#define UINT unsigned int
#endif
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);

/* Nt syscall prototypes - generated via nt_syscalls.h but we redeclare needed ones */
__declspec(dllimport) extern NTSTATUS NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtReadFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
__declspec(dllimport) extern NTSTATUS NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
__declspec(dllimport) extern NTSTATUS NtClose(HANDLE);
__declspec(dllimport) extern NTSTATUS NtAllocateVirtualMemory(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG);
__declspec(dllimport) extern NTSTATUS NtProtectVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtQueryVirtualMemory(HANDLE, PVOID, ULONG, PVOID, SIZE_T, PSIZE_T);
__declspec(dllimport) extern NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
__declspec(dllimport) extern NTSTATUS NtDelayExecution(BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS LdrLoadDll(winldr_ctx_t *ctx, const WCHAR *path, ULONG flags, const WCHAR *name, HMODULE *out);
__declspec(dllimport) extern NTSTATUS LdrGetProcedureAddress(winldr_ctx_t *ctx, HMODULE mod, const ANSI_STRING *name, WORD ordinal, FARPROC *out);
__declspec(dllimport) extern NTSTATUS LdrGetDllHandle(winldr_ctx_t *ctx, const WCHAR *name, HMODULE *out);

/* TEB access for LastError */
static inline PTEB NtCurrentTebMingw(void) {
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    return teb;
}
void SetLastError(DWORD e) {
    PTEB teb = NtCurrentTebMingw();
    if (teb) teb->LastErrorValue = e;
}
DWORD GetLastError(void) {
    PTEB teb = NtCurrentTebMingw();
    return teb ? teb->LastErrorValue : 0;
}
static inline void SetLastErrorFromStatus(NTSTATUS s) {
    if (s == STATUS_SUCCESS) SetLastError(0);
    else SetLastError(RtlNtStatusToDosError(s));
}

/* DllMain */
BOOL NTAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) { (void)h;(void)r;(void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v){ return DllMain(h,r,v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v){ return DllMain(h,r,v); }

/* GetStdHandle */
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

HANDLE WINAPI GetStdHandle(DWORD nStdHandle) {
    switch (nStdHandle) {
        case STD_INPUT_HANDLE:  return (HANDLE)(intptr_t)-10;
        case STD_OUTPUT_HANDLE: return (HANDLE)(intptr_t)-11;
        case STD_ERROR_HANDLE:  return (HANDLE)(intptr_t)-12;
        default: SetLastError(6); /* ERROR_INVALID_HANDLE */ return NULL;
    }
}

/* WriteFile / ReadFile via Nt */
BOOL WINAPI WriteFile(HANDLE hFile, const void *buf, DWORD n, DWORD *written, void *overlapped) {
    (void)overlapped;
    if (!hFile || hFile==(HANDLE)(intptr_t)-1) { SetLastError(6); return 0; }
    IO_STATUS_BLOCK ios;
    LARGE_INTEGER off; off.QuadPart=0;
    NTSTATUS s = NtWriteFile(hFile, NULL, NULL, NULL, &ios, (PVOID)buf, n, NULL, NULL);
    if (s==STATUS_SUCCESS || s==STATUS_PENDING) {
        if (written) *written = (DWORD)ios.Information;
        SetLastError(0);
        return 1;
    }
    SetLastErrorFromStatus(s);
    return 0;
}
BOOL WINAPI ReadFile(HANDLE hFile, void *buf, DWORD n, DWORD *read, void *overlapped) {
    (void)overlapped;
    if (!hFile || hFile==(HANDLE)(intptr_t)-1) { SetLastError(6); return 0; }
    IO_STATUS_BLOCK ios;
    NTSTATUS s = NtReadFile(hFile, NULL, NULL, NULL, &ios, buf, n, NULL, NULL);
    if (s==STATUS_SUCCESS || s==STATUS_PENDING) {
        if (read) *read = (DWORD)ios.Information;
        SetLastError(0);
        return 1;
    }
    SetLastErrorFromStatus(s);
    return 0;
}

/* CreateFileW - minimal mapping to NtCreateFile */
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

HANDLE WINAPI CreateFileW(const WCHAR *lpFileName, DWORD desiredAccess, DWORD shareMode, void *secAttr, DWORD creationDisp, DWORD flags, HANDLE templ) {
    (void)shareMode; (void)secAttr; (void)flags; (void)templ;
    if (!lpFileName) { SetLastError(87); return (HANDLE)(intptr_t)-1; }
    UNICODE_STRING us;
    size_t len=0; while(lpFileName[len]) len++;
    us.Length = (USHORT)(len*sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    us.Buffer = (PWSTR)lpFileName;
    OBJECT_ATTRIBUTES oa;
    oa.Length = sizeof(oa);
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = 0x40; /* OBJ_CASE_INSENSITIVE */
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    HANDLE h=NULL;
    IO_STATUS_BLOCK ios;
    ULONG disp = win32_create_disposition_to_nt(creationDisp);
    ULONG desired = 0;
    if (desiredAccess & GENERIC_READ) desired |= 0x80000000 | 0x00100080; /* GENERIC_READ + SYNCHRONIZE */
    if (desiredAccess & GENERIC_WRITE) desired |= 0x40000000 | 0x00100080;
    if (desired==0) desired = 0x120089; /* default */
    NTSTATUS s = NtCreateFile(&h, desired, &oa, &ios, NULL, 0, shareMode, disp, 0, NULL, 0);
    if (s==STATUS_SUCCESS) { SetLastError(0); return h; }
    SetLastErrorFromStatus(s);
    return (HANDLE)(intptr_t)-1;
}

BOOL WINAPI CloseHandle(HANDLE h) {
    if (!h || h==(HANDLE)(intptr_t)-1) { SetLastError(6); return 0; }
    NTSTATUS s = NtClose(h);
    if (s==STATUS_SUCCESS) { SetLastError(0); return 1; }
    SetLastErrorFromStatus(s);
    return 0;
}

void WINAPI ExitProcess(UINT c) {
    NtTerminateProcess((HANDLE)(intptr_t)-1, (NTSTATUS)c);
    while(1) __asm__ volatile("hlt");
}

/* Virtual memory wrappers */
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000

LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    PVOID base = addr;
    SIZE_T sz = size;
    ULONG ntProt=0;
    switch(prot & 0xFF){
        case 0x01: ntProt=1; break; /* PAGE_NOACCESS */
        case 0x02: ntProt=2; break; /* PAGE_READONLY */
        case 0x04: ntProt=4; break; /* PAGE_READWRITE */
        case 0x10: ntProt=16; break;/* PAGE_EXECUTE */
        case 0x20: ntProt=32; break;/* PAGE_EXECUTE_READ */
        case 0x40: ntProt=64; break;/* PAGE_EXECUTE_READWRITE */
        default: ntProt=4; break;
    }
    NTSTATUS s = NtAllocateVirtualMemory((HANDLE)(intptr_t)-1, &base, 0, &sz, type, ntProt);
    if (s==STATUS_SUCCESS) { SetLastError(0); return base; }
    SetLastErrorFromStatus(s);
    return NULL;
}
BOOL WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    PVOID base=addr;
    SIZE_T sz=size;
    NTSTATUS s = NtFreeVirtualMemory((HANDLE)(intptr_t)-1, &base, &sz, type);
    if (s==STATUS_SUCCESS) { SetLastError(0); return 1; }
    SetLastErrorFromStatus(s);
    return 0;
}
BOOL WINAPI VirtualProtect(LPVOID addr, SIZE_T size, DWORD newProt, DWORD *oldProt) {
    PVOID base=addr;
    SIZE_T sz=size;
    ULONG old=0;
    ULONG ntProt=0;
    if (!win32_protect_to_nt(newProt, &ntProt)) { SetLastError(87); /* ERROR_INVALID_PARAMETER */ return 0; }
    NTSTATUS s = NtProtectVirtualMemory((HANDLE)(intptr_t)-1, &base, &sz, ntProt, &old);
    if (s==STATUS_SUCCESS) { if(oldProt) *oldProt=old; SetLastError(0); return 1; }
    SetLastErrorFromStatus(s);
    return 0;
}

/* GetProcAddress / LoadLibraryW forwarding to Ldr - enhanced */
FARPROC WINAPI GetProcAddress(HMODULE mod, const char *name) {
    if (!mod || !name) { SetLastError(127); return NULL; }
    ANSI_STRING as;
    as.Length = (USHORT)strlen(name);
    as.MaximumLength = as.Length+1;
    as.Buffer = (PSZ)name;
    FARPROC p=NULL;
    NTSTATUS s = LdrGetProcedureAddress(NULL, mod, &as, 0, &p);
    if (s==STATUS_SUCCESS) { SetLastError(0); return p; }
    /* Fallback: try ntdll direct lookup via internal table (for GetModuleHandle case) */
    SetLastErrorFromStatus(s);
    return NULL;
}
HMODULE WINAPI GetModuleHandleA(const char *name) {
    if (!name) {
        // Return EXE base from PEB
        PTEB teb = NtCurrentTebMingw();
        if (teb && teb->ProcessEnvironmentBlock) {
            PEB *peb = teb->ProcessEnvironmentBlock;
            if (peb->ImageBaseAddress) return (HMODULE)peb->ImageBaseAddress;
        }
        SetLastError(0);
        return (HMODULE)0x140000000ULL; // fallback
    }
    HMODULE h=NULL;
    WCHAR w[260];
    size_t i;
    for(i=0;i<259 && name[i];i++) w[i]=(WCHAR)name[i];
    w[i]=0;
    NTSTATUS s = LdrGetDllHandle(NULL, w, &h);
    if (NT_SUCCESS(s) && h) { SetLastError(0); return h; }
    // Try LoadLibrary fallback?
    SetLastError(126);
    return NULL;
}
HMODULE WINAPI GetModuleHandleW(const WCHAR *name) {
    if (!name) {
        PTEB teb = NtCurrentTebMingw();
        if (teb && teb->ProcessEnvironmentBlock) {
            PEB *peb = teb->ProcessEnvironmentBlock;
            if (peb->ImageBaseAddress) return (HMODULE)peb->ImageBaseAddress;
        }
        return (HMODULE)0x140000000ULL;
    }
    HMODULE h=NULL;
    NTSTATUS s = LdrGetDllHandle(NULL, name, &h);
    if (NT_SUCCESS(s) && h) { SetLastError(0); return h; }
    SetLastError(126);
    return NULL;
}
HMODULE WINAPI LoadLibraryW(const WCHAR *name) {
    if (!name) { SetLastError(87); return NULL; }
    HMODULE h=NULL;
    NTSTATUS s = LdrLoadDll(NULL, NULL, 0, name, &h);
    if (s==STATUS_SUCCESS) { SetLastError(0); return h; }
    SetLastErrorFromStatus(s);
    return NULL;
}
HMODULE WINAPI LoadLibraryA(const char *name) {
    if (!name) { SetLastError(87); return NULL; }
    WCHAR w[260];
    size_t i;
    for(i=0;i<259 && name[i];i++) w[i]=(WCHAR)name[i];
    w[i]=0;
    return LoadLibraryW(w);
}
HMODULE WINAPI LoadLibraryExA(const char *name, HANDLE f, DWORD flags) { (void)f;(void)flags; return LoadLibraryA(name); }
HMODULE WINAPI LoadLibraryExW(const WCHAR *name, HANDLE f, DWORD flags) { (void)f;(void)flags; return LoadLibraryW(name); }
BOOL WINAPI FreeLibrary(HMODULE h) { (void)h; SetLastError(0); return 1; }

/* Heap - minimal bump using ntdll heap (or own) */
static uint8_t g_k32_heap[1024*1024];
static size_t g_k32_off=0;
HANDLE WINAPI GetProcessHeap(void) { return (HANDLE)g_k32_heap; }
LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    (void)heap;(void)flags;
    size = (size+15)&~15;
    if (g_k32_off + size > sizeof(g_k32_heap)) { SetLastError(8); return NULL; }
    void *p = g_k32_heap + g_k32_off;
    g_k32_off += size;
    SetLastError(0);
    return p;
}
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID mem) { (void)heap;(void)flags;(void)mem; SetLastError(0); return 1; }
LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size){ (void)heap;(void)flags;(void)mem;(void)size; return NULL; }
SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, const void *mem){ (void)heap;(void)flags;(void)mem; return 0; }

/* CriticalSection - single-thread no-op */
typedef struct { void *DebugInfo; LONG LockCount; LONG RecursionCount; HANDLE OwningThread; HANDLE LockSemaphore; ULONG_PTR SpinCount; } CRITICAL_SECTION;
void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs) { if(cs) memset(cs,0,sizeof(*cs)); }
void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }
void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }
void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }
BOOL WINAPI TryEnterCriticalSection(CRITICAL_SECTION *cs) { (void)cs; return 1; }

/* TLS */
/* The index bitmap is process-wide, so it lives in the PEB. */
DWORD WINAPI TlsAlloc(void) {
    PTEB teb = NtCurrentTebMingw();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    if (!peb) { SetLastError(8); return WIN32_TLS_OUT_OF_INDEXES; }
    DWORD idx = win32_tls_alloc(peb->TlsBitmapBits);
    SetLastError(idx == WIN32_TLS_OUT_OF_INDEXES ? 1087 /* ERROR_NO_MORE_ITEMS */ : 0);
    return idx;
}
BOOL WINAPI TlsFree(DWORD idx) {
    PTEB teb = NtCurrentTebMingw();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    if (!peb || !win32_tls_free(peb->TlsBitmapBits, idx)) { SetLastError(87); return 0; }
    SetLastError(0);
    return 1;
}
LPVOID WINAPI TlsGetValue(DWORD idx) {
    PTEB teb = NtCurrentTebMingw();
    if (!teb || idx>=64) return NULL;
    return teb->TlsSlots[idx];
}
BOOL WINAPI TlsSetValue(DWORD idx, LPVOID val) {
    PTEB teb = NtCurrentTebMingw();
    if (!teb || idx>=64) { SetLastError(87); return 0; }
    teb->TlsSlots[idx]=val;
    return 1;
}

/* String conversions - ASCII only */
int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, const char *mb, int cb, WCHAR *wc, int cc) {
    (void)cp;(void)flags;
    if (!mb) { SetLastError(87); return 0; }
    int len = (cb==-1) ? (int)strlen(mb)+1 : cb;
    if (!wc || cc==0) return len;
    int i;
    for(i=0;i<len && i<cc;i++) wc[i]=(WCHAR)(unsigned char)mb[i];
    return i;
}
int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *wc, int cc, char *mb, int cb, const char *def, BOOL *used) {
    (void)cp;(void)flags;(void)def;(void)used;
    if (!wc) { SetLastError(87); return 0; }
    int len = (cc==-1) ? 0 : cc;
    if (cc==-1) { while(wc[len]) len++; len++; }
    if (!mb || cb==0) return len;
    int i;
    for(i=0;i<len && i<cb;i++) mb[i]=(char)(wc[i] & 0xFF);
    return i;
}
BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE b) { (void)cp;(void)b; return 0; }

/* Process/Thread */
HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
HANDLE WINAPI GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
DWORD WINAPI GetCurrentProcessId(void) {
    PTEB teb = NtCurrentTebMingw();
    if (teb) return (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess;
    return 42;
}
DWORD WINAPI GetCurrentThreadId(void) {
    PTEB teb = NtCurrentTebMingw();
    if (teb) return (DWORD)(ULONG_PTR)teb->ClientId.UniqueThread;
    return 1;
}
BOOL WINAPI GetExitCodeProcess(HANDLE h, DWORD *ec) { (void)h; if(ec) *ec=0; return 1; }
BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD *ec) { (void)h; if(ec) *ec=0; return 1; }
DWORD WINAPI GetPriorityClass(HANDLE h) { (void)h; return 0x20; /* NORMAL */ }
int WINAPI GetThreadPriority(HANDLE h) { (void)h; return 0; }
BOOL WINAPI SetThreadPriority(HANDLE h, int p) { (void)h;(void)p; return 1; }
BOOL WINAPI TerminateThread(HANDLE h, DWORD ec) { (void)h;(void)ec; return 0; }
HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, void *start, void *param, DWORD flags, DWORD *id) {
    (void)sa;(void)stack;(void)start;(void)param;(void)flags;(void)id;
    SetLastError(120); return NULL;
}
HANDLE WINAPI CreateProcessA(const char *app, char *cmd, void *pa, void *ta, BOOL inh, DWORD flags, void *env, const char *dir, void *si, void *pi) {
    (void)app;(void)cmd;(void)pa;(void)ta;(void)inh;(void)flags;(void)env;(void)dir;(void)si;(void)pi;
    SetLastError(2); return NULL;
}
HANDLE WINAPI CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inh, DWORD flags, void *env, const WCHAR *dir, void *si, void *pi) {
    (void)app;(void)cmd;(void)pa;(void)ta;(void)inh;(void)flags;(void)env;(void)dir;(void)si;(void)pi;
    SetLastError(2); return NULL;
}
BOOL WINAPI IsDebuggerPresent(void) { return 0; }

/* File helpers */
HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (!name) { SetLastError(87); return (HANDLE)(intptr_t)-1; }
    WCHAR w[512];
    size_t i;
    for(i=0;i<511 && name[i];i++) w[i]=(WCHAR)(unsigned char)name[i];
    w[i]=0;
    return CreateFileW(w, access, share, sa, disp, flags, tmpl);
}
BOOL WINAPI DeleteFileA(const char *name) {
    if (!name) { SetLastError(87); return 0; }
    WCHAR w[512]; size_t i; for(i=0;i<511&&name[i];i++) w[i]=(WCHAR)name[i]; w[i]=0;
    // Use NtDeleteFile via NtCreateFile with delete? Simplify stub: try CreateFileW with DELETE then close
    SetLastError(2); return 0;
}
BOOL WINAPI DeleteFileW(const WCHAR *name) { (void)name; SetLastError(2); return 0; }
BOOL WINAPI CreateDirectoryA(const char *path, void *sa) { (void)path;(void)sa; SetLastError(0); return 1; }
BOOL WINAPI CreateDirectoryW(const WCHAR *path, void *sa) { (void)path;(void)sa; SetLastError(0); return 1; }
BOOL WINAPI RemoveDirectoryA(const char *path) { (void)path; SetLastError(0); return 1; }
BOOL WINAPI RemoveDirectoryW(const WCHAR *path) { (void)path; SetLastError(0); return 1; }

/* Temp path */
DWORD WINAPI GetTempPathA(DWORD len, char *buf) {
    const char *tmp="/tmp/";
    size_t need=strlen(tmp)+1;
    if (!buf || len==0) return (DWORD)need;
    size_t n = need < len ? need : len;
    memcpy(buf, tmp, n);
    if (n<len) buf[n-1]='\0';
    return (DWORD)need-1;
}
DWORD WINAPI GetTempPathW(DWORD len, WCHAR *buf) {
    const WCHAR tmp[]={ '/', 't','m','p','/',0 };
    size_t need=5+1;
    if (!buf || len==0) return (DWORD)need-1;
    size_t n = need < len ? need : len;
    for(size_t i=0;i<n;i++) buf[i]=tmp[i];
    return (DWORD)need-1;
}
UINT WINAPI GetTempFileNameA(const char *dir, const char *prefix, UINT uniq, char *out) {
    (void)dir;(void)prefix;(void)uniq;
    const char *name="/tmp/tmpfile.tmp";
    if(out) strcpy(out, name);
    return 1;
}
UINT WINAPI GetTempFileNameW(const WCHAR *dir, const WCHAR *prefix, UINT uniq, WCHAR *out) {
    (void)dir;(void)prefix;(void)uniq;
    const WCHAR name[]={'/','t','m','p','/','t','m','p',0};
    if(out) { size_t i; for(i=0;name[i];i++) out[i]=name[i]; out[i]=0; }
    return 1;
}
DWORD WINAPI GetBinaryTypeA(const char *app, DWORD *type){ (void)app; if(type) *type=6; return 1; }
DWORD WINAPI QueryDosDeviceW(const WCHAR *name, WCHAR *target, DWORD max) {
    (void)name;(void)target;(void)max; SetLastError(2); return 0;
}

/* Memory mapping */
HANDLE WINAPI CreateFileMappingA(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const char *name) {
    (void)f;(void)sa;(void)prot;(void)hi;(void)lo;(void)name; SetLastError(0); return (HANDLE)1;
}
HANDLE WINAPI CreateFileMappingW(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const WCHAR *name) {
    (void)f;(void)sa;(void)prot;(void)hi;(void)lo;(void)name; SetLastError(0); return (HANDLE)1;
}
LPVOID WINAPI MapViewOfFile(HANDLE m, DWORD access, DWORD offHi, DWORD offLo, SIZE_T size) {
    (void)m;(void)access;(void)offHi;(void)offLo;(void)size; SetLastError(8); return NULL;
}
BOOL WINAPI UnmapViewOfFile(const void *p) { (void)p; SetLastError(0); return 1; }
BOOL WINAPI SetEndOfFile(HANDLE h) { (void)h; SetLastError(0); return 1; }
DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method) { (void)h;(void)lo;(void)hi;(void)method; return 0; }

/* Sync */
HANDLE WINAPI CreateEventA(void *sa, BOOL man, BOOL init, const char *name){ (void)sa;(void)man;(void)init;(void)name; SetLastError(0); return (HANDLE)10; }
HANDLE WINAPI CreateEventW(void *sa, BOOL man, BOOL init, const WCHAR *name){ (void)sa;(void)man;(void)init;(void)name; return (HANDLE)10; }
HANDLE WINAPI CreateMutexA(void *sa, BOOL own, const char *name){ (void)sa;(void)own;(void)name; return (HANDLE)11; }
HANDLE WINAPI CreateMutexW(void *sa, BOOL own, const WCHAR *name){ (void)sa;(void)own;(void)name; return (HANDLE)11; }
HANDLE WINAPI CreateSemaphoreA(void *sa, LONG init, LONG max, const char *name){ (void)sa;(void)init;(void)max;(void)name; return (HANDLE)12; }
HANDLE WINAPI CreateSemaphoreW(void *sa, LONG init, LONG max, const WCHAR *name){ (void)sa;(void)init;(void)max;(void)name; return (HANDLE)12; }
BOOL WINAPI SetEvent(HANDLE h){ (void)h; return 1; }
BOOL WINAPI ResetEvent(HANDLE h){ (void)h; return 1; }
BOOL WINAPI ReleaseMutex(HANDLE h){ (void)h; return 1; }
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG inc, LONG *prev){ (void)h;(void)inc; if(prev) *prev=0; return 1; }
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms){ (void)h;(void)ms; return 0; /* WAIT_OBJECT_0 */ }
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL waitAll, DWORD ms){ (void)n;(void)hs;(void)waitAll;(void)ms; return 0; }
BOOL WINAPI DuplicateHandle(HANDLE srcP, HANDLE src, HANDLE dstP, HANDLE *dst, DWORD access, BOOL inh, DWORD opts){
    (void)srcP;(void)src;(void)dstP;(void)dst;(void)access;(void)inh;(void)opts; SetLastError(0); if(dst) *dst=src; return 1;
}
BOOL WINAPI GetHandleInformation(HANDLE h, DWORD *flags){ (void)h; if(flags) *flags=0; return 1; }
DWORD WINAPI GetProcessHandleCount(HANDLE p, DWORD *cnt){ (void)p; if(cnt) *cnt=5; return 1; }

/* Perf */
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *c){ if(c) c->QuadPart=123456; return 1; }
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *c){ if(c) c->QuadPart=1000000; return 1; }
void WINAPI Sleep(DWORD ms){
    LARGE_INTEGER d; d.QuadPart = -(LONGLONG)ms*10000LL;
    NtDelayExecution(0, &d);
}
void WINAPI SleepEx(DWORD ms, BOOL alert){ (void)alert; Sleep(ms); }

/* Exception */
void* WINAPI SetUnhandledExceptionFilter(void *f){ (void)f; return NULL; }
LONG WINAPI UnhandledExceptionFilter(void *info){ (void)info; return 1; }

/* VirtualQuery via NtQueryVirtualMemory */
typedef struct { PVOID BaseAddress; PVOID AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State; DWORD Protect; DWORD Type; } MEMORY_BASIC_INFORMATION;
SIZE_T WINAPI VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *info, SIZE_T len){
    if(!info || len < sizeof(*info)){ SetLastError(87); return 0; }
    PVOID base = (PVOID)addr;
    SIZE_T ret = 0;
    // NtQueryVirtualMemory class 0 = MemoryBasicInformation
    NTSTATUS s = NtQueryVirtualMemory((HANDLE)(intptr_t)-1, base, 0, info, sizeof(*info), &ret);
    if(NT_SUCCESS(s)){ SetLastError(0); return sizeof(*info); }
    SetLastErrorFromStatus(s);
    return 0;
}

/* File I/O extra */
BOOL WINAPI FlushFileBuffers(HANDLE h){ (void)h; return 1; }
DWORD WINAPI GetFileType(HANDLE h){ (void)h; return 1; }

#else
/* Linux stub - never linked as DLL */
#endif