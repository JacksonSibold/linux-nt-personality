/*
 * kernel32_str.c - code page conversion: MultiByteToWideChar and
 * WideCharToMultiByte (K32-C1). The conversions and their error codes are in
 * k32_str_pure.h; this file reads the arguments and sets the last error.
 *
 * A length of -1 means the input is NUL terminated, and the NUL is converted
 * too, so the result counts it. A zero output size returns the needed count.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "k32_str_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef UINT
#define UINT unsigned int
#endif

/* Defined in kernel32.c. */
void SetLastError(DWORD e);

static size_t k32c_wlen(const WCHAR *s)
{
    size_t n = 0;
    while (s[n] != 0)
        ++n;
    return n;
}

int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, const char *mb, int cb, WCHAR *wc, int cc)
{
    if (!mb || cb == 0 || cb < -1 || cc < 0) {
        SetLastError(K32STR_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t n = (cb == -1) ? strlen(mb) + 1 : (size_t)cb;
    size_t need = 0;
    int err = k32str_mb_to_wc(cp, flags, (const unsigned char *)mb, n, wc, (size_t)cc, &need);
    if (err) {
        SetLastError(err);
        return 0;
    }
    SetLastError(0);
    return (int)need;
}

int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *wc, int cc, char *mb, int cb, const char *def, BOOL *used)
{
    if (!wc || cc == 0 || cc < -1 || cb < 0) {
        SetLastError(K32STR_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* UTF-8 has no default character, so lpDefaultChar and lpUsedDefaultChar
     * must be NULL for it. */
    if (k32str_resolve_cp(cp) == K32STR_CP_UTF8 && (def || used)) {
        SetLastError(K32STR_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t n = (cc == -1) ? k32c_wlen(wc) + 1 : (size_t)cc;
    unsigned char defch = def ? (unsigned char)def[0] : (unsigned char)'?';
    size_t need = 0;
    int u = 0;
    int err = k32str_wc_to_mb(cp, flags, wc, n, (unsigned char *)mb, (size_t)cb, defch, &need, &u);
    if (err) {
        SetLastError(err);
        return 0;
    }
    if (used)
        *used = u ? TRUE : FALSE;
    SetLastError(0);
    return (int)need;
}

#endif /* __MINGW32__ */
