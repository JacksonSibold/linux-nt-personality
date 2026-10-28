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
#include "k32_module_pure.h"
#include "k32_io_pure.h"
#include "k32_file_pure.h"
#include "k32_sync_pure.h"
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
__declspec(dllimport) extern NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtDelayExecution(BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtCreateEvent(PHANDLE, ULONG, PVOID, ULONG, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtSetEvent(HANDLE, PLONG);
__declspec(dllimport) extern NTSTATUS NtResetEvent(HANDLE, PLONG);
__declspec(dllimport) extern NTSTATUS NtCreateMutant(PHANDLE, ULONG, PVOID, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtReleaseMutant(HANDLE, PLONG);
__declspec(dllimport) extern NTSTATUS NtCreateSemaphore(PHANDLE, ULONG, PVOID, LONG, LONG);
__declspec(dllimport) extern NTSTATUS NtReleaseSemaphore(HANDLE, LONG, PLONG);
__declspec(dllimport) extern NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtWaitForMultipleObjects(ULONG, PHANDLE, ULONG, BOOLEAN, PLARGE_INTEGER);

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

/* DllMain. At DLL_PROCESS_ATTACH the process heap is published in the PEB (see
 * k32_publish_process_heap). */
void k32_publish_process_heap(void); /* kernel32_heap.c */
BOOL NTAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) {
    (void)h;(void)v;
    if (r == 1 /* DLL_PROCESS_ATTACH */) k32_publish_process_heap();
    return 1;
}
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v){ return DllMain(h,r,v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v){ return DllMain(h,r,v); }

/* -------------------------------------------------------------------------- */
/* Standard handles, ReadFile and WriteFile (K32-19)                          */
/*                                                                            */
/* The standard handles are the process parameters' Standard* slots, not     */
/* constants. Status and count policy and the console rules are in           */
/* k32_io_pure.h.                                                             */
/* -------------------------------------------------------------------------- */

#define K32_INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* OVERLAPPED (Win32 layout). Only Offset and OffsetHigh are used: they give the
 * byte offset of a synchronous transfer. */
typedef struct {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} K32_OVERLAPPED;

/* FILE_FS_DEVICE_INFORMATION (FileFsDeviceInformation, class 4). */
typedef struct {
    ULONG DeviceType;
    ULONG Characteristics;
} K32_FS_DEVICE_INFORMATION;
#define K32_FILE_FS_DEVICE_INFORMATION_CLASS 4

/* The PEB slot for a standard handle index, or NULL when there are no
 * process parameters or the index is not a standard handle. */
static HANDLE *k32_std_slot(DWORD which)
{
    PTEB teb = NtCurrentTebMingw();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    if (!pp) return NULL;
    switch (k32_std_index(which)) {
    case 0:  return &pp->StandardInput;
    case 1:  return &pp->StandardOutput;
    case 2:  return &pp->StandardError;
    default: return NULL;
    }
}

HANDLE WINAPI GetStdHandle(DWORD which) {
    DWORD err = k32_std_handle_error(which);
    if (err) { SetLastError(err); return K32_INVALID_HANDLE_VALUE; }
    HANDLE *slot = k32_std_slot(which);
    SetLastError(0);
    return slot ? *slot : NULL;
}

BOOL WINAPI SetStdHandle(DWORD which, HANDLE h) {
    DWORD err = k32_std_handle_error(which);
    HANDLE *slot = err ? NULL : k32_std_slot(which);
    if (!slot) { SetLastError(err ? err : K32_ERROR_INVALID_HANDLE); return 0; }
    *slot = h;
    SetLastError(0);
    return 1;
}

/* The command line and environment block that binfmt_pe wrote into the PEB's
 * process parameters (pe_build_process_parameters). NULL without parameters. */
static RTL_USER_PROCESS_PARAMETERS *k32_process_parameters(void) {
    PTEB teb = NtCurrentTebMingw();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    return peb ? peb->ProcessParameters : NULL;
}

WCHAR *WINAPI GetCommandLineW(void) {
    RTL_USER_PROCESS_PARAMETERS *pp = k32_process_parameters();
    return pp ? pp->CommandLine.Buffer : NULL;
}

WCHAR *WINAPI GetEnvironmentStringsW(void) {
    RTL_USER_PROCESS_PARAMETERS *pp = k32_process_parameters();
    return pp ? (WCHAR *)pp->Environment : NULL;
}

/* Shared body of ReadFile and WriteFile. A pending request is waited for on
 * the handle, so the result is the final status either way. An overlapped
 * request is treated as synchronous (the offset is used, nothing is queued). */
static BOOL k32_transfer(HANDLE h, void *buf, DWORD n, DWORD *count, void *overlapped, int is_read)
{
    K32_OVERLAPPED *ov = (K32_OVERLAPPED *)overlapped;
    if (count) *count = 0;
    if (!h || h == K32_INVALID_HANDLE_VALUE) { SetLastError(K32_ERROR_INVALID_HANDLE); return 0; }
    if (!buf && n) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    LARGE_INTEGER off;
    PLARGE_INTEGER offp = NULL;
    if (ov) {
        off.QuadPart = k32_offset_from_parts(ov->Offset, ov->OffsetHigh);
        offp = &off;
    }
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    NTSTATUS s = is_read
        ? NtReadFile(h, NULL, NULL, NULL, &ios, buf, n, offp, NULL)
        : NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)buf, n, offp, NULL);
    if (s == STATUS_PENDING) {
        NTSTATUS w = NtWaitForSingleObject(h, 0, NULL);
        if (!NT_SUCCESS(w)) { SetLastErrorFromStatus(w); return 0; }
        s = ios.DUMMYUNIONNAME.Status;
    }
    DWORD eof = k32_io_eof_error(s, is_read, ov != NULL);
    if (eof) { SetLastError(eof); return 0; }
    if (k32_io_classify(s, is_read) != K32_IO_DONE) { SetLastErrorFromStatus(s); return 0; }
    if (count) *count = k32_io_count(s, ios.Information);
    /* Success leaves the last error alone (Windows ReadFile/WriteFile do not clear it;
     * probe_handles golden keeps the stale error across printf output). */
    return 1;
}

BOOL WINAPI WriteFile(HANDLE hFile, const void *buf, DWORD n, DWORD *written, void *overlapped) {
    return k32_transfer(hFile, (void *)buf, n, written, overlapped, 0);
}

BOOL WINAPI ReadFile(HANDLE hFile, void *buf, DWORD n, DWORD *read, void *overlapped) {
    return k32_transfer(hFile, buf, n, read, overlapped, 1);
}

/* -------------------------------------------------------------------------- */
/* Files: names and create (K32-8, K32-15)                                    */
/*                                                                            */
/* A Win32 name is normalised to an absolute DOS path, then to an NT name     */
/* (\??\...), in static buffers. These calls are not reentrant (see K32-14).  */
/* The access, share, flag, result and path rules are in k32_file_pure.h.     */
/* -------------------------------------------------------------------------- */

