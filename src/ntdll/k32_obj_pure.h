/*
 * k32_obj_pure.h - pure rules for the named kernel object, wait, timer and completion port shims
 * (kernel32_obj.c): the object name a Win32 name becomes in the NT namespace, the wait results including the alertable
 * ones, the timer due time and period, and the parameter checks of CreateIoCompletionPort.
 * Host tests: tests/ntdll-host/test_k32_obj_pure.c.
 */
#ifndef NTDLL_K32_OBJ_PURE_H
#define NTDLL_K32_OBJ_PURE_H

#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include <stdint.h>

#define K32O_ERROR_PATH_NOT_FOUND 3u
#define K32O_ERROR_INVALID_PARAMETER 87u
#define K32O_ERROR_FILENAME_EXCED_RANGE 206u
#define K32O_ERROR_ALREADY_EXISTS 183u

#define K32O_WAIT_IO_COMPLETION 192u
#define K32O_WAIT_TIMEOUT 258u
#define K32O_WAIT_FAILED 0xFFFFFFFFu
#define K32O_INFINITE 0xFFFFFFFFu

#define K32O_STATUS_USER_APC ((NTSTATUS)0x000000C0L)
#define K32O_STATUS_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000L)
#define K32O_STATUS_ALERTED ((NTSTATUS)0x00000101L)

static const WCHAR k32o_ns_prefix[] = {'\\', 'B', 'a', 's', 'e', 'N', 'a', 'm', 'e', 'd', 'O', 'b', 'j', 'e', 'c', 't', 's', '\\'};
#define K32O_NS_PREFIX_LEN 18u

static inline WCHAR k32o_lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
}

/* Does name start with prefix (ASCII, case-insensitive) followed by a backslash? Returns the characters used. */
static inline size_t k32o_has_prefix(const WCHAR *name, size_t n, const char *prefix)
{
    size_t p = 0;
    while (prefix[p]) {
        if (p >= n || k32o_lower(name[p]) != (WCHAR)(unsigned char)prefix[p])
            return 0;
        ++p;
    }
    if (p < n && name[p] == '\\')
        return p + 1;
    return 0;
}

/* The NT name of a Win32 object name: "\BaseNamedObjects\" plus the name with its "Global\", "Local\" or "Session\N\" prefix
 * removed (one namespace serves them all here). A backslash left in the name is ERROR_PATH_NOT_FOUND, an empty name
 * invalid. Returns 0 or a Win32 error; *outn is the length written (no NUL). */
static inline DWORD k32o_object_name(const WCHAR *name, size_t n, WCHAR *out, size_t cap, size_t *outn)
{
    size_t skip = 0, i, o = 0;
    if (outn)
        *outn = 0;
    if (!name || n == 0)
        return K32O_ERROR_INVALID_PARAMETER;
    skip = k32o_has_prefix(name, n, "global");
    if (!skip)
        skip = k32o_has_prefix(name, n, "local");
    if (!skip) {
        size_t s = k32o_has_prefix(name, n, "session");
        if (s) {
            size_t k = s;
            while (k < n && name[k] >= '0' && name[k] <= '9')
                ++k;
            if (k > s && k < n && name[k] == '\\')
                skip = k + 1;
        }
    }
    if (skip >= n)
        return K32O_ERROR_INVALID_PARAMETER;
    if (K32O_NS_PREFIX_LEN + (n - skip) + 1 > cap || (K32O_NS_PREFIX_LEN + (n - skip)) * sizeof(WCHAR) > 0xFFFEu)
        return K32O_ERROR_FILENAME_EXCED_RANGE;
    for (i = 0; i < K32O_NS_PREFIX_LEN; ++i)
        out[o++] = k32o_ns_prefix[i];
    for (i = skip; i < n; ++i) {
        if (name[i] == '\\')
            return K32O_ERROR_PATH_NOT_FOUND;
        out[o++] = name[i];
    }
    out[o] = 0;
    if (outn)
        *outn = o;
    return 0;
}

/* The Win32 result of a wait: the NT wait status as WAIT_OBJECT_0 + n, WAIT_ABANDONED_0 + n, WAIT_TIMEOUT, or
 * WAIT_IO_COMPLETION when an alertable wait ran an APC; WAIT_FAILED for anything else. */
static inline DWORD k32o_wait_result(NTSTATUS s)
{
    if (s >= STATUS_WAIT_0 && s <= STATUS_WAIT_63)
        return (DWORD)(s - STATUS_WAIT_0);
    if (s >= STATUS_ABANDONED_WAIT_0 && s <= STATUS_ABANDONED_WAIT_63)
        return 0x80u + (DWORD)(s - STATUS_ABANDONED_WAIT_0);
    if (s == STATUS_TIMEOUT)
        return K32O_WAIT_TIMEOUT;
    if (s == K32O_STATUS_USER_APC || s == K32O_STATUS_ALERTED)
        return K32O_WAIT_IO_COMPLETION;
    return K32O_WAIT_FAILED;
}

/* A millisecond timeout as an NT relative delay (negative 100 ns units); NULL for INFINITE. */
static inline PLARGE_INTEGER k32o_timeout(DWORD ms, LARGE_INTEGER *t)
{
    if (ms == K32O_INFINITE)
        return NULL;
    t->QuadPart = -(LONGLONG)ms * 10000;
    return t;
}

/* SetWaitableTimer's due time: a negative value is relative, a positive one absolute, both in 100 ns; the period is in
 * milliseconds and must not be negative. Returns 0 or a Win32 error. */
static inline DWORD k32o_timer_args_check(const LARGE_INTEGER *due, LONG period)
{
    if (!due || period < 0)
        return K32O_ERROR_INVALID_PARAMETER;
    return 0;
}

/* CreateIoCompletionPort: with INVALID_HANDLE_VALUE as the file a new port is made (the existing port must then be
 * NULL); otherwise the file is attached to a port that must be given. 1 = create a port, 2 = associate, 0 = invalid. */
static inline int k32o_iocp_mode(HANDLE file, HANDLE existing)
{
    if (file == (HANDLE)(intptr_t)-1)
        return existing == NULL ? 1 : 0;
    if (!file)
        return 0;
    return existing != NULL ? 2 : 0;
}

#endif /* NTDLL_K32_OBJ_PURE_H */
