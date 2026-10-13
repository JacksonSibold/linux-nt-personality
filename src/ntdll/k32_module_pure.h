/*
 * k32_module_pure.h - pure helpers for the kernel32 module APIs.
 *
 * LoadLibrary*, FreeLibrary, GetModuleHandle*, GetModuleFileName*,
 * GetProcAddress and DisableThreadLibraryCalls in kernel32.c call the loader
 * with values checked here. Nothing in this header touches the TEB, the PEB,
 * the loader or the NT syscalls, so tests/ntdll-host can compile it on the host
 * (test_k32_module_pure.c).
 *
 * Error values are Win32 error codes (the last-error value). A return of 0
 * from an error function means "no error" unless the function says otherwise.
 *
 * Decisions recorded here (see docs/modules/ntdll-win32.md):
 *   - ANSI text is converted one byte to one WCHAR (Latin-1). This is the
 *     ANSI code page for ASCII only. A full code-page conversion needs
 *     MultiByteToWideChar, which is not implemented.
 *   - Unmappable WCHAR values become '?' in ANSI output, as WC_DEFAULTCHAR does.
 *   - DONT_RESOLVE_DLL_REFERENCES is refused (ERROR_CALL_NOT_IMPLEMENTED): the
 *     loader always runs DllMain (decision 0031).
 */
#ifndef NTDLL_K32_MODULE_PURE_H
#define NTDLL_K32_MODULE_PURE_H

#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include <stdint.h>

/* Win32 error codes used by the module APIs. */
#define K32_ERROR_NOT_ENOUGH_MEMORY      8
#define K32_ERROR_INVALID_PARAMETER      87
#define K32_ERROR_CALL_NOT_IMPLEMENTED   120
#define K32_ERROR_INSUFFICIENT_BUFFER    122
#define K32_ERROR_MOD_NOT_FOUND          126
#define K32_ERROR_PROC_NOT_FOUND         127
#define K32_ERROR_BAD_EXE_FORMAT         193
#define K32_ERROR_FILENAME_EXCED_RANGE   206
#define K32_ERROR_DLL_INIT_FAILED        1114

/* GetModuleHandleEx flags. */
#define K32_GMH_FLAG_PIN                 0x00000001
#define K32_GMH_FLAG_UNCHANGED_REFCOUNT  0x00000002
#define K32_GMH_FLAG_FROM_ADDRESS        0x00000004

/* LoadLibraryEx flags that this layer understands. */
#define K32_LOAD_LIBRARY_DONT_RESOLVE_DLL_REFERENCES 0x00000001
#define K32_LOAD_LIBRARY_AS_DATAFILE                 0x00000002
#define K32_LOAD_LIBRARY_AS_IMAGE_RESOURCE           0x00000020
#define K32_LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE       0x00000040

/* Same value as LDR_DLL_LOAD_AS_DATAFILE in ldr.h. Kept here so the host test
 * does not need the PE parser header. */
#define K32_LDR_DLL_LOAD_AS_DATAFILE                 0x00000002

/* Same value as LDRP_DONT_CALL_FOR_THREADS, stored in LDR_DATA_TABLE_ENTRY.Flags. */
#define K32_LDRP_DONT_CALL_FOR_THREADS               0x00040000u

/* Largest module name, in characters, including the terminating NUL. */
#define K32_MODULE_NAME_MAX                          260

/* ------------------------------------------------------------------------ */
/* ANSI <-> wide                                                            */
/* ------------------------------------------------------------------------ */

/* Widens a NUL-terminated byte string. cap counts WCHAR slots including the
 * terminator. Returns the number of characters written (not counting the
 * NUL), or (size_t)-1 if in is NULL or the result does not fit. */
static inline size_t k32_wide_from_ansi(const char *in, WCHAR *out, size_t cap)
{
    if (!in || !out || cap == 0) return (size_t)-1;
    size_t i = 0;
    for (; in[i]; ++i) {
        if (i + 1 >= cap) return (size_t)-1;
        out[i] = (WCHAR)(unsigned char)in[i];
    }
    if (i + 1 > cap) return (size_t)-1;
    out[i] = 0;
    return i;
}

/* Narrows a NUL-terminated wide string. cap counts bytes including the
 * terminator. Characters above 0xFF become '?'. Returns the number of bytes
 * written (not counting the NUL), or (size_t)-1 if in is NULL or it does not fit. */
static inline size_t k32_ansi_from_wide(const WCHAR *in, char *out, size_t cap)
{
    if (!in || !out || cap == 0) return (size_t)-1;
    size_t i = 0;
    for (; in[i]; ++i) {
        if (i + 1 >= cap) return (size_t)-1;
        out[i] = in[i] <= 0xFF ? (char)(unsigned char)in[i] : '?';
    }
    if (i + 1 > cap) return (size_t)-1;
    out[i] = 0;
    return i;
}