#define K32_PATH_CAP (K32_PATH_MAX_CHARS + 8u)
#define K32_INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define K32_INVALID_FILE_SIZE       0xFFFFFFFFu

static WCHAR g_k32_dos[K32_PATH_CAP];
static WCHAR g_k32_nt[K32_PATH_CAP + 8u];
static WCHAR g_k32_wide[K32_PATH_CAP];

__declspec(dllimport) extern NTSTATUS NtQueryAttributesFile(POBJECT_ATTRIBUTES, PVOID);
__declspec(dllimport) extern NTSTATUS NtFlushBuffersFile(HANDLE, PIO_STATUS_BLOCK);

/* Length of a name, scanned at most to the long-path limit plus one. */
static size_t k32_name_len(const WCHAR *s)
{
    size_t n = 0;
    while (n <= K32_PATH_MAX_CHARS && s[n]) ++n;
    return n;
}

/* The process current directory (RTL_USER_PROCESS_PARAMETERS.CurrentDirectory). */
static void k32_current_dir(const WCHAR **buf, size_t *n)
{
    PTEB teb = NtCurrentTebMingw();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    if (!pp) { *buf = NULL; *n = 0; return; }
    *buf = pp->CurrentDirectory.DosPath.Buffer;
    *n = pp->CurrentDirectory.DosPath.Length / sizeof(WCHAR);
}

/* Win32 name to the NT object name in g_k32_nt. Returns 0 or a Win32 error. */
static DWORD k32_nt_name(const WCHAR *name, size_t n, UNICODE_STRING *us)
{
    const WCHAR *cwd = NULL;
    size_t cwdn = 0, dn = 0;
    k32_current_dir(&cwd, &cwdn);
    DWORD err = k32_path_dos_normalise(name, n, cwd, cwdn, g_k32_dos, K32_PATH_CAP, &dn);
    if (err) return err;
    size_t nn = k32_path_to_nt(g_k32_dos, dn, g_k32_nt, K32_PATH_CAP + 8u);
    if (!nn) return K32_ERROR_FILENAME_EXCED_RANGE;
    /* Length and MaximumLength (Length + 2) are USHORT (K32-15). */
    if (nn * sizeof(WCHAR) + sizeof(WCHAR) > 0xFFFFu) return K32_ERROR_FILENAME_EXCED_RANGE;
    g_k32_nt[nn] = 0;
    us->Length = (USHORT)(nn * sizeof(WCHAR));
    us->MaximumLength = (USHORT)(us->Length + sizeof(WCHAR));
    us->Buffer = g_k32_nt;
    return 0;
}

static void k32_init_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us)
{
    oa->Length = sizeof(*oa);
    oa->RootDirectory = NULL;
    oa->ObjectName = us;
    oa->Attributes = 0x40; /* OBJ_CASE_INSENSITIVE */
    oa->SecurityDescriptor = NULL;
    oa->SecurityQualityOfService = NULL;
}

/* Widens an ANSI name into g_k32_wide. Returns 0 or a Win32 error. */
static DWORD k32_widen_name(const char *name)
{
    if (!name) return K32_ERROR_INVALID_PARAMETER;
    if (k32_wide_from_ansi(name, g_k32_wide, K32_PATH_CAP) == (size_t)-1) return K32_ERROR_FILENAME_EXCED_RANGE;
    return 0;
}

HANDLE WINAPI CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE templ)
{
    (void)sa; (void)templ;
    if (!name) { SetLastError(K32_ERROR_INVALID_PARAMETER); return K32_INVALID_HANDLE_VALUE; }
    if (!k32_disposition_valid(disp)) { SetLastError(K32_ERROR_INVALID_PARAMETER); return K32_INVALID_HANDLE_VALUE; }
    UNICODE_STRING us;
    DWORD err = k32_nt_name(name, k32_name_len(name), &us);
    if (err) { SetLastError(err); return K32_INVALID_HANDLE_VALUE; }
    OBJECT_ATTRIBUTES oa;
    k32_init_oa(&oa, &us);
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    HANDLE h = NULL;
    NTSTATUS s = NtCreateFile(&h, k32_file_access_to_nt(access), &oa, &ios, NULL,
                              k32_file_attributes_to_nt(flags), k32_file_share_to_nt(share),
                              win32_create_disposition_to_nt(disp), k32_file_options(flags), NULL, 0);
    if (!NT_SUCCESS(s)) {
        DWORD o = k32_create_error_override(disp, s);
        if (o) SetLastError(o);
        else SetLastErrorFromStatus(s);
        return K32_INVALID_HANDLE_VALUE;
    }
    SetLastError(k32_create_success_error(disp, ios.Information));
    return h;
}

