/*
 * kernel32_env.c - environment variables, the environment block and the
 * command line in ANSI form. The rules are in k32_env_pure.h.
 *
 * The process environment is the block at PEB->ProcessParameters->Environment,
 * with EnvironmentSize in bytes (written by pe_build_process_parameters).
 *
 * Policy (documented, and checked by tests/ntdll-host/test_k32_env_pure.c and
 * tests/kernel/s9):
 *  - SetEnvironmentVariable builds a new block on the heap and points
 *    Environment and EnvironmentSize at it, as RtlSetEnvironmentVariable does
 *    on Windows. The old block is not freed (the shim's heap frees nothing).
 *  - GetEnvironmentStringsW returns the live block, not a copy as on Windows.
 *    tests/kernel/s8 asserts that identity, so it is kept.
 *    FreeEnvironmentStringsW accepts only the live block (TRUE, no free); any
 *    other pointer fails with ERROR_INVALID_PARAMETER.
 *  - GetEnvironmentStringsA, GetCommandLineA and the A variants of the
 *    functions above return heap copies; GetCommandLineA caches its copy
 *    for the life of the process.
 *  - ANSI means CP_ACP, which is Windows-1252 in this shim (k32_str_pure.h,
 *    the same mapping as MultiByteToWideChar). A character with no 1252
 *    mapping becomes '?'. CP_ACP is not treated as UTF-8.
 *  - Size rules: a call with a NULL or too small buffer returns the size it
 *    needs and writes nothing. GetEnvironmentVariable returns the length
 *    without the NUL on success and the length with it when the buffer is
 *    too small; ExpandEnvironmentStrings returns the count with the NUL in
 *    both cases.
 *  - Thread safety: none. A Set and a Get on one block race.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "k32_env_pure.h"
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

/* Defined in kernel32.c. */
void SetLastError(DWORD e);
void *WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *mem);
HANDLE WINAPI GetProcessHeap(void);

/* The process parameters, or NULL before they exist. */
static RTL_USER_PROCESS_PARAMETERS *k32e_pp(void)
{
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    return peb ? peb->ProcessParameters : NULL;
}

/* The live block and its length in WCHARs (0 if unterminated or absent). */
static WCHAR *k32e_block(size_t *units)
{
    RTL_USER_PROCESS_PARAMETERS *pp = k32e_pp();
    WCHAR *blk = pp ? (WCHAR *)pp->Environment : NULL;
    *units = blk ? k32e_block_units(blk, K32E_MAX_UNITS) : 0;
    return blk;
}

static int k32e_has_eq(const WCHAR *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '=')
            return 1;
    }
    return 0;
}

/* Looks up name (nlen WCHARs, nlen > 0) in the live block. */
static DWORD k32e_lookup(const WCHAR *name, size_t nlen, const WCHAR **val, size_t *vlen)
{
    size_t units = 0;
    WCHAR *blk = k32e_block(&units);
    size_t voff = 0;
    if (nlen == 0)
        return K32E_ERROR_ENVVAR_NOT_FOUND;
    if (!k32e_find(blk, units, name, nlen, &voff, vlen))
        return K32E_ERROR_ENVVAR_NOT_FOUND;
    *val = blk + voff;
    return 0;
}

/* Narrow (CP_ACP) length of n WCHARs, not counting a NUL. */
static size_t k32e_narrow_need(const WCHAR *w, size_t n)
{
    size_t need = 0;
    int used = 0;
    (void)k32str_wc_to_mb(K32STR_CP_ACP, 0, w, n, NULL, 0, '?', &need, &used);
    return need;
}

/* Writes n WCHARs as narrow CP_ACP bytes and a NUL after them. cap must be
 * at least the narrow length plus one. */
static void k32e_narrow_write(const WCHAR *w, size_t n, char *buf, size_t cap)
{
    size_t need = 0;
    int used = 0;
    (void)k32str_wc_to_mb(K32STR_CP_ACP, 0, w, n, (unsigned char *)buf, cap, '?', &need, &used);
    buf[need] = 0;
}

