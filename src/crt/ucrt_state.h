/*
 * ucrt_state.h - the runtime's per-process state that the library functions read and write: errno, the
 * invalid-parameter handler, the strtok and rand state. One copy per translation unit that includes it; the
 * DLL wrappers include everything from a single file, so there is one per DLL.
 */
#ifndef UCRT_STATE_H
#define UCRT_STATE_H

#include "ucrt_types.h"

typedef void (*ucrt_invalid_handler_fn)(void);

typedef struct ucrt_state {
    int errno_value;
    unsigned long doserrno_value;
    ucrt_invalid_handler_fn invalid_handler; /* called for an invalid parameter; NULL: only errno is set */
    unsigned long invalid_count;             /* how many invalid parameters were reported (for tests) */
    char *strtok_ctx;
    ucrt_wchar *wcstok_ctx;
    unsigned int rand_seed;
    int printf_exp_digits;                   /* minimum exponent digits in %e: 2 (UCRT) or 3 (the old msvcrt) */
    int printf_count_output;                 /* %n is honoured only after _set_printf_count_output(1) */
    int printf_iso_wide;                     /* wide printf: %s is narrow and %S wide (ISO), not the Microsoft reverse */
} ucrt_state;

static ucrt_state g_ucrt_state = {0, 0, 0, 0, 0, 0, 1, 2, 0, 0};

/* The DLL points these at its per-thread slots; without them errno and _doserrno are the process-wide fields above. */
static int *(*g_ucrt_errno_slot)(void);
static unsigned long *(*g_ucrt_doserrno_slot)(void);

static inline int *ucrt_errno_ptr(void)
{
    return g_ucrt_errno_slot ? g_ucrt_errno_slot() : &g_ucrt_state.errno_value;
}

static inline unsigned long *ucrt_doserrno_ptr(void)
{
    return g_ucrt_doserrno_slot ? g_ucrt_doserrno_slot() : &g_ucrt_state.doserrno_value;
}

static inline void ucrt_set_errno(int e)
{
    *ucrt_errno_ptr() = e;
}

static inline int ucrt_get_errno(void)
{
    return *ucrt_errno_ptr();
}

static inline void ucrt_set_doserrno(unsigned long e)
{
    *ucrt_doserrno_ptr() = e;
}

/* The C runtime's invalid parameter path: sets errno to e (EINVAL for a bad argument, ERANGE for a buffer
 * that is too small), calls the installed handler, and returns e so a function can `return ucrt_invalid(e)`.
 * The Windows default handler ends the process, which the DLL wrapper installs; with no handler the caller
 * just gets the error code. */
static inline int ucrt_invalid(int e)
{
    g_ucrt_state.invalid_count++;
    ucrt_set_errno(e);
    if (g_ucrt_state.invalid_handler)
        g_ucrt_state.invalid_handler();
    return e;
}

#endif /* UCRT_STATE_H */