BOOL WINAPI CloseHandle(HANDLE h) {
    if (!h) { SetLastError(6); return 0; }
    NTSTATUS s = NtClose(h);
    /* Success leaves the last error alone, as Windows does (probe_handles golden:
     * CloseHandle(pseudo) returns TRUE and keeps the stale error 6). */
    if (s==STATUS_SUCCESS) return 1;
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
        default:
            /* K32-16: an unsupported protection fails; it is not silently PAGE_READWRITE. */
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return NULL;
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

/* -------------------------------------------------------------------------- */
/* Module APIs (K32-4)                                                        */
/*                                                                            */
/* A Win32 HMODULE is the image base (LDR_DATA_TABLE_ENTRY.DllBase). The      */
/* loader's own handle is the entry address (LdrLoadDll returns &entry), so   */
/* these functions convert between the two by walking PEB.Ldr. The main       */
/* executable is not in the loader list; its handle is PEB.ImageBaseAddress.  */
/* Argument and error policy is in k32_module_pure.h.                         */
/* -------------------------------------------------------------------------- */

static PEB *k32_peb(void) {
    PTEB teb = NtCurrentTebMingw();
    return teb ? teb->ProcessEnvironmentBlock : NULL;
}

/* The process loader context (published by LdrInitializeThunk). */
static winldr_ctx_t *k32_loader(void) {
    return ldr_process_context(k32_peb());
}

/* SizeOfImage from the main image's PE header (PE32+: OptionalHeader + 56). */
static ULONG k32_image_size(const void *base) {
    const unsigned char *b = (const unsigned char *)base;
    ULONG nt = 0, size = 0;
    if (!b || b[0] != 'M' || b[1] != 'Z') return 0;
    memcpy(&nt, b + 0x3C, sizeof(nt));
    if (memcmp(b + nt, "PE\0\0", 4) != 0) return 0;
    memcpy(&size, b + nt + 24 + 56, sizeof(size));
    return size;
}

static HMODULE k32_main_module(void) {
    PEB *peb = k32_peb();
    return peb ? (HMODULE)peb->ImageBaseAddress : NULL;
}

static int k32_is_main(HMODULE h) {
    return h && h == k32_main_module();
}

/* Loader entry for a Win32 HMODULE (image base), or NULL. */
static LDR_DATA_TABLE_ENTRY *k32_entry_by_base(HMODULE base) {
    PEB *peb = k32_peb();
    if (!base || !peb || !peb->Ldr) return NULL;
    PLIST_ENTRY head = &peb->Ldr->InLoadOrderModuleList;
    for (PLIST_ENTRY e = head->Flink; e != head; e = e->Flink) {
        /* InLoadOrderLinks is the first member (ldr.h asserts the offset). */
        LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;
        if (m->DllBase == base) return m;
    }
    return NULL;
}

/* Win32 HMODULE that contains addr, or NULL. */
static HMODULE k32_module_at(uintptr_t addr) {
    PEB *peb = k32_peb();
    if (!peb) return NULL;
    if (k32_addr_in_module(addr, (uintptr_t)peb->ImageBaseAddress, k32_image_size(peb->ImageBaseAddress)))
        return (HMODULE)peb->ImageBaseAddress;
    if (!peb->Ldr) return NULL;
    PLIST_ENTRY head = &peb->Ldr->InLoadOrderModuleList;
    for (PLIST_ENTRY e = head->Flink; e != head; e = e->Flink) {
        LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;
        if (k32_addr_in_module(addr, (uintptr_t)m->DllBase, m->SizeOfImage))
            return m->DllBase;
    }
    return NULL;
}

/* Win32 error for a failed Ldr* status. */
static DWORD k32_status_error(NTSTATUS s) {
    DWORD e = k32_module_status_to_error(s);
    return e ? e : RtlNtStatusToDosError(s);
}

/* Length of a wide string, scanned at most to K32_MODULE_NAME_MAX + 1 so that
 * the length check can reject long names without reading past them. */
static size_t k32_wlen_bounded(const WCHAR *s) {
    size_t n = 0;
    while (s[n] && n <= K32_MODULE_NAME_MAX) ++n;
    return n;
}

/* Converts a loader status and handle to a Win32 HMODULE (the image base). */
static HMODULE k32_handle_result(NTSTATUS s, HMODULE entry) {
    if (s == STATUS_SUCCESS) return ((LDR_DATA_TABLE_ENTRY *)entry)->DllBase;
    SetLastError(k32_status_error(s));
    return NULL;
}

HMODULE WINAPI LoadLibraryExW(const WCHAR *name, HANDLE file, DWORD flags) {
    (void)file;
    if (!name) { SetLastError(K32_ERROR_INVALID_PARAMETER); return NULL; }
    size_t n = k32_wlen_bounded(name);
    DWORD err = k32_loadlibrary_name_error(name, n);
    if (err) { SetLastError(err); return NULL; }
    ULONG ldr_flags = 0;
    err = k32_loadlibraryex_flags(flags, &ldr_flags);
    if (err) { SetLastError(err); return NULL; }
    winldr_ctx_t *ctx = k32_loader();
    if (!ctx) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }

    /* A directory in the name becomes the search override; the base name is
     * normalised and gets ".dll" when it has no extension. */
    WCHAR dir[K32_MODULE_NAME_MAX], base[K32_MODULE_NAME_MAX];
    size_t dl = k32_name_dir_len(name, n);
    for (size_t i = 0; i < dl; ++i) dir[i] = name[i];
    dir[dl] = 0;
    size_t bl = k32_module_name_normalise(name, n, base, K32_MODULE_NAME_MAX);
    if (!bl) { SetLastError(K32_ERROR_MOD_NOT_FOUND); return NULL; }

    HMODULE entry = NULL;
    NTSTATUS s = LdrLoadDll(ctx, dl ? dir : NULL, ldr_flags, base, &entry);
    return k32_handle_result(s, entry);
}

HMODULE WINAPI LoadLibraryW(const WCHAR *name) {
    return LoadLibraryExW(name, NULL, 0);
}