/* Widens a NUL-terminated narrow string to a heap block. *len excludes the NUL. */
static int k32e_widen(const char *s, WCHAR **out, size_t *len)
{
    size_t n = strlen(s);
    size_t need = 0;
    int err = k32str_mb_to_wc(K32STR_CP_ACP, 0, (const unsigned char *)s, n, NULL, 0, &need);
    if (err)
        return K32E_ERROR_INVALID_PARAMETER;
    if (need >= K32E_MAX_UNITS)
        return K32E_ERROR_INVALID_PARAMETER;
    WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (need + 1) * sizeof(WCHAR));
    if (!w)
        return K32E_ERROR_NOT_ENOUGH_MEMORY;
    (void)k32str_mb_to_wc(K32STR_CP_ACP, 0, (const unsigned char *)s, n, w, need, &need);
    w[need] = 0;
    *out = w;
    *len = need;
    return 0;
}

/* ------------------------------------------------------------------------- */

DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size)
{
    if (!name || (!buf && size)) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t nlen = k32e_wlen(name, K32E_MAX_UNITS);
    if (nlen >= K32E_MAX_UNITS) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const WCHAR *val = NULL;
    size_t vlen = 0;
    DWORD err = k32e_lookup(name, nlen, &val, &vlen);
    if (err) {
        SetLastError(err);
        return 0;
    }
    if (size == 0 || vlen + 1 > size) {
        SetLastError(0);
        return (DWORD)(vlen + 1);
    }
    for (size_t k = 0; k < vlen; ++k)
        buf[k] = val[k];
    buf[vlen] = 0;
    SetLastError(0);
    return (DWORD)vlen;
}

DWORD WINAPI GetEnvironmentVariableA(const char *name, char *buf, DWORD size)
{
    if (!name || (!buf && size)) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return 0;
    }
    WCHAR *wn = NULL;
    size_t nlen = 0;
    int err = k32e_widen(name, &wn, &nlen);
    if (err) {
        SetLastError(err);
        return 0;
    }
    const WCHAR *val = NULL;
    size_t vlen = 0;
    err = k32e_lookup(wn, nlen, &val, &vlen);
    HeapFree(GetProcessHeap(), 0, wn);
    if (err) {
        SetLastError(err);
        return 0;
    }
    size_t need = k32e_narrow_need(val, vlen);
    if (size == 0 || need + 1 > size) {
        SetLastError(0);
        return (DWORD)(need + 1);
    }
    k32e_narrow_write(val, vlen, buf, size);
    SetLastError(0);
    return (DWORD)need;
}

