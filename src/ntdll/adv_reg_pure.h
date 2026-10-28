/*
 * adv_reg_pure.h - decisions of the registry functions of advapi32.dll that need no system call: which predefined key maps to
 * which path, which value types a RegGetValue call accepts, how string data is terminated, and when a name or data buffer is
 * too small.
 *
 * The full functions are in advapi32_reg.c. Host tests: tests/ntdll-host/test_adv_reg_pure.c.
 */
#ifndef ADV_REG_PURE_H
#define ADV_REG_PURE_H

#include <stddef.h>
#include <stdint.h>

#define ADV_R_SUCCESS 0u
#define ADV_R_FILE_NOT_FOUND 2u
#define ADV_R_ACCESS_DENIED 5u
#define ADV_R_INVALID_HANDLE 6u
#define ADV_R_INVALID_PARAMETER 87u
#define ADV_R_MORE_DATA 234u
#define ADV_R_NO_MORE_ITEMS 259u
#define ADV_R_BAD_PATHNAME 161u
#define ADV_R_UNSUPPORTED_TYPE 1783u

/* Value types */
#define ADV_REG_NONE 0u
#define ADV_REG_SZ 1u
#define ADV_REG_EXPAND_SZ 2u
#define ADV_REG_BINARY 3u
#define ADV_REG_DWORD 4u
#define ADV_REG_MULTI_SZ 7u
#define ADV_REG_QWORD 11u

/* RegGetValue flags */
#define ADV_RRF_RT_REG_NONE 0x00000001u
#define ADV_RRF_RT_REG_SZ 0x00000002u
#define ADV_RRF_RT_REG_EXPAND_SZ 0x00000004u
#define ADV_RRF_RT_REG_BINARY 0x00000008u
#define ADV_RRF_RT_REG_DWORD 0x00000010u
#define ADV_RRF_RT_REG_MULTI_SZ 0x00000020u
#define ADV_RRF_RT_REG_QWORD 0x00000040u
#define ADV_RRF_NOEXPAND 0x10000000u
#define ADV_RRF_ZEROONFAILURE 0x20000000u

/* ---- Predefined keys ---------------------------------------------------------------------------------------------------------- */

/* On x86-64 the predefined handles are the 32-bit values sign-extended; callers that went through a DWORD have them zero-extended.
 * Both are taken. Returns the low 32 bits, or 0 when the handle is not one. */
static inline uint32_t adv_predef_id(uint64_t h)
{
    if ((h >> 32) == 0xFFFFFFFFull || (h >> 32) == 0)
        if ((uint32_t)h >= 0x80000000u && (uint32_t)h <= 0x800000FFu)
            return (uint32_t)h;
    return 0;
}

#define ADV_ID_CLASSES_ROOT 0x80000000u
#define ADV_ID_CURRENT_USER 0x80000001u
#define ADV_ID_LOCAL_MACHINE 0x80000002u
#define ADV_ID_USERS 0x80000003u
#define ADV_ID_PERFORMANCE_DATA 0x80000004u
#define ADV_ID_CURRENT_CONFIG 0x80000005u

/* The NT path a predefined key stands for, or NULL when there is none. The kernel registry has \Registry\Machine and
 * \Registry\User; the current user's keys are taken directly under the latter. */
static inline const char *adv_predef_path(uint32_t id)
{
    switch (id) {
    case ADV_ID_CLASSES_ROOT: return "\\Registry\\Machine\\Software\\Classes";
    case ADV_ID_CURRENT_USER: return "\\Registry\\User";
    case ADV_ID_LOCAL_MACHINE: return "\\Registry\\Machine";
    case ADV_ID_USERS: return "\\Registry\\User";
    case ADV_ID_CURRENT_CONFIG: return "\\Registry\\Machine\\System\\CurrentControlSet\\Hardware Profiles\\Current";
    default: return NULL;
    }
}

/* A subkey name may not start with a backslash. */
static inline uint32_t adv_check_subkey(const uint16_t *sub)
{
    if (sub && sub[0] == '\\')
        return ADV_R_BAD_PATHNAME;
    return ADV_R_SUCCESS;
}

/* ---- RegGetValue ------------------------------------------------------------------------------------------------------------ */

/* Whether a value of `type` passes the type restrictions in `flags`. A REG_EXPAND_SZ passes a REG_SZ restriction when it will be
 * expanded. */
static inline int adv_rrf_allows(uint32_t type, uint32_t flags)
{
    uint32_t bit;
    switch (type) {
    case ADV_REG_NONE: bit = ADV_RRF_RT_REG_NONE; break;
    case ADV_REG_SZ: bit = ADV_RRF_RT_REG_SZ; break;
    case ADV_REG_EXPAND_SZ: bit = ADV_RRF_RT_REG_EXPAND_SZ; break;
    case ADV_REG_BINARY: bit = ADV_RRF_RT_REG_BINARY; break;
    case ADV_REG_DWORD: bit = ADV_RRF_RT_REG_DWORD; break;
    case ADV_REG_MULTI_SZ: bit = ADV_RRF_RT_REG_MULTI_SZ; break;
    case ADV_REG_QWORD: bit = ADV_RRF_RT_REG_QWORD; break;
    default: bit = 0; break;
    }
    if (flags & bit)
        return 1;
    if (type == ADV_REG_EXPAND_SZ && (flags & ADV_RRF_RT_REG_SZ) && !(flags & ADV_RRF_NOEXPAND))
        return 1;
    return 0;
}

static inline int adv_is_string_type(uint32_t type) { return type == ADV_REG_SZ || type == ADV_REG_EXPAND_SZ || type == ADV_REG_MULTI_SZ; }

/* How many bytes of terminator a string value of `len` bytes needs so that it ends in a NUL (two for a multi-string): the data
 * of a value in the registry need not be terminated. */
static inline size_t adv_terminator_bytes(uint32_t type, const uint8_t *data, size_t len)
{
    size_t have = 0;
    size_t want = type == ADV_REG_MULTI_SZ ? 4 : 2;
    /* count the zero bytes at the end, in whole characters */
    while (have + 2 <= len && data[len - have - 1] == 0 && data[len - have - 2] == 0 && have < want)
        have += 2;
    return want - have;
}

/* ---- Enumeration ------------------------------------------------------------------------------------------------------------ */

/* A name of `chars` characters fits a buffer of `cap` characters if there is room for the NUL as well. */
static inline int adv_name_fits(size_t chars, size_t cap) { return chars < cap; }

/* ---- Data in the system code page ------------------------------------------------------------------------------------------- */

/* The functions with an "A" suffix keep strings in the system code page, the registry in UTF-16. Types whose data is text. */
static inline int adv_value_is_text(uint32_t type) { return adv_is_string_type(type); }

#endif