/* ------------------------------------------------------------------------ */
/* GetModuleFileName                                                        */
/* ------------------------------------------------------------------------ */

/* Number of characters to copy from a name of n characters into a buffer of
 * cap slots. One slot is kept for the NUL. */
static inline size_t k32_filename_copy_len(size_t n, size_t cap)
{
    if (cap == 0) return 0;
    return n < cap ? n : cap - 1;
}

/* GetModuleFileName return value. When the name does not fit, the copy is
 * truncated, the return value is cap, and the error is ERROR_INSUFFICIENT_BUFFER
 * (MSDN). A zero-size buffer is the same case. The error is set only on the
 * truncated path; success leaves *err unchanged. */
static inline DWORD k32_filename_result(size_t n, DWORD cap, DWORD *err)
{
    if (cap == 0 || n >= cap) {
        if (err) *err = K32_ERROR_INSUFFICIENT_BUFFER;
        return cap;
    }
    return (DWORD)n;
}

/* ------------------------------------------------------------------------ */
/* GetModuleHandleEx                                                        */
/* ------------------------------------------------------------------------ */

/* Known flags only, and PIN cannot be combined with UNCHANGED_REFCOUNT. */
static inline int k32_module_flags_valid(DWORD flags)
{
    const DWORD known = K32_GMH_FLAG_PIN | K32_GMH_FLAG_UNCHANGED_REFCOUNT
                      | K32_GMH_FLAG_FROM_ADDRESS;
    if (flags & ~known) return 0;
    if ((flags & K32_GMH_FLAG_PIN) && (flags & K32_GMH_FLAG_UNCHANGED_REFCOUNT))
        return 0;
    return 1;
}

/* Argument check for GetModuleHandleEx. Returns 0 when the arguments are
 * acceptable, else the Win32 error. name is the module name (without
 * FROM_ADDRESS) or the address (with it). A NULL name without FROM_ADDRESS
 * means the main module. */
static inline DWORD k32_getmodulehandleex_check(DWORD flags, const void *name, void **out)
{
    if (!k32_module_flags_valid(flags)) return K32_ERROR_INVALID_PARAMETER;
    if (!out) return K32_ERROR_INVALID_PARAMETER;
    if ((flags & K32_GMH_FLAG_FROM_ADDRESS) && !name) return K32_ERROR_MOD_NOT_FOUND;
    return 0;
}

/* Address-in-module test for FROM_ADDRESS. Written so that base + size may
 * wrap without giving a false match. */
static inline int k32_addr_in_module(uintptr_t addr, uintptr_t base, ULONG size)
{
    if (!base) return 0;
    if (addr < base) return 0;
    return (uintptr_t)(addr - base) < (uintptr_t)size;
}

/* ------------------------------------------------------------------------ */
/* GetProcAddress                                                           */
/* ------------------------------------------------------------------------ */

/* lpProcName is a name pointer, or an ordinal when the value is at most
 * 0xFFFF (MAKEINTRESOURCE). Returns 0 when the arguments are acceptable. For
 * an ordinal, *by_ordinal is 1 and *ordinal holds the value. A NULL pointer is
 * an invalid argument; that also covers ordinal 0, which is a NULL pointer and
 * never names an export. */
