/*
 * msvcrt_policy.h - pure size arithmetic for the msvcrt shims.
 *
 * msvcrt.c calls these helpers so that tests/ntdll-host can check the
 * overflow rules without the MinGW heap or kernel32 imports.
 */
#ifndef NTDLL_MSVCRT_POLICY_H
#define NTDLL_MSVCRT_POLICY_H

#include "nt_types.h"

/* CRT-3: n * s for calloc. Returns 1 and stores the product when it fits in
 * SIZE_T, returns 0 and leaves *out unchanged when it wraps. */
static inline int ms_mul_size(SIZE_T n, SIZE_T s, SIZE_T *out)
{
    if (s != 0 && n > ((SIZE_T)-1) / s)
        return 0;
    *out = n * s;
    return 1;
}

/* CRT-1: sprintf and vsprintf have no size argument. They write at most
 * MS_SPRINTF_LIMIT bytes, the output plus its NUL, to the caller's buffer.
 * A caller whose buffer is smaller than MS_SPRINTF_LIMIT still overflows;
 * only removing the export fixes that (docs/review/ntdll.md, CRT-1). */
#define MS_SPRINTF_LIMIT ((SIZE_T)1024)

/* CRT-1: bytes sprintf copies for output of formatted_len characters. The
 * output is at most MS_SPRINTF_LIMIT - 1 characters, so the result never
 * exceeds MS_SPRINTF_LIMIT. */
static inline SIZE_T ms_sprintf_write_size(SIZE_T formatted_len)
{
    if (formatted_len >= MS_SPRINTF_LIMIT)
        return MS_SPRINTF_LIMIT;
    return formatted_len + 1;
}

#endif /* NTDLL_MSVCRT_POLICY_H */
