/*
 * advapi32.c - registry subset of advapi32.dll on the NT registry services.
 *
 * Exports exactly what the differential probes import (advapi32.def): the
 * Reg*A functions for HKEY_LOCAL_MACHINE and HKEY_CURRENT_USER, and
 * ImpersonateNamedPipeClient, which has no NT service behind it yet and fails
 * with ERROR_CALL_NOT_IMPLEMENTED. Pure mapping lives in advapi_pure.h.
 */
#ifdef __MINGW32__
#include "nt_types.h"
#include "ntstatus.h"
#include "advapi_pure.h"
#include <stddef.h>
#include <stdint.h>
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#ifndef HINSTANCE
typedef PVOID HINSTANCE;
#endif
#ifndef HKEY
typedef HANDLE HKEY;
#endif

#define ADV_OBJ_CASE_INSENSITIVE 0x00000040UL
#define ADV_KEY_READ_ACCESS 0x00020019UL
#define ADV_DELETE_ACCESS 0x00010000UL
#define ADV_KEY_VALUE_PARTIAL_INFORMATION 2UL
#define ADV_MEM_COMMIT_RESERVE 0x3000UL
#define ADV_PAGE_READWRITE 0x04UL

__declspec(dllimport) extern NTSTATUS NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
__declspec(dllimport) extern NTSTATUS NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG,
                                                  PUNICODE_STRING, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtDeleteKey(HANDLE);
__declspec(dllimport) extern NTSTATUS NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtDeleteValueKey(HANDLE, PUNICODE_STRING);
__declspec(dllimport) extern NTSTATUS NtClose(HANDLE);
__declspec(dllimport) extern NTSTATUS NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID *, PSIZE_T, ULONG);
__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);