HMODULE WINAPI LoadLibraryExA(const char *name, HANDLE file, DWORD flags) {
    if (!name) { SetLastError(K32_ERROR_INVALID_PARAMETER); return NULL; }
    WCHAR w[K32_MODULE_NAME_MAX];
    if (k32_wide_from_ansi(name, w, K32_MODULE_NAME_MAX) == (size_t)-1) {
        SetLastError(K32_ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    return LoadLibraryExW(w, file, flags);
}

HMODULE WINAPI LoadLibraryA(const char *name) {
    return LoadLibraryExA(name, NULL, 0);
}

BOOL WINAPI FreeLibrary(HMODULE h) {
    if (!h) { SetLastError(6); /* ERROR_INVALID_HANDLE */ return 0; }
    if (k32_is_main(h)) return 1;               /* the main module is never unloaded */
    winldr_ctx_t *ctx = k32_loader();
    LDR_DATA_TABLE_ENTRY *m = k32_entry_by_base(h);
    if (!ctx || !m) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    NTSTATUS s = LdrUnloadDll(ctx, (HMODULE)m);
    if (s == STATUS_SUCCESS) return 1;
    SetLastError(k32_status_error(s));
    return 0;
}

/* Looks a module up by name (after normalisation). Returns the Win32 HMODULE,
 * or NULL with the error set. Does not change the reference count. */
static HMODULE k32_find_by_name(const WCHAR *name, size_t n) {
    DWORD err = k32_loadlibrary_name_error(name, n);
    if (err) { SetLastError(err); return NULL; }
    winldr_ctx_t *ctx = k32_loader();
    if (!ctx) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    WCHAR base[K32_MODULE_NAME_MAX];
    if (!k32_module_name_normalise(name, n, base, K32_MODULE_NAME_MAX)) {
        SetLastError(K32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    HMODULE entry = NULL;
    NTSTATUS s = LdrGetDllHandle(ctx, base, &entry);
    return k32_handle_result(s, entry);
}

HMODULE WINAPI GetModuleHandleW(const WCHAR *name) {
    if (!name) return k32_main_module();
    return k32_find_by_name(name, k32_wlen_bounded(name));
}

HMODULE WINAPI GetModuleHandleA(const char *name) {
    if (!name) return k32_main_module();
    WCHAR w[K32_MODULE_NAME_MAX];
    if (k32_wide_from_ansi(name, w, K32_MODULE_NAME_MAX) == (size_t)-1) {
        SetLastError(K32_ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    return GetModuleHandleW(w);
}

/* Shared body of GetModuleHandleExA and GetModuleHandleExW. With FROM_ADDRESS,
 * name is an address; otherwise it is a name (wide or ANSI) or NULL for the
 * main module. */
static BOOL k32_get_module_handle_ex(DWORD flags, const void *name, HMODULE *out, int wide) {
    DWORD err = k32_getmodulehandleex_check(flags, name, (void **)out);
    if (err) { SetLastError(err); return 0; }

    HMODULE base = NULL;
    if (flags & K32_GMH_FLAG_FROM_ADDRESS) {
        base = k32_module_at((uintptr_t)name);
        if (!base) { SetLastError(K32_ERROR_MOD_NOT_FOUND); return 0; }
    } else if (!name) {
        base = k32_main_module();
    } else if (wide) {
        base = k32_find_by_name((const WCHAR *)name, k32_wlen_bounded((const WCHAR *)name));
    } else {
        WCHAR w[K32_MODULE_NAME_MAX];
        if (k32_wide_from_ansi((const char *)name, w, K32_MODULE_NAME_MAX) == (size_t)-1) {
            SetLastError(K32_ERROR_FILENAME_EXCED_RANGE);
            return 0;
        }
        base = k32_find_by_name(w, k32_wlen_bounded(w));
    }
    if (!base) return 0;

    /* Reference counts: PIN pins; otherwise a reference is added unless
     * UNCHANGED_REFCOUNT. The main module is not in the loader list. */
    if (!k32_is_main(base)) {
        LDR_DATA_TABLE_ENTRY *m = k32_entry_by_base(base);
        winldr_ctx_t *ctx = k32_loader();
        if (!m || !ctx) { SetLastError(K32_ERROR_MOD_NOT_FOUND); return 0; }
        if (flags & K32_GMH_FLAG_PIN)
            LdrAddRefDll(ctx, LDR_DLL_PIN, (HMODULE)m);
        else if (!(flags & K32_GMH_FLAG_UNCHANGED_REFCOUNT))
            LdrAddRefDll(ctx, 0, (HMODULE)m);
    }
    *out = base;
    return 1;
}

BOOL WINAPI GetModuleHandleExW(DWORD flags, const WCHAR *name, HMODULE *out) {
    return k32_get_module_handle_ex(flags, name, out, 1);
}

BOOL WINAPI GetModuleHandleExA(DWORD flags, const char *name, HMODULE *out) {
    return k32_get_module_handle_ex(flags, name, out, 0);
}

/* Full path of a module, or the executable's image path for the main module.
 * Returns the source buffer and its length in characters. */
static const WCHAR *k32_module_path(HMODULE h, size_t *n, DWORD *err) {
    PEB *peb = k32_peb();
    *n = 0;
    if (!h || k32_is_main(h)) {
        if (peb && peb->ProcessParameters) {
            const UNICODE_STRING *ip = &peb->ProcessParameters->ImagePathName;
            *n = ip->Length / sizeof(WCHAR);
            return ip->Buffer;
        }
        return NULL;
    }
    LDR_DATA_TABLE_ENTRY *m = k32_entry_by_base(h);
    if (!m) { *err = K32_ERROR_MOD_NOT_FOUND; return NULL; }
    *n = m->FullDllName.Length / sizeof(WCHAR);
    return m->FullDllName.Buffer;
}

/* GetModuleFileName: applies the MSDN truncation rules (return value is size
 * and ERROR_INSUFFICIENT_BUFFER when the name does not fit). */
DWORD WINAPI GetModuleFileNameW(HMODULE h, WCHAR *buf, DWORD size) {
    if (!buf && size) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    DWORD err = 0;
    size_t n = 0;
    const WCHAR *src = k32_module_path(h, &n, &err);
    if (err) { SetLastError(err); return 0; }
    DWORD ret = k32_filename_result(n, size, &err);
    size_t c = k32_filename_copy_len(n, size);
    for (size_t i = 0; i < c && src; ++i) buf[i] = src[i];
    if (size && c < size) buf[c] = 0;
    if (err) SetLastError(err);
    return ret;
}

DWORD WINAPI GetModuleFileNameA(HMODULE h, char *buf, DWORD size) {
    if (!buf && size) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    DWORD err = 0;
    size_t n = 0;
    const WCHAR *src = k32_module_path(h, &n, &err);
    if (err) { SetLastError(err); return 0; }
    DWORD ret = k32_filename_result(n, size, &err);
    size_t c = k32_filename_copy_len(n, size);
    for (size_t i = 0; i < c && src; ++i)
        buf[i] = src[i] <= 0xFF ? (char)(unsigned char)src[i] : '?';
    if (size && c < size) buf[c] = 0;
    if (err) SetLastError(err);
    return ret;
}

/* GetProcAddress by name, or by ordinal when proc is at most 0xFFFF. */
FARPROC WINAPI GetProcAddress(HMODULE h, const char *proc) {
    WORD ord = 0;
    int by_ord = 0;
    DWORD err = k32_getprocaddress_check(proc, &ord, &by_ord);
    if (err) { SetLastError(err); return NULL; }
    if (!h) { SetLastError(K32_ERROR_MOD_NOT_FOUND); return NULL; }
    /* The main executable's exports are not in the loader list yet. */
    LDR_DATA_TABLE_ENTRY *m = k32_entry_by_base(h);
    winldr_ctx_t *ctx = k32_loader();
    if (!m || !ctx) { SetLastError(K32_ERROR_PROC_NOT_FOUND); return NULL; }

    ANSI_STRING as;
    size_t len = 0;
    if (!by_ord) {
        while (proc[len]) ++len;
        if (len > 0xFFFE) { SetLastError(K32_ERROR_PROC_NOT_FOUND); return NULL; }
        as.Length = (USHORT)len;
        as.MaximumLength = (USHORT)(len + 1);
        as.Buffer = (PSZ)proc;
    }
    FARPROC p = NULL;
    NTSTATUS s = LdrGetProcedureAddress(ctx, (HMODULE)m, by_ord ? NULL : &as, ord, &p);
    if (s == STATUS_SUCCESS) return p;
    SetLastError(k32_status_error(s));
    return NULL;
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE h) {
    DWORD err = k32_disable_thread_calls_check(h);
    if (err) { SetLastError(err); return 0; }
    if (k32_is_main(h)) return 1;
    LDR_DATA_TABLE_ENTRY *m = k32_entry_by_base(h);
    if (!m) { SetLastError(K32_ERROR_MOD_NOT_FOUND); return 0; }
    /* Recorded only: the loader does not send thread notifications yet (LDR-3). */
    m->Flags = k32_disable_thread_calls_flags(m->Flags);
    return 1;
}

/* The heap API (GetProcessHeap, HeapAlloc, HeapCreate and the rest) is in kernel32_heap.c. */

/* Critical sections (K32-S1). The RTL protocol, with the counting rules in
 * k32_sync_pure.h. The wake primitive is an auto-reset NT event in
 * LockSemaphore, created on first use by the waiter or by the leaver, whichever
 * needs it first. A thread that owns the section re-enters by counting. */
typedef struct { void *DebugInfo; LONG LockCount; LONG RecursionCount; HANDLE OwningThread; HANDLE LockSemaphore; ULONG_PTR SpinCount; } CRITICAL_SECTION;

static HANDLE k32_cs_self(void)
{
    PTEB teb = NtCurrentTebMingw();
    return teb ? (HANDLE)(ULONG_PTR)teb->ClientId.UniqueThread : NULL;
}

/* The section's event, created if no one has yet. Returns NULL only when the
 * event cannot be created; the callers then retry after a short delay. */
static HANDLE k32_cs_event(CRITICAL_SECTION *cs)
{
    HANDLE ev = __atomic_load_n(&cs->LockSemaphore, __ATOMIC_ACQUIRE);
    if (ev) return ev;
    HANDLE created = NULL;
    /* SynchronizationEvent (1): auto-reset, so one signal wakes one waiter. */
    NTSTATUS s = NtCreateEvent(&created, WIN32_EVENT_ALL_ACCESS, NULL, 1, 0);
    if (!NT_SUCCESS(s)) return NULL;
    HANDLE expected = NULL;
    if (__atomic_compare_exchange_n(&cs->LockSemaphore, &expected, created, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return created;
    NtClose(created);   /* another thread created it first */
    return expected;
}

void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->LockCount = K32S_CS_LOCK_FREE;
}

void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs) return;
    HANDLE self = k32_cs_self();
    if (k32s_cs_owned_by(cs->OwningThread, self)) {
        cs->RecursionCount++;
        __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        return;
    }
    LONG after = __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
    if (k32s_cs_must_wait(after)) {
        HANDLE ev = k32_cs_event(cs);
        while (!ev) {
            /* Event creation failed: back off 1 ms and try again. */
            LARGE_INTEGER d; d.QuadPart = -10000;
            NtDelayExecution(0, &d);
            ev = k32_cs_event(cs);
        }
        NtWaitForSingleObject(ev, 0, NULL);
    }
    cs->OwningThread = self;
    cs->RecursionCount = 1;
}

void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs) return;
    if (--cs->RecursionCount == 0) {
        /* Clear the owner before the count changes, so a woken waiter never
         * sees a stale owner. */
        cs->OwningThread = NULL;
        LONG after = __atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        if (k32s_cs_must_signal(after)) {
            HANDLE ev = k32_cs_event(cs);
            while (!ev) {
                LARGE_INTEGER d; d.QuadPart = -10000;
                NtDelayExecution(0, &d);
                ev = k32_cs_event(cs);
            }
            NtSetEvent(ev, NULL);
        }
        return;
    }
    __atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
}

void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs) return;
    HANDLE ev = __atomic_exchange_n(&cs->LockSemaphore, NULL, __ATOMIC_ACQ_REL);
    if (ev) NtClose(ev);
    cs->OwningThread = NULL;
    cs->RecursionCount = 0;
    cs->LockCount = K32S_CS_LOCK_FREE;
    cs->SpinCount = 0;
}

BOOL WINAPI TryEnterCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs) return 0;
    HANDLE self = k32_cs_self();
    if (k32s_cs_owned_by(cs->OwningThread, self)) {
        cs->RecursionCount++;
        __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        return 1;
    }
    LONG expected = K32S_CS_LOCK_FREE;
    if (!__atomic_compare_exchange_n(&cs->LockCount, &expected, 0, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    cs->OwningThread = self;
    cs->RecursionCount = 1;
    return 1;
}

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

/* String conversions: MultiByteToWideChar and WideCharToMultiByte are in
 * kernel32_str.c (K32-C1). */
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
/* GetExitCodeThread, GetExitCodeProcess, TerminateThread, CreateThread and
 * CreateProcess are in kernel32_thread.c (K32-T1). */
BOOL WINAPI IsDebuggerPresent(void) { return 0; }

/* File names: delete, directories and attributes (K32-8). */

/* Opens and closes an object with the given access, disposition and options.
 * Returns the NTSTATUS; the open is the action (delete on close, or a create). */
static NTSTATUS k32_open_close(UNICODE_STRING *us, ULONG access, ULONG disp, ULONG opts)
{
    OBJECT_ATTRIBUTES oa;
    k32_init_oa(&oa, us);
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    HANDLE h = NULL;
    NTSTATUS s = NtCreateFile(&h, access, &oa, &ios, NULL, 0, K32_FILE_SHARE_ALL, disp, opts, NULL, 0);
    if (!NT_SUCCESS(s)) return s;
    return NtClose(h);
}

HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE templ)
{
    DWORD err = k32_widen_name(name);
    if (err) { SetLastError(err); return K32_INVALID_HANDLE_VALUE; }
    return CreateFileW(g_k32_wide, access, share, sa, disp, flags, templ);
}

