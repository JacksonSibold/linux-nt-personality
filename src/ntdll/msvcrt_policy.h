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

/* Startup argv (__getmainargs). Splits a narrow command line into argv by the
 * Windows rules: spaces and tabs separate arguments outside quotes; a quote
 * toggles quoting and is not copied; 2n backslashes before a quote give n
 * backslashes and a quote toggle; 2n+1 give n backslashes and a literal quote;
 * other backslashes are literal. Each argument is a NUL-terminated copy in buf,
 * and argv[argc] is NULL. Returns argc (0 for an empty line), or -1 if the
 * tokens do not fit in buf (cap bytes) or in argv (max_args entries, including
 * the NULL). */
static inline int ms_split_cmdline(const char *cmd, char *buf, SIZE_T cap, char **argv, int max_args)
{
    SIZE_T bo = 0, i = 0;
    int argc = 0;
    for (;;) {
        while (cmd[i] == ' ' || cmd[i] == '\t')
            i++;
        if (cmd[i] == '\0')
            break;
        if (argc + 1 >= max_args)
            return -1;
        argv[argc++] = buf + bo;
        int inq = 0;
        for (;;) {
            SIZE_T nb = 0;
            while (cmd[i] == '\\') {
                nb++;
                i++;
            }
            if (cmd[i] == '"') {
                for (SIZE_T k = 0; k < nb / 2; k++) {
                    if (bo + 1 > cap) return -1;
                    buf[bo++] = '\\';
                }
                if (nb % 2 == 0) {
                    inq = !inq;
                } else {
                    if (bo + 1 > cap) return -1;
                    buf[bo++] = '"';
                }
                i++;
                continue;
            }
            for (SIZE_T k = 0; k < nb; k++) {
                if (bo + 1 > cap) return -1;
                buf[bo++] = '\\';
            }
            if (cmd[i] == '\0' || (!inq && (cmd[i] == ' ' || cmd[i] == '\t')))
                break;
            if (bo + 1 > cap) return -1;
            buf[bo++] = cmd[i++];
        }
        if (bo + 1 > cap) return -1;
        buf[bo++] = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

#endif /* NTDLL_MSVCRT_POLICY_H */
