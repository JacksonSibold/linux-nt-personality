/*
 * k32_mm_pure.h - pure flag mapping for the kernel32 file mapping shims.
 *
 * CreateFileMapping, MapViewOfFile and UnmapViewOfFile in kernel32_mm.c
 * map Win32 protection and access flags to the NT section and view values
 * with the helpers here. tests/ntdll-host checks them on the host.
 *
 * The NT page protections and the SEC_* allocation attributes have the same
 * numeric values in Win32 and NT, so the mapping is a validity check plus a
 * split of the two fields, not a translation table.
 */
#ifndef NTDLL_K32_MM_PURE_H
#define NTDLL_K32_MM_PURE_H

#include "nt_types.h"

/* Page protection values (flProtect low byte, Win32 = NT). */
#define K32M_PAGE_NOACCESS           0x01u
#define K32M_PAGE_READONLY           0x02u
#define K32M_PAGE_READWRITE          0x04u
#define K32M_PAGE_WRITECOPY          0x08u
#define K32M_PAGE_EXECUTE            0x10u
#define K32M_PAGE_EXECUTE_READ       0x20u
#define K32M_PAGE_EXECUTE_READWRITE  0x40u
#define K32M_PAGE_EXECUTE_WRITECOPY  0x80u

/* SEC_* allocation attributes (flProtect high bits). */
#define K32M_SEC_IMAGE      0x01000000u
#define K32M_SEC_RESERVE    0x04000000u
#define K32M_SEC_COMMIT     0x08000000u
#define K32M_SEC_NOCACHE    0x10000000u
#define K32M_SEC_WRITECOMBINE 0x40000000u
#define K32M_SEC_LARGE_PAGES  0x80000000u
#define K32M_SEC_ATTR_MASK  0xFFFFFF00u

/* MapViewOfFile dwDesiredAccess (FILE_MAP_*). */
#define K32M_FILE_MAP_COPY     0x0001u
#define K32M_FILE_MAP_WRITE    0x0002u
#define K32M_FILE_MAP_READ     0x0004u
#define K32M_FILE_MAP_EXECUTE  0x0020u

/* SECTION_ALL_ACCESS, the access CreateFileMapping asks NtCreateSection for. */
#define K32M_SECTION_ALL_ACCESS 0x000F001Fu

/* MapViewOfFile offsets are multiples of the allocation granularity. */
#define K32M_ALLOCATION_GRANULARITY 0x10000u

/* Win32 error codes used here. */
#define K32M_ERROR_INVALID_PARAMETER 87u
#define K32M_ERROR_MAPPED_ALIGNMENT  1132u

/* The page protection of a section: the base value only, and it must be one
 * that a section can have. PAGE_NOACCESS and the bare PAGE_EXECUTE are not
 * section protections. Returns 0 when the value is not one. */
static inline DWORD k32m_section_protect(DWORD fl_protect)
{
    DWORD base = fl_protect & 0xFFu;
    switch (base) {
    case K32M_PAGE_READONLY:
    case K32M_PAGE_READWRITE:
    case K32M_PAGE_WRITECOPY:
    case K32M_PAGE_EXECUTE_READ:
    case K32M_PAGE_EXECUTE_READWRITE:
    case K32M_PAGE_EXECUTE_WRITECOPY:
        return base;
    default:
        return 0;
    }
}

/* The allocation attributes: at most one of SEC_IMAGE, SEC_RESERVE and
 * SEC_COMMIT, plus the cache flags. SEC_IMAGE does not combine with
 * SEC_RESERVE or SEC_COMMIT. Returns 0 if the combination is invalid. Without
 * SEC_RESERVE or SEC_IMAGE the section is committed (SEC_COMMIT), as Win32
 * defaults it. */
static inline DWORD k32m_section_alloc_attrs(DWORD fl_protect)
{
    DWORD attrs = fl_protect & K32M_SEC_ATTR_MASK;
    DWORD kind = attrs & (K32M_SEC_IMAGE | K32M_SEC_RESERVE | K32M_SEC_COMMIT);
    DWORD cache = attrs & ~(K32M_SEC_IMAGE | K32M_SEC_RESERVE | K32M_SEC_COMMIT);
    if (kind & K32M_SEC_IMAGE) {
        if (kind & (K32M_SEC_RESERVE | K32M_SEC_COMMIT))
            return 0;
        return K32M_SEC_IMAGE | cache;
    }
    if (kind == (K32M_SEC_RESERVE | K32M_SEC_COMMIT))
        return 0;
    if (kind == K32M_SEC_RESERVE)
        return K32M_SEC_RESERVE | cache;
    return K32M_SEC_COMMIT | cache;
}

/* Full CreateFileMapping flProtect check. Returns 1 when valid. */
static inline int k32m_section_flags_ok(DWORD fl_protect)
{
    return k32m_section_protect(fl_protect) != 0 &&
           k32m_section_alloc_attrs(fl_protect) != 0;
}

/* The NT view protection for MapViewOfFile's dwDesiredAccess. A write access
 * is writable; COPY is copy-on-write; READ alone is read-only; EXECUTE adds
 * the execute right. Returns 0 when no access bit is set or a bit is unknown. */
static inline DWORD k32m_view_protect(DWORD access)
{
    const DWORD known = K32M_FILE_MAP_COPY | K32M_FILE_MAP_WRITE |
                        K32M_FILE_MAP_READ | K32M_FILE_MAP_EXECUTE;
    if (access & ~known)
        return 0;
    int exec = (access & K32M_FILE_MAP_EXECUTE) != 0;
    if (access & K32M_FILE_MAP_WRITE)
        return exec ? K32M_PAGE_EXECUTE_READWRITE : K32M_PAGE_READWRITE;
    if (access & K32M_FILE_MAP_COPY)
        return exec ? K32M_PAGE_EXECUTE_WRITECOPY : K32M_PAGE_WRITECOPY;
    if (access & K32M_FILE_MAP_READ)
        return exec ? K32M_PAGE_EXECUTE_READ : K32M_PAGE_READONLY;
    if (exec)
        return K32M_PAGE_EXECUTE_READ;
    return 0;
}

/* The file offset of a view. MapViewOfFile takes the high and low halves. */
static inline ULONGLONG k32m_offset(DWORD hi, DWORD lo)
{
    return ((ULONGLONG)hi << 32) | (ULONGLONG)lo;
}

/* Offsets must be a multiple of the allocation granularity. Returns 1 when
 * aligned. */
static inline int k32m_offset_aligned(DWORD hi, DWORD lo)
{
    (void)hi;
    return (lo & (K32M_ALLOCATION_GRANULARITY - 1u)) == 0;
}

/* An anonymous section (no file) needs a non-zero size. A file section with a
 * zero size takes the file's size; the caller then passes NULL to
 * NtCreateSection. Returns 1 when the size is accepted for the kind. */
static inline int k32m_size_ok(int anonymous, DWORD hi, DWORD lo)
{
    if (anonymous)
        return hi != 0 || lo != 0;
    return 1;
}

#endif /* NTDLL_K32_MM_PURE_H */