/* DeleteFile opens the file with DELETE and FILE_DELETE_ON_CLOSE. A directory
 * is refused (STATUS_FILE_IS_A_DIRECTORY, ERROR_ACCESS_DENIED). */
BOOL WINAPI DeleteFileW(const WCHAR *name)
{
    if (!name) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    UNICODE_STRING us;
    DWORD err = k32_nt_name(name, k32_name_len(name), &us);
    if (err) { SetLastError(err); return 0; }
    NTSTATUS s = k32_open_close(&us, K32_DELETE | K32_SYNCHRONIZE, K32_NT_FILE_OPEN,
                                K32_FILE_NON_DIRECTORY_FILE | K32_FILE_DELETE_ON_CLOSE |
                                K32_FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    SetLastError(0);
    return 1;
}

BOOL WINAPI DeleteFileA(const char *name)
{
    DWORD err = k32_widen_name(name);
    if (err) { SetLastError(err); return 0; }
    return DeleteFileW(g_k32_wide);
}

BOOL WINAPI CreateDirectoryW(const WCHAR *path, void *sa)
{
    (void)sa;
    if (!path) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    UNICODE_STRING us;
    DWORD err = k32_nt_name(path, k32_name_len(path), &us);
    if (err) { SetLastError(err); return 0; }
    /* FILE_CREATE on an existing name fails with a collision, which maps to
     * ERROR_ALREADY_EXISTS as Win32 expects. */
    NTSTATUS s = k32_open_close(&us, K32_FILE_LIST_DIRECTORY | K32_FILE_READ_ATTRIBUTES | K32_SYNCHRONIZE,
                                K32_NT_FILE_CREATE,
                                K32_FILE_DIRECTORY_FILE | K32_FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    SetLastError(0);
    return 1;
}

BOOL WINAPI CreateDirectoryA(const char *path, void *sa)
{
    DWORD err = k32_widen_name(path);
    if (err) { SetLastError(err); return 0; }
    return CreateDirectoryW(g_k32_wide, sa);
}

/* RemoveDirectory opens the directory with DELETE and FILE_DELETE_ON_CLOSE. A
 * non-empty directory fails with STATUS_DIRECTORY_NOT_EMPTY. */
BOOL WINAPI RemoveDirectoryW(const WCHAR *path)
{
    if (!path) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    UNICODE_STRING us;
    DWORD err = k32_nt_name(path, k32_name_len(path), &us);
    if (err) { SetLastError(err); return 0; }
    NTSTATUS s = k32_open_close(&us, K32_DELETE | K32_SYNCHRONIZE, K32_NT_FILE_OPEN,
                                K32_FILE_DIRECTORY_FILE | K32_FILE_DELETE_ON_CLOSE |
                                K32_FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    SetLastError(0);
    return 1;
}

BOOL WINAPI RemoveDirectoryA(const char *path)
{
    DWORD err = k32_widen_name(path);
    if (err) { SetLastError(err); return 0; }
    return RemoveDirectoryW(g_k32_wide);
}

/* FILE_BASIC_INFORMATION (NtQueryAttributesFile). */
typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
} K32_FILE_BASIC_INFORMATION;

DWORD WINAPI GetFileAttributesW(const WCHAR *name)
{
    if (!name) { SetLastError(K32_ERROR_INVALID_PARAMETER); return K32_INVALID_FILE_ATTRIBUTES; }
    UNICODE_STRING us;
    DWORD err = k32_nt_name(name, k32_name_len(name), &us);
    if (err) { SetLastError(err); return K32_INVALID_FILE_ATTRIBUTES; }
    OBJECT_ATTRIBUTES oa;
    k32_init_oa(&oa, &us);
    K32_FILE_BASIC_INFORMATION bi;
    memset(&bi, 0, sizeof(bi));
    NTSTATUS s = NtQueryAttributesFile(&oa, &bi);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return K32_INVALID_FILE_ATTRIBUTES; }
    SetLastError(0);
    return k32_attributes_from_nt(bi.FileAttributes);
}

DWORD WINAPI GetFileAttributesA(const char *name)
{
    DWORD err = k32_widen_name(name);
    if (err) { SetLastError(err); return K32_INVALID_FILE_ATTRIBUTES; }
    return GetFileAttributesW(g_k32_wide);
}

/* Temp path: GetTempPathA and GetTempFileNameA are in kernel32_fs.c (K32-F1).
 * The wide variants below are not exported and still return the fixed path. */
DWORD WINAPI GetTempPathW(DWORD len, WCHAR *buf) {
    const WCHAR tmp[]={ '/', 't','m','p','/',0 };
    size_t need=5+1;
    if (!buf || len==0) return (DWORD)need-1;
    size_t n = need < len ? need : len;
    for(size_t i=0;i<n;i++) buf[i]=tmp[i];
    return (DWORD)need-1;
}
UINT WINAPI GetTempFileNameW(const WCHAR *dir, const WCHAR *prefix, UINT uniq, WCHAR *out) {
    (void)dir;(void)prefix;(void)uniq;
    const WCHAR name[]={'/','t','m','p','/','t','m','p',0};
    if(out) { size_t i; for(i=0;name[i];i++) out[i]=name[i]; out[i]=0; }
    return 1;
}
DWORD WINAPI QueryDosDeviceW(const WCHAR *name, WCHAR *target, DWORD max) {
    (void)name;(void)target;(void)max; SetLastError(2); return 0;
}

/* Memory mapping: CreateFileMapping, MapViewOfFile and UnmapViewOfFile are in
 * kernel32_mm.c (K32-M1). */

/* File size and position (K32-11, K32-8). FilePositionInformation is class 14,
 * FileStandardInformation class 5, FileEndOfFileInformation class 20. */
__declspec(dllimport) extern NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
#define K32_FILE_POSITION_CLASS        14
#define K32_FILE_STANDARD_CLASS        5
#define K32_FILE_END_OF_FILE_CLASS     20
#define K32_INVALID_SET_FILE_POINTER   0xFFFFFFFFUL

/* FILE_STANDARD_INFORMATION (class 5). */
typedef struct {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} K32_FILE_STANDARD_INFORMATION;

static NTSTATUS k32_query_position(HANDLE h, LONGLONG *pos)
{
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    LARGE_INTEGER p;
    p.QuadPart = 0;
    NTSTATUS s = NtQueryInformationFile(h, &ios, &p, sizeof(p), K32_FILE_POSITION_CLASS);
    if (NT_SUCCESS(s)) *pos = p.QuadPart;
    return s;
}

static NTSTATUS k32_query_standard(HANDLE h, K32_FILE_STANDARD_INFORMATION *info)
{
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    return NtQueryInformationFile(h, &ios, info, sizeof(*info), K32_FILE_STANDARD_CLASS);
}

static NTSTATUS k32_set_position(HANDLE h, LONGLONG pos)
{
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    LARGE_INTEGER p;
    p.QuadPart = pos;
    return NtSetInformationFile(h, &ios, &p, sizeof(p), K32_FILE_POSITION_CLASS);
}

static BOOL k32_handle_ok(HANDLE h)
{
    if (!h || h == K32_INVALID_HANDLE_VALUE) { SetLastError(K32_ERROR_INVALID_HANDLE); return 0; }
    return 1;
}

/* The target of a seek (method and distance). Sets the error and returns 0 on
 * failure. The current position or end of file is read only for the methods
 * that need it. */
static BOOL k32_seek(HANDLE h, DWORD method, LONGLONG dist, LONGLONG *target)
{
    LONGLONG cur = 0, end = 0;
    if (method == 1) {
        NTSTATUS s = k32_query_position(h, &cur);
        if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    } else if (method == 2) {
        K32_FILE_STANDARD_INFORMATION st;
        NTSTATUS s = k32_query_standard(h, &st);
        if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
        end = st.EndOfFile.QuadPart;
    }
    DWORD e = k32_seek_target(method, cur, end, dist, target);
    if (e) { SetLastError(e); return 0; }
    return 1;
}

DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method)
{
    if (!k32_handle_ok(h)) return K32_INVALID_SET_FILE_POINTER;
    LONGLONG dist = hi ? (LONGLONG)*hi * 4294967296LL + (LONGLONG)(DWORD)lo : (LONGLONG)lo;
    LONGLONG target = 0;
    if (!k32_seek(h, method, dist, &target)) return K32_INVALID_SET_FILE_POINTER;
    NTSTATUS s = k32_set_position(h, target);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return K32_INVALID_SET_FILE_POINTER; }
    if (hi) *hi = (LONG)(target >> 32);
    SetLastError(0);
    return (DWORD)(target & 0xFFFFFFFFLL);
}

BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *newpos, DWORD method)
{
    if (!k32_handle_ok(h)) return 0;
    LONGLONG target = 0;
    if (!k32_seek(h, method, dist.QuadPart, &target)) return 0;
    NTSTATUS s = k32_set_position(h, target);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    if (newpos) newpos->QuadPart = target;
    SetLastError(0);
    return 1;
}

BOOL WINAPI GetFileSizeEx(HANDLE h, LARGE_INTEGER *size)
{
    if (!k32_handle_ok(h)) return 0;
    if (!size) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    K32_FILE_STANDARD_INFORMATION st;
    NTSTATUS s = k32_query_standard(h, &st);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    size->QuadPart = st.EndOfFile.QuadPart;
    SetLastError(0);
    return 1;
}

/* Low DWORD returned; high DWORD to hi. INVALID_FILE_SIZE on failure. */
DWORD WINAPI GetFileSize(HANDLE h, DWORD *hi)
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) return K32_INVALID_FILE_SIZE;
    DWORD lo = 0, high = 0;
    k32_file_size_split(sz.QuadPart, &lo, &high);
    if (hi) *hi = high;
    SetLastError(0);
    return lo;
}

/* Truncates or extends the file at the current position (EOF is set to it). */
BOOL WINAPI SetEndOfFile(HANDLE h)
{
    if (!k32_handle_ok(h)) return 0;
    LONGLONG pos = 0;
    NTSTATUS s = k32_query_position(h, &pos);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    LARGE_INTEGER eof;
    eof.QuadPart = pos;
    s = NtSetInformationFile(h, &ios, &eof, sizeof(eof), K32_FILE_END_OF_FILE_CLASS);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    SetLastError(0);
    return 1;
}