/* DllMain: no per-process state. */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, PVOID v) { (void)h; (void)r; (void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v) { return DllMain(h, r, v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v) { return DllMain(h, r, v); }

/* TEB LastErrorValue, as kernel32.c writes it (ImpersonateNamedPipeClient only). */
static void adv_set_last_error(DWORD e)
{
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    if (teb) teb->LastErrorValue = e;
}

static LONG adv_status_to_error(NTSTATUS s)
{
    if (NT_SUCCESS(s)) return (LONG)ADV_ERROR_SUCCESS;
    return (LONG)RtlNtStatusToDosError(s);
}

/* Builds the NT object name for a subkey. A predefined root is absolute
 * (\Registry\Machine\...), so RootDirectory stays NULL. An open key handle is
 * the root, and the subkey is relative to it. Returns a Win32 error code. */
static LONG adv_build_key_oa(unsigned long hkey, const char *sub,
                             WCHAR *name, size_t name_cap,
                             UNICODE_STRING *us, OBJECT_ATTRIBUTES *oa, HANDLE *root)
{
    char path[512];
    const char *base = adv_predef_root_path(hkey);
    size_t n = 0;

    if (adv_is_predef_key(hkey) && !base) return (LONG)ADV_ERROR_INVALID_HANDLE;

    if (base) {
        for (const char *p = base; *p; ++p) {
            if (n + 1 >= sizeof(path)) return (LONG)ADV_ERROR_INVALID_PARAMETER;
            path[n++] = *p;
        }
        if (sub && sub[0]) {
            if (n + 1 >= sizeof(path)) return (LONG)ADV_ERROR_INVALID_PARAMETER;
            path[n++] = '\\';
            for (const char *p = sub; *p; ++p) {
                if (n + 1 >= sizeof(path)) return (LONG)ADV_ERROR_INVALID_PARAMETER;
                path[n++] = *p;
            }
        }
        path[n] = '\0';
        *root = NULL;
    } else {
        /* An open key handle. An empty subkey would need a second handle to
         * the same key, which this shim does not make. */
        if (!sub || !sub[0]) return (LONG)ADV_ERROR_INVALID_PARAMETER;
        for (const char *p = sub; *p; ++p) {
            if (n + 1 >= sizeof(path)) return (LONG)ADV_ERROR_INVALID_PARAMETER;
            path[n++] = *p;
        }
        path[n] = '\0';
        *root = (HANDLE)(uintptr_t)hkey;
    }

    long units = adv_ascii_to_utf16((unsigned short *)name, name_cap, path);
    if (units < 0 || units == 0) return (LONG)ADV_ERROR_INVALID_PARAMETER;
    unsigned long bytes = adv_utf16_bytes((unsigned long)units);
    if (bytes == 0) return (LONG)ADV_ERROR_INVALID_PARAMETER;

    us->Length = (USHORT)bytes;
    us->MaximumLength = (USHORT)(bytes + 2);
    us->Buffer = name;

    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->RootDirectory = *root;
    oa->ObjectName = us;
    oa->Attributes = ADV_OBJ_CASE_INSENSITIVE;
    oa->SecurityDescriptor = NULL;
    oa->SecurityQualityOfService = NULL;
    return (LONG)ADV_ERROR_SUCCESS;
}

/* Opens or creates a key. A predefined root has no NT handle of its own, so
 * the shim opens the root key here and the caller closes it. Returns the
 * Win32 error. *disp is set only for create. */
static LONG adv_open_key(unsigned long hkey, const char *sub, unsigned long access,
                         int create, HANDLE *out, ULONG *disp)
{
    WCHAR name[512];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE root;
    LONG err = adv_build_key_oa(hkey, sub, name, sizeof(name) / sizeof(name[0]), &us, &oa, &root);
    if (err != (LONG)ADV_ERROR_SUCCESS) return err;

    ULONG d = 0;
    NTSTATUS st = create
        ? NtCreateKey(out, adv_key_access(access), &oa, 0, NULL, 0, &d)
        : NtOpenKey(out, adv_key_access(access), &oa);
    if (disp) *disp = d;
    return adv_status_to_error(st);
}

/* A handle for hkey usable with the value and close calls. Predefined roots
 * are opened for the call and reported through *opened. */
static LONG adv_resolve(unsigned long hkey, unsigned long access, HANDLE *h, int *opened)
{
    *opened = 0;
    if (!adv_is_predef_key(hkey)) {
        if (hkey == 0) return (LONG)ADV_ERROR_INVALID_HANDLE;
        *h = (HANDLE)(uintptr_t)hkey;
        return (LONG)ADV_ERROR_SUCCESS;
    }
    LONG err = adv_open_key(hkey, NULL, access, 0, h, NULL);
    if (err == (LONG)ADV_ERROR_SUCCESS) *opened = 1;
    return err;
}

static void adv_close_if_opened(HANDLE h, int opened)
{
    if (opened) NtClose(h);
}

/* Value name as UNICODE_STRING. NULL or "" is the default value (length 0). */
static LONG adv_value_name(const char *in, WCHAR *buf, size_t cap, UNICODE_STRING *us)
{
    if (!in || !in[0]) {
        us->Length = 0;
        us->MaximumLength = 0;
        us->Buffer = buf;
        return (LONG)ADV_ERROR_SUCCESS;
    }
    long units = adv_ascii_to_utf16((unsigned short *)buf, cap, in);
    if (units < 0) return (LONG)ADV_ERROR_INVALID_PARAMETER;
    unsigned long bytes = adv_utf16_bytes((unsigned long)units);
    if (bytes == 0) return (LONG)ADV_ERROR_INVALID_PARAMETER;
    us->Length = (USHORT)bytes;
    us->MaximumLength = (USHORT)(bytes + 2);
    us->Buffer = buf;
    return (LONG)ADV_ERROR_SUCCESS;
}

/* Exports ------------------------------------------------------------------ */

LONG WINAPI RegCloseKey(HKEY hKey)
{
    unsigned long k = (unsigned long)(uintptr_t)hKey;
    if (adv_is_predef_key(k)) return (LONG)ADV_ERROR_SUCCESS; /* predefined keys do not close */
    if (!hKey) return (LONG)ADV_ERROR_INVALID_HANDLE;
    return adv_status_to_error(NtClose((HANDLE)hKey));
}

LONG WINAPI RegOpenKeyExA(HKEY hKey, const char *lpSubKey, DWORD ulOptions,
                          DWORD samDesired, HKEY *phkResult)
{
    (void)ulOptions;
    if (!phkResult) return (LONG)ADV_ERROR_INVALID_PARAMETER;
    HANDLE h = NULL;
    LONG err = adv_open_key((unsigned long)(uintptr_t)hKey, lpSubKey, samDesired, 0, &h, NULL);
    if (err == (LONG)ADV_ERROR_SUCCESS) *phkResult = h;
    return err;
}

LONG WINAPI RegCreateKeyExA(HKEY hKey, const char *lpSubKey, DWORD Reserved, char *lpClass,
                            DWORD dwOptions, DWORD samDesired, void *lpSecurityAttributes,
                            HKEY *phkResult, DWORD *lpdwDisposition)
{
    (void)Reserved; (void)lpClass; (void)dwOptions; (void)lpSecurityAttributes;
    if (!phkResult) return (LONG)ADV_ERROR_INVALID_PARAMETER;
    HANDLE h = NULL;
    ULONG disp = 0;
    LONG err = adv_open_key((unsigned long)(uintptr_t)hKey, lpSubKey, samDesired, 1, &h, &disp);
    if (err == (LONG)ADV_ERROR_SUCCESS) {
        *phkResult = h;
        if (lpdwDisposition) *lpdwDisposition = (DWORD)disp;
    }
    return err;
}

LONG WINAPI RegDeleteKeyA(HKEY hKey, const char *lpSubKey)
{
    HANDLE h = NULL;
    LONG err = adv_open_key((unsigned long)(uintptr_t)hKey, lpSubKey, ADV_DELETE_ACCESS, 0, &h, NULL);
    if (err != (LONG)ADV_ERROR_SUCCESS) return err;
    err = adv_status_to_error(NtDeleteKey(h));
    NtClose(h);
    return err;
}

LONG WINAPI RegDeleteValueA(HKEY hKey, const char *lpValueName)
{
    HANDLE h = NULL;
    int opened = 0;
    LONG err = adv_resolve((unsigned long)(uintptr_t)hKey, ADV_KEY_READ_ACCESS, &h, &opened);
    if (err != (LONG)ADV_ERROR_SUCCESS) return err;
    WCHAR name[260];
    UNICODE_STRING us;
    err = adv_value_name(lpValueName, name, sizeof(name) / sizeof(name[0]), &us);
    if (err == (LONG)ADV_ERROR_SUCCESS)
        err = adv_status_to_error(NtDeleteValueKey(h, &us));
    adv_close_if_opened(h, opened);
    return err;
}

LONG WINAPI RegQueryValueExA(HKEY hKey, const char *lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    HANDLE h = NULL;
    int opened = 0;
    LONG err = adv_resolve((unsigned long)(uintptr_t)hKey, ADV_KEY_READ_ACCESS, &h, &opened);
    if (err != (LONG)ADV_ERROR_SUCCESS) return err;

    WCHAR name[260];
    UNICODE_STRING us;
    err = adv_value_name(lpValueName, name, sizeof(name) / sizeof(name[0]), &us);
    if (err != (LONG)ADV_ERROR_SUCCESS) { adv_close_if_opened(h, opened); return err; }

    /* Size query: a zero-length buffer makes NtQueryValueKey report the needed
     * record size through ResultLength and STATUS_BUFFER_TOO_SMALL. */
    ULONG record_len = 0;
    unsigned char probe_byte = 0;
    NTSTATUS st = NtQueryValueKey(h, &us, ADV_KEY_VALUE_PARTIAL_INFORMATION,
                                  &probe_byte, 0, &record_len);
    if (st != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(st)) {
        adv_close_if_opened(h, opened);
        return adv_status_to_error(st);
    }

    /* Read the whole record into an allocated block. */
    PVOID rec = NULL;
    SIZE_T alloc = record_len ? record_len : 1;
    if (NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1, &rec, 0, &alloc,
                                           ADV_MEM_COMMIT_RESERVE, ADV_PAGE_READWRITE))) {
        ULONG got = 0;
        st = NtQueryValueKey(h, &us, ADV_KEY_VALUE_PARTIAL_INFORMATION, rec, (ULONG)alloc, &got);
    } else {
        st = STATUS_NO_MEMORY;
    }

    if (NT_SUCCESS(st)) {
        unsigned char *p = (unsigned char *)rec;
        ULONG type = *(ULONG *)(p + 4);
        ULONG data_len = *(ULONG *)(p + 8);
        if (!adv_partial_data_fits(record_len, data_len)) {
            st = STATUS_INVALID_PARAMETER;
        } else {
            unsigned long cb_in = lpcbData ? (unsigned long)*lpcbData : 0UL;
            unsigned long cb_out = 0;
            err = (LONG)adv_query_plan(data_len, lpData != NULL, cb_in, &cb_out);
            if (lpType) *lpType = type;
            if (lpcbData) *lpcbData = (DWORD)cb_out;
            if (err == (LONG)ADV_ERROR_SUCCESS && lpData) {
                unsigned long n = data_len < cb_in ? data_len : cb_in;
                for (unsigned long i = 0; i < n; ++i) lpData[i] = p[ADV_PARTIAL_DATA_OFFSET + i];
            }
        }
    } else {
        err = adv_status_to_error(st);
    }

    if (rec) {
        PVOID fp = rec;
        SIZE_T fsz = 0;
        NtFreeVirtualMemory((HANDLE)-1, &fp, &fsz, 0x8000UL /* MEM_RELEASE */);
    }
    adv_close_if_opened(h, opened);
    if (!NT_SUCCESS(st) && err == (LONG)ADV_ERROR_SUCCESS) err = adv_status_to_error(st);
    return err;
}

LONG WINAPI RegSetValueExA(HKEY hKey, const char *lpValueName, DWORD Reserved, DWORD dwType,
                           const BYTE *lpData, DWORD cbData)
{
    (void)Reserved;
    HANDLE h = NULL;
    int opened = 0;
    LONG err = adv_resolve((unsigned long)(uintptr_t)hKey, ADV_KEY_READ_ACCESS, &h, &opened);
    if (err != (LONG)ADV_ERROR_SUCCESS) return err;

    WCHAR name[260];
    UNICODE_STRING us;
    err = adv_value_name(lpValueName, name, sizeof(name) / sizeof(name[0]), &us);
    if (err == (LONG)ADV_ERROR_SUCCESS) {
        /* NtSetValueKey takes a data pointer even for a zero-length value. */
        PVOID data = (lpData && cbData) ? (PVOID)lpData : (PVOID)&name[0];
        err = adv_status_to_error(NtSetValueKey(h, &us, 0, (ULONG)dwType, data, (ULONG)cbData));
    }
    adv_close_if_opened(h, opened);
    return err;
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
