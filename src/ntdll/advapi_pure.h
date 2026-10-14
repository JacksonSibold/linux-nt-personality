/*
 * advapi_pure.h - pure argument and result mapping for the advapi32 shim
 * (src/ntdll/advapi32.c). No NT types and no I/O, so tests/ntdll-host can
 * check the mapping without the MinGW build.
 *
 * Scope: the registry functions the differential probes import (Reg*A) and
 * the predefined root keys they use. Only HKEY_LOCAL_MACHINE and
 * HKEY_CURRENT_USER are mapped: the kernel registry has \Registry\Machine and
 * \Registry\User and no other root. Other predefined keys fail with
 * ERROR_INVALID_HANDLE rather than being guessed.
 */
#ifndef NTDLL_ADVAPI_PURE_H
#define NTDLL_ADVAPI_PURE_H

#include <stddef.h>

/* Win32 error codes used by the shim. */
#define ADV_ERROR_SUCCESS 0UL
#define ADV_ERROR_FILE_NOT_FOUND 2UL
#define ADV_ERROR_ACCESS_DENIED 5UL
#define ADV_ERROR_INVALID_HANDLE 6UL
#define ADV_ERROR_INVALID_PARAMETER 87UL
#define ADV_ERROR_CALL_NOT_IMPLEMENTED 120UL
#define ADV_ERROR_MORE_DATA 234UL

/* Predefined root key handles (winreg.h values). */
#define ADV_HKEY_CLASSES_ROOT ((unsigned long)0x80000000UL)
#define ADV_HKEY_CURRENT_USER ((unsigned long)0x80000001UL)
#define ADV_HKEY_LOCAL_MACHINE ((unsigned long)0x80000002UL)
#define ADV_HKEY_USERS ((unsigned long)0x80000003UL)

/* REG_CREATED_NEW_KEY / REG_OPENED_EXISTING_KEY disposition values. */
#define ADV_REG_CREATED_NEW_KEY 1UL
#define ADV_REG_OPENED_EXISTING_KEY 2UL

/* KEY_WOW64_64KEY and KEY_WOW64_32KEY. The kernel registry has one view, so
 * these flags are dropped before the NT open. */
#define ADV_KEY_WOW64_64KEY 0x0100UL
#define ADV_KEY_WOW64_32KEY 0x0200UL

/* Root path for a predefined key, or NULL when the shim does not map it. */
static inline const char *adv_predef_root_path(unsigned long hkey)
{
    switch (hkey) {
    case ADV_HKEY_LOCAL_MACHINE: return "\\Registry\\Machine";
    case ADV_HKEY_CURRENT_USER:  return "\\Registry\\User";
    default:                     return NULL;
    }
}

static inline int adv_is_predef_key(unsigned long hkey)
{
    return hkey >= ADV_HKEY_CLASSES_ROOT && hkey <= ADV_HKEY_USERS;
}

/* The access mask passed to NtOpenKey / NtCreateKey: the WOW64 view flags
 * removed, all other bits kept. */
static inline unsigned long adv_key_access(unsigned long samDesired)
{
    return samDesired & ~(ADV_KEY_WOW64_64KEY | ADV_KEY_WOW64_32KEY);
}

/* RegQueryValueExA sizing. `needed` is the byte length of the value's data.
 * With no data buffer the call succeeds and reports the length. With a buffer
 * smaller than `needed` it fails with ERROR_MORE_DATA and reports the length,
 * and no bytes are copied. Otherwise it succeeds. `*out_cb` is always set to
 * `needed`. */
static inline unsigned long adv_query_plan(unsigned long needed, int has_buffer,
                                           unsigned long cb_in, unsigned long *out_cb)
{
    *out_cb = needed;
    if (!has_buffer) return ADV_ERROR_SUCCESS;
    if (cb_in < needed) return ADV_ERROR_MORE_DATA;
    return ADV_ERROR_SUCCESS;
}

/* KEY_VALUE_PARTIAL_INFORMATION: TitleIndex (4), Type (4), DataLength (4),
 * then the data. The data starts at offset 12. Returns 1 when the data fits
 * in a record of `record_len` bytes, 0 otherwise. */
#define ADV_PARTIAL_DATA_OFFSET 12UL

static inline int adv_partial_data_fits(unsigned long record_len, unsigned long data_len)
{
    if (record_len < ADV_PARTIAL_DATA_OFFSET) return 0;
    return data_len <= record_len - ADV_PARTIAL_DATA_OFFSET;
}

/* Convert an ASCII name to UTF-16 into out[0..cap-1], NUL-terminated.
 * Returns the number of code units written (without the NUL), or -1 when the
 * name is longer than cap - 1 or holds a byte above 0x7F (the shim does not
 * convert code pages). */
static inline long adv_ascii_to_utf16(unsigned short *out, size_t cap, const char *in)
{
    size_t n = 0;
    if (cap == 0) return -1;
    if (!in) { out[0] = 0; return 0; }
    for (; in[n]; ++n) {
        if ((unsigned char)in[n] > 0x7F) return -1;
        if (n + 1 >= cap) return -1;
        out[n] = (unsigned short)(unsigned char)in[n];
    }
    out[n] = 0;
    return (long)n;
}

/* Byte length of a count of UTF-16 code units, as UNICODE_STRING.Length
 * requires. Returns 0 when it would exceed 0xFFFE (the USHORT limit). */
static inline unsigned long adv_utf16_bytes(unsigned long units)
{
    unsigned long bytes = units * 2UL;
    return bytes > 0xFFFEUL ? 0UL : bytes;
}

#endif /* NTDLL_ADVAPI_PURE_H */
