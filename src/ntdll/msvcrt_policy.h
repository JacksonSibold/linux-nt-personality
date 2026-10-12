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

#endif /* NTDLL_MSVCRT_POLICY_H */