BOOL WINAPI FlushFileBuffers(HANDLE h)
{
    if (!k32_handle_ok(h)) return 0;
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    NTSTATUS s = NtFlushBuffersFile(h, &ios);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    SetLastError(0);
    return 1;
}

/* Sync */
/* K32-7: these create real NT objects and return their handles. Named objects
 * are not supported (there is no shared object namespace here), so a name
 * fails with ERROR_CALL_NOT_IMPLEMENTED instead of returning an unshared object. */
#define K32_ERROR_CALL_NOT_IMPLEMENTED 120
static HANDLE k32_create_sync_result(NTSTATUS s, HANDLE h)
{
    HANDLE r = win32_sync_create_result(s, h);
    /* A successful create keeps the last error (probe_handles golden: 32 from an
     * earlier failed CreateFile survives CreateEvent and CreateMutex). */
    if (!r) SetLastErrorFromStatus(s);
    return r;
}
HANDLE WINAPI CreateEventA(void *sa, BOOL man, BOOL init, const char *name){
    (void)sa;
    if (name) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateEvent(&h, WIN32_EVENT_ALL_ACCESS, NULL, win32_event_type_to_nt(man), init ? 1 : 0);
    return k32_create_sync_result(s, h);
}
HANDLE WINAPI CreateEventW(void *sa, BOOL man, BOOL init, const WCHAR *name){
    (void)sa;
    if (name) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateEvent(&h, WIN32_EVENT_ALL_ACCESS, NULL, win32_event_type_to_nt(man), init ? 1 : 0);
    return k32_create_sync_result(s, h);
}
HANDLE WINAPI CreateMutexA(void *sa, BOOL own, const char *name){
    (void)sa;
    if (name) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateMutant(&h, WIN32_MUTANT_ALL_ACCESS, NULL, own ? 1 : 0);
    return k32_create_sync_result(s, h);
}
HANDLE WINAPI CreateMutexW(void *sa, BOOL own, const WCHAR *name){
    (void)sa;
    if (name) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateMutant(&h, WIN32_MUTANT_ALL_ACCESS, NULL, own ? 1 : 0);
    return k32_create_sync_result(s, h);
}
HANDLE WINAPI CreateSemaphoreA(void *sa, LONG init, LONG max, const char *name){
    (void)sa;
    if (name) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateSemaphore(&h, WIN32_SEMAPHORE_ALL_ACCESS, NULL, init, max);
    return k32_create_sync_result(s, h);
}
HANDLE WINAPI CreateSemaphoreW(void *sa, LONG init, LONG max, const WCHAR *name){
    (void)sa;
    if (name) { SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateSemaphore(&h, WIN32_SEMAPHORE_ALL_ACCESS, NULL, init, max);
    return k32_create_sync_result(s, h);
}
static BOOL k32_sync_status(NTSTATUS s)
{
    if (NT_SUCCESS(s)) { SetLastError(0); return 1; }
    SetLastErrorFromStatus(s);
    return 0;
}
BOOL WINAPI SetEvent(HANDLE h){ return k32_sync_status(NtSetEvent(h, NULL)); }
BOOL WINAPI ResetEvent(HANDLE h){ return k32_sync_status(NtResetEvent(h, NULL)); }
BOOL WINAPI ReleaseMutex(HANDLE h){ return k32_sync_status(NtReleaseMutant(h, NULL)); }
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG inc, LONG *prev){
    LONG old = 0;
    NTSTATUS s = NtReleaseSemaphore(h, inc, &old);
    if (prev && NT_SUCCESS(s)) *prev = old;
    return k32_sync_status(s);
}
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms){
    LARGE_INTEGER t;
    NTSTATUS s = NtWaitForSingleObject(h, 0, win32_timeout_to_nt(ms, &t));
    DWORD r = win32_wait_result(s);
    if (r == WIN32_WAIT_FAILED) { SetLastErrorFromStatus(s); return r; }
    SetLastError(0);
    return r;
}
/* K32-S2: WaitForMultipleObjects on NtWaitForMultipleObjects. The count and
 * the handle checks are in k32s_wait_multi_args_ok. */
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL waitAll, DWORD ms){
    if (!k32s_wait_multi_args_ok(n, (const void *const *)hs)) {
        SetLastError(K32S_ERROR_INVALID_PARAMETER);
        return K32S_WAIT_FAILED;
    }
    LARGE_INTEGER t;
    NTSTATUS s = NtWaitForMultipleObjects(n, (PHANDLE)hs, k32s_wait_type(waitAll), 0, win32_timeout_to_nt(ms, &t));
    DWORD r = k32s_wait_multi_result(s, n);
    if (r == K32S_WAIT_FAILED) { SetLastErrorFromStatus(s); return r; }
    SetLastError(0);
    return r;
}
__declspec(dllimport) extern NTSTATUS NtDuplicateObject(HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG);
/* K32-13: the target gets a handle to the same object, from NtDuplicateObject.
 * OBJ_INHERIT is 0x2; DUPLICATE_CLOSE_SOURCE and DUPLICATE_SAME_ACCESS keep
 * the Win32 values. */
BOOL WINAPI DuplicateHandle(HANDLE srcP, HANDLE src, HANDLE dstP, HANDLE *dst, DWORD access, BOOL inh, DWORD opts){
    HANDLE out = NULL;
    NTSTATUS s = NtDuplicateObject(srcP, src, dstP, &out, access, inh ? 0x2 : 0, opts);
    if (s != STATUS_SUCCESS) { SetLastErrorFromStatus(s); return 0; }
    if (dst) *dst = out;
    /* Success leaves the last error alone, as Windows does (probe_handles golden:
     * a successful DuplicateHandle keeps the stale 32 from an earlier failure). */
    return 1;
}

/* Perf */
/* K32-10: the counter and its frequency come from the kernel, not constants. */
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *c){
    LARGE_INTEGER count, freq;
    if(!c) return 0;
    if(!NT_SUCCESS(NtQueryPerformanceCounter(&count, &freq))) return 0;
    c->QuadPart = count.QuadPart;
    return 1;
}
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *c){
    LARGE_INTEGER count, freq;
    if(!c) return 0;
    if(!NT_SUCCESS(NtQueryPerformanceCounter(&count, &freq))) return 0;
    c->QuadPart = freq.QuadPart;
    return 1;
}
void WINAPI Sleep(DWORD ms){
    LARGE_INTEGER d; d.QuadPart = -(LONGLONG)ms*10000LL;
    NtDelayExecution(0, &d);
}
void WINAPI SleepEx(DWORD ms, BOOL alert){ (void)alert; Sleep(ms); }
/* K32-S3: milliseconds since the kernel started, from the performance counter. */
DWORD WINAPI GetTickCount(void){
    LARGE_INTEGER count, freq;
    if(!NT_SUCCESS(NtQueryPerformanceCounter(&count, &freq))) return 0;
    return k32s_tick_count_ms(count.QuadPart, freq.QuadPart);
}