BOOL WINAPI SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value)
{
    size_t nlen = name ? k32e_wlen(name, K32E_MAX_UNITS) : 0;
    if (!name || nlen == 0 || nlen >= K32E_MAX_UNITS || k32e_has_eq(name, nlen)) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    size_t vlen = 0;
    if (value) {
        vlen = k32e_wlen(value, K32E_MAX_UNITS);
        if (vlen >= K32E_MAX_UNITS) {
            SetLastError(K32E_ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    RTL_USER_PROCESS_PARAMETERS *pp = k32e_pp();
    if (!pp) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    size_t units = 0;
    WCHAR *blk = k32e_block(&units);
    size_t voff = 0, oldlen = 0;
    int exists = k32e_find(blk, units, name, nlen, &voff, &oldlen);
    if (!value && !exists) {
        SetLastError(K32E_ERROR_ENVVAR_NOT_FOUND);
        return FALSE;
    }
    int has_value = value != NULL;
    size_t need = 0;
    int err = k32e_set_build(NULL, 0, blk, units, name, nlen, value, vlen, has_value, &need);
    if (err) {
        SetLastError(err);
        return FALSE;
    }
    WCHAR *nb = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, need * sizeof(WCHAR));
    if (!nb) {
        SetLastError(K32E_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    (void)k32e_set_build(nb, need, blk, units, name, nlen, value, vlen, has_value, &need);
    pp->Environment = nb;
    pp->EnvironmentSize = (ULONG)(need * sizeof(WCHAR));
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetEnvironmentVariableA(const char *name, const char *value)
{
    if (!name) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    WCHAR *wn = NULL, *wv = NULL;
    size_t nl = 0, vl = 0;
    int err = k32e_widen(name, &wn, &nl);
    if (err) {
        SetLastError(err);
        return FALSE;
    }
    if (value) {
        err = k32e_widen(value, &wv, &vl);
        if (err) {
            HeapFree(GetProcessHeap(), 0, wn);
            SetLastError(err);
            return FALSE;
        }
    }
    BOOL ok = SetEnvironmentVariableW(wn, wv);
    HeapFree(GetProcessHeap(), 0, wn);
    if (wv)
        HeapFree(GetProcessHeap(), 0, wv);
    return ok;
}

DWORD WINAPI ExpandEnvironmentStringsW(const WCHAR *src, WCHAR *dst, DWORD size)
{
    if (!src || (!dst && size)) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t slen = k32e_wlen(src, K32E_MAX_UNITS);
    if (slen >= K32E_MAX_UNITS) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t units = 0;
    WCHAR *blk = k32e_block(&units);
    size_t need = 0;
    int err = k32e_expand(NULL, 0, src, slen, blk, units, &need);
    if (err) {
        SetLastError(err);
        return 0;
    }
    if (!dst || size == 0 || need > size) {
        SetLastError(0);
        return (DWORD)need;
    }
    (void)k32e_expand(dst, size, src, slen, blk, units, &need);
    SetLastError(0);
    return (DWORD)need;
}

DWORD WINAPI ExpandEnvironmentStringsA(const char *src, char *dst, DWORD size)
{
    if (!src || (!dst && size)) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return 0;
    }
    WCHAR *ws = NULL;
    size_t slen = 0;
    int err = k32e_widen(src, &ws, &slen);
    if (err) {
        SetLastError(err);
        return 0;
    }
    size_t units = 0;
    WCHAR *blk = k32e_block(&units);
    size_t wneed = 0;
    err = k32e_expand(NULL, 0, ws, slen, blk, units, &wneed);
    if (err) {
        HeapFree(GetProcessHeap(), 0, ws);
        SetLastError(err);
        return 0;
    }
    WCHAR *wx = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, wneed * sizeof(WCHAR));
    if (!wx) {
        HeapFree(GetProcessHeap(), 0, ws);
        SetLastError(K32E_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    (void)k32e_expand(wx, wneed, ws, slen, blk, units, &wneed);
    HeapFree(GetProcessHeap(), 0, ws);
    /* wneed counts the NUL, the narrow form is the same length minus that NUL. */
    size_t bneed = k32e_narrow_need(wx, wneed - 1);
    DWORD ret = 0;
    if (!dst || size == 0 || bneed + 1 > size) {
        ret = (DWORD)(bneed + 1);
    } else {
        k32e_narrow_write(wx, wneed - 1, dst, size);
        ret = (DWORD)(bneed + 1);
    }
    HeapFree(GetProcessHeap(), 0, wx);
    SetLastError(0);
    return ret;
}

/* The live block, as narrow CP_ACP. A heap copy: free it with FreeEnvironmentStringsA. */
char *WINAPI GetEnvironmentStringsA(void)
{
    size_t units = 0;
    WCHAR *blk = k32e_block(&units);
    if (!blk || units == 0) {
        SetLastError(K32E_ERROR_ENVVAR_NOT_FOUND);
        return NULL;
    }
    /* units includes the terminating NUL, which converts to a NUL byte. */
    size_t need = k32e_narrow_need(blk, units);
    char *p = (char *)HeapAlloc(GetProcessHeap(), 0, need + 1);
    if (!p) {
        SetLastError(K32E_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    k32e_narrow_write(blk, units, p, need + 1);
    SetLastError(0);
    return p;
}

BOOL WINAPI FreeEnvironmentStringsA(char *p)
{
    if (!p) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    HeapFree(GetProcessHeap(), 0, p);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI FreeEnvironmentStringsW(WCHAR *p)
{
    size_t units = 0;
    WCHAR *blk = k32e_block(&units);
    if (!p || p != blk) {
        SetLastError(K32E_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* The live block is not a copy, so there is nothing to free. */
    SetLastError(0);
    return TRUE;
}

/* The command line in narrow CP_ACP, converted once and kept for the process. */
char *WINAPI GetCommandLineA(void)
{
    static char *cached;
    if (cached)
        return cached;
    RTL_USER_PROCESS_PARAMETERS *pp = k32e_pp();
    if (!pp || !pp->CommandLine.Buffer)
        return NULL;
    const WCHAR *w = pp->CommandLine.Buffer;
    size_t n = pp->CommandLine.Length / sizeof(WCHAR);
    size_t need = k32e_narrow_need(w, n);
    char *p = (char *)HeapAlloc(GetProcessHeap(), 0, need + 1);
    if (!p)
        return NULL;
    k32e_narrow_write(w, n, p, need + 1);
    cached = p;
    return p;
}

#endif /* __MINGW32__ */
