/*
 * kernel32_mm.c - file mapping shims: CreateFileMapping, MapViewOfFile and
 * UnmapViewOfFile on NtCreateSection, NtMapViewOfSection and
 * NtUnmapViewOfSection (K32-M1). The flag rules are in k32_mm_pure.h.
 *
 * Named mappings are not supported: the name argument fails with
 * ERROR_CALL_NOT_IMPLEMENTED, as CreateEventA does, because the kernel has no
 * object-name namespace for sections yet.
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
#include "k32_mm_pure.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef PVOID LPVOID;

#define K32MM_INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define K32MM_ERROR_INVALID_HANDLE 6u
#define K32MM_ERROR_CALL_NOT_IMPLEMENTED 120u
/* MapViewOfFile's ViewUnmap inheritance (NtMapViewOfSection InheritDisposition). */
#define K32MM_VIEW_UNMAP 2u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
__declspec(dllimport) extern NTSTATUS NtMapViewOfSection(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtUnmapViewOfSection(HANDLE, PVOID);

/* Defined in kernel32.c. */
void SetLastError(DWORD e);

static void k32mm_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

/* The section for a file or for anonymous memory. The file handle is NULL or
 * INVALID_HANDLE_VALUE for anonymous memory. */
static HANDLE k32mm_create(HANDLE file, DWORD prot, DWORD hi, DWORD lo)
{
    DWORD page = k32m_section_protect(prot);
    DWORD attrs = k32m_section_alloc_attrs(prot);
    if (!page || !attrs) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (file == NULL) {
        SetLastError(K32MM_ERROR_INVALID_HANDLE);
        return NULL;
    }
    int anonymous = (file == K32MM_INVALID_HANDLE_VALUE);
    if (!k32m_size_ok(anonymous, hi, lo)) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    /* A file section with no size takes the file's size: NULL MaximumSize. */
    LARGE_INTEGER max;
    PLARGE_INTEGER pmax = NULL;
    if (anonymous || hi != 0 || lo != 0) {
        max.QuadPart = (LONGLONG)k32m_offset(hi, lo);
        pmax = &max;
    }
    HANDLE h = NULL;
    NTSTATUS s = NtCreateSection(&h, K32M_SECTION_ALL_ACCESS, NULL, pmax, page, attrs,
                                 anonymous ? NULL : file);
    if (!NT_SUCCESS(s)) {
        k32mm_status(s);
        return NULL;
    }
    SetLastError(0);
    return h;
}

HANDLE WINAPI CreateFileMappingA(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const char *name)
{
    (void)sa;
    if (name) {
        SetLastError(K32MM_ERROR_CALL_NOT_IMPLEMENTED);
        return NULL;
    }
    return k32mm_create(f, prot, hi, lo);
}

HANDLE WINAPI CreateFileMappingW(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const WCHAR *name)
{
    (void)sa;
    if (name) {
        SetLastError(K32MM_ERROR_CALL_NOT_IMPLEMENTED);
        return NULL;
    }
    return k32mm_create(f, prot, hi, lo);
}

/* Maps a view of the current process. size 0 maps from the offset to the end
 * of the section. */
LPVOID WINAPI MapViewOfFile(HANDLE m, DWORD access, DWORD offHi, DWORD offLo, SIZE_T size)
{
    DWORD prot = k32m_view_protect(access);
    if (!prot) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!k32m_offset_aligned(offHi, offLo)) {
        SetLastError(K32M_ERROR_MAPPED_ALIGNMENT);
        return NULL;
    }
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)k32m_offset(offHi, offLo);
    PVOID base = NULL;
    SIZE_T view = size;
    NTSTATUS s = NtMapViewOfSection(m, K32MM_INVALID_HANDLE_VALUE, &base, 0, 0, &off, &view,
                                    K32MM_VIEW_UNMAP, 0, prot);
    if (!NT_SUCCESS(s)) {
        k32mm_status(s);
        return NULL;
    }
    SetLastError(0);
    return base;
}

BOOL WINAPI UnmapViewOfFile(const void *p)
{
    if (!p) {
        SetLastError(K32M_ERROR_INVALID_PARAMETER);
        return 0;
    }
    NTSTATUS s = NtUnmapViewOfSection(K32MM_INVALID_HANDLE_VALUE, (PVOID)p);
    if (!NT_SUCCESS(s)) {
        k32mm_status(s);
        return 0;
    }
    SetLastError(0);
    return 1;
}

#endif /* __MINGW32__ */