/* Exception */
LONG WINAPI UnhandledExceptionFilter(void *info){ (void)info; return 1; }

/* VirtualQuery via NtQueryVirtualMemory */
typedef struct { PVOID BaseAddress; PVOID AllocationBase; DWORD AllocationProtect; WORD PartitionId; SIZE_T RegionSize; DWORD State; DWORD Protect; DWORD Type; } MEMORY_BASIC_INFORMATION;
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
/* -------------------------------------------------------------------------- */
/* Console and file type (K32-19)                                             */
/*                                                                            */
/* Console modes are kept in this process only (no console host is reached), */
/* so they do not survive a child process. The code page reports the OEM      */
/* default; SetConsole*CP is not implemented (see k32_io_pure.h).             */
/* -------------------------------------------------------------------------- */

#define K32_OEMCP_DEFAULT 437
static DWORD g_k32_console_in_mode = K32_CONSOLE_IN_DEFAULT;
static DWORD g_k32_console_out_mode = K32_CONSOLE_OUT_DEFAULT;

/* 0 when h is a console handle (output is set to 0 for the standard input
 * slot and 1 for any other console handle); otherwise the Win32 error. */
static DWORD k32_console_handle(HANDLE h, int *output)
{
    if (!h || h == K32_INVALID_HANDLE_VALUE) return K32_ERROR_INVALID_HANDLE;
    K32_FS_DEVICE_INFORMATION info;
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    NTSTATUS s = NtQueryVolumeInformationFile(h, &ios, &info, sizeof(info), K32_FILE_FS_DEVICE_INFORMATION_CLASS);
    if (!NT_SUCCESS(s)) return RtlNtStatusToDosError(s);
    if (!k32_device_is_console(info.DeviceType)) return K32_ERROR_INVALID_HANDLE;
    if (output) {
        HANDLE *in = k32_std_slot(K32_STD_INPUT_HANDLE);
        *output = !(in && *in == h);
    }
    return 0;
}

BOOL WINAPI GetConsoleMode(HANDLE h, DWORD *mode) {
    int output = 0;
    DWORD err = k32_console_handle(h, &output);
    if (err) { SetLastError(err); return 0; }
    if (!mode) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    *mode = output ? g_k32_console_out_mode : g_k32_console_in_mode;
    SetLastError(0);
    return 1;
}

BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode) {
    int output = 0;
    DWORD err = k32_console_handle(h, &output);
    if (err) { SetLastError(err); return 0; }
    if (!k32_console_mode_valid(mode, output)) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    if (output) g_k32_console_out_mode = mode;
    else        g_k32_console_in_mode = mode;
    SetLastError(0);
    return 1;
}

UINT WINAPI GetConsoleCP(void) { SetLastError(0); return K32_OEMCP_DEFAULT; }
UINT WINAPI GetConsoleOutputCP(void) { SetLastError(0); return K32_OEMCP_DEFAULT; }
BOOL WINAPI SetConsoleCP(UINT cp) { (void)cp; SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return 0; }
BOOL WINAPI SetConsoleOutputCP(UINT cp) { (void)cp; SetLastError(K32_ERROR_CALL_NOT_IMPLEMENTED); return 0; }

/* A console write or read: the handle must be a console; the transfer itself
 * is WriteFile or ReadFile. */
static BOOL k32_console_write(HANDLE h, const char *b, DWORD n, DWORD *written) {
    int output = 0;
    DWORD err = k32_console_handle(h, &output);
    if (err) { if (written) *written = 0; SetLastError(err); return 0; }
    return WriteFile(h, b, n, written, NULL);
}

static BOOL k32_console_read(HANDLE h, char *b, DWORD n, DWORD *read) {
    int output = 0;
    DWORD err = k32_console_handle(h, &output);
    if (err) { if (read) *read = 0; SetLastError(err); return 0; }
    return ReadFile(h, b, n, read, NULL);
}

BOOL WINAPI WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *reserved) {
    (void)reserved;
    return k32_console_write(h, (const char *)buf, n, written);
}

/* Text is narrowed in chunks (k32_narrow_counted); written counts characters. */
BOOL WINAPI WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *reserved) {
    (void)reserved;
    char tmp[K32_CONSOLE_CHUNK];
    DWORD total = 0;
    if (written) *written = 0;
    if (!buf && n) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    while (total < n) {
        size_t c = k32_console_chunk(n - total);
        DWORD w = 0;
        k32_narrow_counted(buf + total, c, tmp);
        if (!k32_console_write(h, tmp, (DWORD)c, &w)) { if (written) *written = total; return 0; }
        total += w;
        if (w < c) break;
    }
    if (written) *written = total;
    return 1;
}

BOOL WINAPI ReadConsoleA(HANDLE h, void *buf, DWORD n, DWORD *read, void *control) {
    (void)control;
    return k32_console_read(h, (char *)buf, n, read);
}

/* Reads in chunks (k32_widen_counted). A short read ends the call, as a
 * console line does. */
BOOL WINAPI ReadConsoleW(HANDLE h, WCHAR *buf, DWORD n, DWORD *read, void *control) {
    (void)control;
    char tmp[K32_CONSOLE_CHUNK];
    DWORD total = 0;
    if (read) *read = 0;
    if (!buf && n) { SetLastError(K32_ERROR_INVALID_PARAMETER); return 0; }
    while (total < n) {
        size_t c = k32_console_chunk(n - total);
        DWORD r = 0;
        if (!k32_console_read(h, tmp, (DWORD)c, &r)) { if (read) *read = total; return 0; }
        k32_widen_counted(tmp, r, buf + total);
        total += r;
        if (r < c) break;
    }
    if (read) *read = total;
    return 1;
}

/* FILE_TYPE_* from the device type (k32_io_pure.h). Failure returns
 * FILE_TYPE_UNKNOWN with the error set; an unknown device is success. */
DWORD WINAPI GetFileType(HANDLE h) {
    if (!h || h == K32_INVALID_HANDLE_VALUE) { SetLastError(K32_ERROR_INVALID_HANDLE); return 0; }
    K32_FS_DEVICE_INFORMATION info;
    IO_STATUS_BLOCK ios;
    ios.DUMMYUNIONNAME.Status = 0;
    ios.Information = 0;
    NTSTATUS s = NtQueryVolumeInformationFile(h, &ios, &info, sizeof(info), K32_FILE_FS_DEVICE_INFORMATION_CLASS);
    if (!NT_SUCCESS(s)) { SetLastErrorFromStatus(s); return 0; }
    SetLastError(0);
    return k32_file_type_from_device(info.DeviceType);
}

#else
/* Linux stub - never linked as DLL */
#endif