static inline DWORD k32_getprocaddress_check(const void *proc, WORD *ordinal, int *by_ordinal)
{
    if (!proc) return K32_ERROR_INVALID_PARAMETER;
    uintptr_t v = (uintptr_t)proc;
    if (v > 0xFFFF) {
        if (by_ordinal) *by_ordinal = 0;
        return 0;
    }
    if (by_ordinal) *by_ordinal = 1;
    if (ordinal) *ordinal = (WORD)v;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Module names                                                             */
/* ------------------------------------------------------------------------ */

static inline int k32_is_sep(WCHAR c)
{
    return c == '\\' || c == '/';
}

/* Length of the directory prefix of name (up to and including the last
 * separator), or 0 when the name has none. */
static inline size_t k32_name_dir_len(const WCHAR *name, size_t len)
{
    size_t dir = 0;
    for (size_t i = 0; i < len; ++i)
        if (k32_is_sep(name[i])) dir = i + 1;
    return dir;
}

/* Normalises a module name for the loader. The directory part is dropped and
 * ".dll" is appended when the base name has no dot. A name that already has an
 * extension is kept. Returns the length written (not counting the NUL), or 0
 * if the input is empty, has no base name, or does not fit in cap slots. */
static inline size_t k32_module_name_normalise(const WCHAR *in, size_t len,
                                               WCHAR *out, size_t cap)
{
    if (!in || !out || cap == 0) return 0;
    size_t start = k32_name_dir_len(in, len);
    size_t base_len = len - start;
    if (base_len == 0) return 0;

    int has_dot = 0;
    for (size_t i = start; i < len; ++i)
        if (in[i] == '.') has_dot = 1;

    static const WCHAR dll[] = { '.', 'd', 'l', 'l' };
    size_t add = has_dot ? 0 : sizeof(dll) / sizeof(dll[0]);
    if (base_len + add + 1 > cap) return 0;

    for (size_t i = 0; i < base_len; ++i) out[i] = in[start + i];
    for (size_t i = 0; i < add; ++i) out[base_len + i] = dll[i];
    out[base_len + add] = 0;
    return base_len + add;
}

/* Argument check for LoadLibrary* on the name. len is the name length in
 * characters. Returns 0 when acceptable. */
static inline DWORD k32_loadlibrary_name_error(const void *name, size_t len)
{
    if (!name) return K32_ERROR_INVALID_PARAMETER;
    if (len == 0) return K32_ERROR_MOD_NOT_FOUND;
    if (len + 1 > K32_MODULE_NAME_MAX) return K32_ERROR_FILENAME_EXCED_RANGE;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* LoadLibraryEx flags                                                      */
/* ------------------------------------------------------------------------ */

/* Maps LoadLibraryEx flags to the loader flags. Returns 0 on success, else
 * the Win32 error: ERROR_CALL_NOT_IMPLEMENTED for DONT_RESOLVE_DLL_REFERENCES,
 * ERROR_INVALID_PARAMETER for a NULL out pointer or for bits above 0xFFFF,
 * which are not defined. The low 16 bits hold the defined flags and the search
 * flags, which are accepted and ignored. */
static inline DWORD k32_loadlibraryex_flags(DWORD flags, ULONG *ldr_flags)
{
    if (!ldr_flags) return K32_ERROR_INVALID_PARAMETER;
    if (flags & 0xFFFF0000u) return K32_ERROR_INVALID_PARAMETER;
    if (flags & K32_LOAD_LIBRARY_DONT_RESOLVE_DLL_REFERENCES) return K32_ERROR_CALL_NOT_IMPLEMENTED;
    *ldr_flags = 0;
    if (flags & (K32_LOAD_LIBRARY_AS_DATAFILE | K32_LOAD_LIBRARY_AS_IMAGE_RESOURCE
               | K32_LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))
        *ldr_flags = K32_LDR_DLL_LOAD_AS_DATAFILE;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* NTSTATUS to Win32 error for module calls                                 */
/* ------------------------------------------------------------------------ */

/* Module-specific mapping. The values match RtlNtStatusToDosError (the table in
 * ntstatus_to_dos.c), except STATUS_DLL_INIT_FAILED: that table has no entry for
 * it and falls back to 317, so this list gives ERROR_DLL_INIT_FAILED (1114),
 * the Win32 value (docs/review/ntdll.md, K32-18). Returns 0 for a status not in
 * this list, and the caller then uses RtlNtStatusToDosError. */
static inline DWORD k32_module_status_to_error(NTSTATUS s)
{
    switch ((ULONG)s) {
    case 0xC0000135u: return K32_ERROR_MOD_NOT_FOUND;        /* STATUS_DLL_NOT_FOUND */
    case 0xC0000139u: return K32_ERROR_PROC_NOT_FOUND;       /* STATUS_ENTRYPOINT_NOT_FOUND */
    case 0xC000007Au: return K32_ERROR_PROC_NOT_FOUND;       /* STATUS_PROCEDURE_NOT_FOUND */
    case 0xC000007Bu: return K32_ERROR_BAD_EXE_FORMAT;       /* STATUS_INVALID_IMAGE_FORMAT */
    case 0xC00001BAu: return K32_ERROR_DLL_INIT_FAILED;      /* STATUS_DLL_INIT_FAILED */
    case 0xC000000Du: return K32_ERROR_INVALID_PARAMETER;    /* STATUS_INVALID_PARAMETER */
    case 0xC0000017u: return K32_ERROR_NOT_ENOUGH_MEMORY;    /* STATUS_NO_MEMORY */
    default:          return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* DisableThreadLibraryCalls                                                */
/* ------------------------------------------------------------------------ */

/* Returns 0 when hModule is a module handle, else ERROR_INVALID_PARAMETER. */
static inline DWORD k32_disable_thread_calls_check(const void *module)
{
    return module ? 0 : K32_ERROR_INVALID_PARAMETER;
}

/* Module flags with the do-not-call-for-threads bit set. */
static inline ULONG k32_disable_thread_calls_flags(ULONG flags)
{
    return flags | K32_LDRP_DONT_CALL_FOR_THREADS;
}

#endif /* NTDLL_K32_MODULE_PURE_H */
