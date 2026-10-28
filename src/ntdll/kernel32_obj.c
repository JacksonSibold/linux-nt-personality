/*
 * kernel32_obj.c - kernel objects with names, waits and completion ports: CreateEvent/Mutex/Semaphore/WaitableTimer
 * (and their Ex forms), the Open* functions, PulseEvent, the Ex waits that can run APCs, SleepEx, SignalObjectAndWait,
 * QueueUserAPC, SetWaitableTimer, and the I/O completion port functions.
 *
 * Names go into the \BaseNamedObjects directory of the NT object namespace (the kernel keeps one for all sessions);
 * "Global\", "Local\" and "Session\N\" prefixes are accepted and mean the same thing here. The rules are in
 * k32_obj_pure.h.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_obj_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef PVOID LPVOID;
typedef ULONG_PTR *PULONG_PTR;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define KO_OBJ_CASE_INSENSITIVE 0x40u
#define KO_OBJ_OPENIF 0x80u
#define KO_OBJ_INHERIT 0x02u
#define KO_EVENT_ALL_ACCESS 0x001F0003u
#define KO_MUTANT_ALL_ACCESS 0x001F0001u
#define KO_SEMAPHORE_ALL_ACCESS 0x001F0003u
#define KO_TIMER_ALL_ACCESS 0x001F0003u
#define KO_IO_COMPLETION_ALL_ACCESS 0x001F0003u
#define KO_SYNCHRONIZE 0x00100000u
#define KO_CREATE_EVENT_MANUAL_RESET 0x1u
#define KO_CREATE_EVENT_INITIAL_SET 0x2u
#define KO_CREATE_MUTEX_INITIAL_OWNER 0x1u
#define KO_CREATE_WAITABLE_TIMER_MANUAL_RESET 0x1u
#define KO_INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define KO_FILE_COMPLETION_INFORMATION 30u
#define KO_ERROR_FILE_NOT_FOUND 2u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtOpenEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
__declspec(dllimport) extern NTSTATUS NtCreateMutant(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtOpenMutant(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
__declspec(dllimport) extern NTSTATUS NtCreateSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG, LONG);
__declspec(dllimport) extern NTSTATUS NtOpenSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
__declspec(dllimport) extern NTSTATUS NtCreateTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
__declspec(dllimport) extern NTSTATUS NtOpenTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
__declspec(dllimport) extern NTSTATUS NtSetTimer(HANDLE, PLARGE_INTEGER, PVOID, PVOID, BOOLEAN, LONG, PBOOLEAN);
__declspec(dllimport) extern NTSTATUS NtCancelTimer(HANDLE, PBOOLEAN);
__declspec(dllimport) extern NTSTATUS NtPulseEvent(HANDLE, PLONG);
__declspec(dllimport) extern NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtWaitForMultipleObjects(ULONG, PHANDLE, ULONG, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtSignalAndWaitForSingleObject(HANDLE, HANDLE, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtDelayExecution(BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtQueueApcThread(HANDLE, PVOID, PVOID, PVOID, PVOID);
__declspec(dllimport) extern NTSTATUS NtTestAlert(void);
__declspec(dllimport) extern NTSTATUS NtCreateIoCompletion(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
__declspec(dllimport) extern NTSTATUS NtSetIoCompletion(HANDLE, ULONG_PTR, PVOID, NTSTATUS, ULONG_PTR);
__declspec(dllimport) extern NTSTATUS NtRemoveIoCompletion(HANDLE, PULONG_PTR, PVOID *, PIO_STATUS_BLOCK, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtRemoveIoCompletionEx(HANDLE, PVOID, ULONG, PULONG, PLARGE_INTEGER, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);

/* Defined in kernel32.c. */
void SetLastError(DWORD e);
DWORD GetLastError(void);

static void ko_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

/* The attributes of a named (or unnamed) object. name may be NULL. The name buffer is the caller's. */
typedef struct {
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    WCHAR buf[K32O_NS_PREFIX_LEN + 260 + 8];
    int named;
} KO_ATTR;

static DWORD ko_attr(KO_ATTR *a, const WCHAR *name, int inherit)
{
    size_t n = 0, outn = 0;
    DWORD err;
    memset(a, 0, sizeof(*a));
    a->oa.Length = sizeof(a->oa);
    a->oa.Attributes = inherit ? KO_OBJ_INHERIT : 0;
    if (!name)
        return 0;
    while (name[n] && n < 400)
        ++n;
    if (n >= 400)
        return K32O_ERROR_FILENAME_EXCED_RANGE;
    err = k32o_object_name(name, n, a->buf, sizeof(a->buf) / sizeof(a->buf[0]), &outn);
    if (err)
        return err;
    a->us.Buffer = a->buf;
    a->us.Length = (USHORT)(outn * sizeof(WCHAR));
    a->us.MaximumLength = (USHORT)(a->us.Length + sizeof(WCHAR));
    a->oa.ObjectName = &a->us;
    a->oa.Attributes |= KO_OBJ_CASE_INSENSITIVE | KO_OBJ_OPENIF;
    a->named = 1;
    return 0;
}

/* Widens an ANSI name into buf (260 characters at most). Returns 0 or an error. */
static DWORD ko_widen(const char *name, WCHAR *buf, size_t cap)
{
    size_t i = 0;
    for (; name[i]; ++i) {
        if (i + 1 >= cap)
            return K32O_ERROR_FILENAME_EXCED_RANGE;
        buf[i] = (WCHAR)(unsigned char)name[i];
    }
    buf[i] = 0;
    return 0;
}

/* The handle a create produced: success keeps the last error, except that opening an existing named object reports
 * ERROR_ALREADY_EXISTS. */
static HANDLE ko_create_result(NTSTATUS s, HANDLE h)
{
    if (!NT_SUCCESS(s) || !h) {
        ko_status(s);
        return NULL;
    }
    if (s == K32O_STATUS_OBJECT_NAME_EXISTS)
        SetLastError(K32O_ERROR_ALREADY_EXISTS);
    return h;
}

static HANDLE ko_open_result(NTSTATUS s, HANDLE h)
{
    if (!NT_SUCCESS(s) || !h) {
        ko_status(s);
        return NULL;
    }
    return h;
}

static int ko_inherit(void *sa)
{
    /* SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } */
    return sa ? ((DWORD *)sa)[4] != 0 : 0;
}

/* ---- Events -------------------------------------------------------------------------------------- */

HANDLE WINAPI CreateEventExW(void *sa, const WCHAR *name, DWORD flags, DWORD access)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err = ko_attr(&a, name, ko_inherit(sa));
    NTSTATUS s;
    if (err) {
        SetLastError(err);
        return NULL;
    }
    (void)access;
    s = NtCreateEvent(&h, KO_EVENT_ALL_ACCESS, a.named || a.oa.Attributes ? &a.oa : NULL,
                      (flags & KO_CREATE_EVENT_MANUAL_RESET) ? 0u : 1u, (flags & KO_CREATE_EVENT_INITIAL_SET) ? 1 : 0);
    return ko_create_result(s, h);
}

HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name)
{
    return CreateEventExW(sa, name, (manual ? KO_CREATE_EVENT_MANUAL_RESET : 0) | (initial ? KO_CREATE_EVENT_INITIAL_SET : 0), 0x1F0003u);
}

HANDLE WINAPI CreateEventExA(void *sa, const char *name, DWORD flags, DWORD access)
{
    WCHAR w[260];
    if (name) {
        DWORD err = ko_widen(name, w, 260);
        if (err) {
            SetLastError(err);
            return NULL;
        }
    }
    return CreateEventExW(sa, name ? w : NULL, flags, access);
}

HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name)
{
    return CreateEventExA(sa, name, (manual ? KO_CREATE_EVENT_MANUAL_RESET : 0) | (initial ? KO_CREATE_EVENT_INITIAL_SET : 0), 0x1F0003u);
}

HANDLE WINAPI OpenEventW(DWORD access, BOOL inherit, const WCHAR *name)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_attr(&a, name, inherit);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    a.oa.Attributes &= ~KO_OBJ_OPENIF;
    return ko_open_result(NtOpenEvent(&h, access, &a.oa), h);
}

HANDLE WINAPI OpenEventA(DWORD access, BOOL inherit, const char *name)
{
    WCHAR w[260];
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_widen(name, w, 260);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    return OpenEventW(access, inherit, w);
}

BOOL WINAPI PulseEvent(HANDLE h)
{
    NTSTATUS s = NtPulseEvent(h, NULL);
    ko_status(s);
    return NT_SUCCESS(s);
}

/* ---- Mutexes and semaphores ---------------------------------------------------------------------------- */

HANDLE WINAPI CreateMutexExW(void *sa, const WCHAR *name, DWORD flags, DWORD access)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err = ko_attr(&a, name, ko_inherit(sa));
    NTSTATUS s;
    (void)access;
    if (err) {
        SetLastError(err);
        return NULL;
    }
    s = NtCreateMutant(&h, KO_MUTANT_ALL_ACCESS, a.named || a.oa.Attributes ? &a.oa : NULL, (flags & KO_CREATE_MUTEX_INITIAL_OWNER) ? 1 : 0);
    return ko_create_result(s, h);
}

HANDLE WINAPI CreateMutexW(void *sa, BOOL owner, const WCHAR *name)
{
    return CreateMutexExW(sa, name, owner ? KO_CREATE_MUTEX_INITIAL_OWNER : 0, 0x1F0001u);
}

HANDLE WINAPI CreateMutexExA(void *sa, const char *name, DWORD flags, DWORD access)
{
    WCHAR w[260];
    if (name) {
        DWORD err = ko_widen(name, w, 260);
        if (err) {
            SetLastError(err);
            return NULL;
        }
    }
    return CreateMutexExW(sa, name ? w : NULL, flags, access);
}

HANDLE WINAPI CreateMutexA(void *sa, BOOL owner, const char *name)
{
    return CreateMutexExA(sa, name, owner ? KO_CREATE_MUTEX_INITIAL_OWNER : 0, 0x1F0001u);
}

HANDLE WINAPI OpenMutexW(DWORD access, BOOL inherit, const WCHAR *name)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_attr(&a, name, inherit);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    a.oa.Attributes &= ~KO_OBJ_OPENIF;
    return ko_open_result(NtOpenMutant(&h, access, &a.oa), h);
}

HANDLE WINAPI OpenMutexA(DWORD access, BOOL inherit, const char *name)
{
    WCHAR w[260];
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_widen(name, w, 260);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    return OpenMutexW(access, inherit, w);
}

HANDLE WINAPI CreateSemaphoreExW(void *sa, LONG initial, LONG maximum, const WCHAR *name, DWORD flags, DWORD access)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err;
    NTSTATUS s;
    (void)flags;
    (void)access;
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_attr(&a, name, ko_inherit(sa));
    if (err) {
        SetLastError(err);
        return NULL;
    }
    s = NtCreateSemaphore(&h, KO_SEMAPHORE_ALL_ACCESS, a.named || a.oa.Attributes ? &a.oa : NULL, initial, maximum);
    return ko_create_result(s, h);
}

HANDLE WINAPI CreateSemaphoreW(void *sa, LONG initial, LONG maximum, const WCHAR *name)
{
    return CreateSemaphoreExW(sa, initial, maximum, name, 0, 0x1F0003u);
}

HANDLE WINAPI CreateSemaphoreA(void *sa, LONG initial, LONG maximum, const char *name)
{
    WCHAR w[260];
    if (name) {
        DWORD err = ko_widen(name, w, 260);
        if (err) {
            SetLastError(err);
            return NULL;
        }
    }
    return CreateSemaphoreExW(sa, initial, maximum, name ? w : NULL, 0, 0x1F0003u);
}

HANDLE WINAPI OpenSemaphoreW(DWORD access, BOOL inherit, const WCHAR *name)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_attr(&a, name, inherit);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    a.oa.Attributes &= ~KO_OBJ_OPENIF;
    return ko_open_result(NtOpenSemaphore(&h, access, &a.oa), h);
}

HANDLE WINAPI OpenSemaphoreA(DWORD access, BOOL inherit, const char *name)
{
    WCHAR w[260];
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_widen(name, w, 260);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    return OpenSemaphoreW(access, inherit, w);
}

/* ---- Waitable timers ------------------------------------------------------------------------------------ */

HANDLE WINAPI CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD access)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err = ko_attr(&a, name, ko_inherit(sa));
    NTSTATUS s;
    (void)access;
    if (err) {
        SetLastError(err);
        return NULL;
    }
    /* NotificationTimer (manual reset) is 0, SynchronizationTimer 1 */
    s = NtCreateTimer(&h, KO_TIMER_ALL_ACCESS, a.named || a.oa.Attributes ? &a.oa : NULL, (flags & KO_CREATE_WAITABLE_TIMER_MANUAL_RESET) ? 0u : 1u);
    return ko_create_result(s, h);
}

HANDLE WINAPI CreateWaitableTimerW(void *sa, BOOL manual, const WCHAR *name)
{
    return CreateWaitableTimerExW(sa, name, manual ? KO_CREATE_WAITABLE_TIMER_MANUAL_RESET : 0, 0x1F0003u);
}

HANDLE WINAPI CreateWaitableTimerA(void *sa, BOOL manual, const char *name)
{
    WCHAR w[260];
    if (name) {
        DWORD err = ko_widen(name, w, 260);
        if (err) {
            SetLastError(err);
            return NULL;
        }
    }
    return CreateWaitableTimerExW(sa, name ? w : NULL, manual ? KO_CREATE_WAITABLE_TIMER_MANUAL_RESET : 0, 0x1F0003u);
}

HANDLE WINAPI OpenWaitableTimerW(DWORD access, BOOL inherit, const WCHAR *name)
{
    KO_ATTR a;
    HANDLE h = NULL;
    DWORD err;
    if (!name) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    err = ko_attr(&a, name, inherit);
    if (err) {
        SetLastError(err);
        return NULL;
    }
    a.oa.Attributes &= ~KO_OBJ_OPENIF;
    return ko_open_result(NtOpenTimer(&h, access, &a.oa), h);
}

BOOL WINAPI SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *completion, void *arg, BOOL resume)
{
    LARGE_INTEGER t;
    DWORD err = k32o_timer_args_check(due, period);
    NTSTATUS s;
    if (err) {
        SetLastError(err);
        return FALSE;
    }
    t = *due;
    s = NtSetTimer(h, &t, completion, arg, resume ? TRUE : FALSE, period, NULL);
    ko_status(s);
    return NT_SUCCESS(s);
}

BOOL WINAPI CancelWaitableTimer(HANDLE h)
{
    NTSTATUS s = NtCancelTimer(h, NULL);
    ko_status(s);
    return NT_SUCCESS(s);
}

/* ---- Waits that can run APCs ------------------------------------------------------------------------------- */

DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER t;
    NTSTATUS s = NtWaitForSingleObject(h, alertable ? TRUE : FALSE, k32o_timeout(ms, &t));
    DWORD r = k32o_wait_result(s);
    if (r == K32O_WAIT_FAILED) {
        ko_status(s);
        return r;
    }
    SetLastError(0);
    return r;
}

DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL wait_all, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER t;
    NTSTATUS s;
    DWORD r;
    if (n == 0 || n > 64 || !hs) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return K32O_WAIT_FAILED;
    }
    for (DWORD i = 0; i < n; ++i) {
        if (!hs[i] || hs[i] == KO_INVALID_HANDLE) {
            SetLastError(6u); /* ERROR_INVALID_HANDLE */
            return K32O_WAIT_FAILED;
        }
    }
    s = NtWaitForMultipleObjects(n, (PHANDLE)hs, wait_all ? 0u : 1u, alertable ? TRUE : FALSE, k32o_timeout(ms, &t));
    r = k32o_wait_result(s);
    if (r == K32O_WAIT_FAILED) {
        ko_status(s);
        return r;
    }
    SetLastError(0);
    return r;
}

DWORD WINAPI SleepEx(DWORD ms, BOOL alertable)
{
    LARGE_INTEGER t;
    NTSTATUS s;
    if (ms == K32O_INFINITE) {
        s = NtDelayExecution(alertable ? TRUE : FALSE, NULL); /* a NULL interval is an infinite delay */
    } else {
        t.QuadPart = -(LONGLONG)ms * 10000;
        s = NtDelayExecution(alertable ? TRUE : FALSE, &t);
    }
    return (s == K32O_STATUS_USER_APC || s == K32O_STATUS_ALERTED) ? K32O_WAIT_IO_COMPLETION : 0;
}

DWORD WINAPI SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER t;
    NTSTATUS s = NtSignalAndWaitForSingleObject(signal, wait, alertable ? TRUE : FALSE, k32o_timeout(ms, &t));
    DWORD r = k32o_wait_result(s);
    if (r == K32O_WAIT_FAILED) {
        ko_status(s);
        return r;
    }
    SetLastError(0);
    return r;
}

/* The APC routine of a Win32 user APC takes one argument. NtQueueApcThread passes three; the first is the function. */
static void NTAPI ko_apc_thunk(PVOID fn, PVOID data, PVOID unused)
{
    (void)unused;
    ((void(WINAPI *)(ULONG_PTR))fn)((ULONG_PTR)data);
}

DWORD WINAPI QueueUserAPC(void *fn, HANDLE thread, ULONG_PTR data)
{
    NTSTATUS s;
    if (!fn) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = NtQueueApcThread(thread, (PVOID)ko_apc_thunk, fn, (PVOID)data, NULL);
    ko_status(s);
    return NT_SUCCESS(s) ? 1u : 0u;
}

/* ---- I/O completion ports ---------------------------------------------------------------------------------- */

HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads)
{
    int mode = k32o_iocp_mode(file, existing);
    NTSTATUS s;
    if (mode == 0) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (mode == 1) {
        HANDLE h = NULL;
        s = NtCreateIoCompletion(&h, KO_IO_COMPLETION_ALL_ACCESS, NULL, threads);
        if (!NT_SUCCESS(s)) {
            ko_status(s);
            return NULL;
        }
        return h;
    }
    {
        struct {
            HANDLE Port;
            PVOID Key;
        } ci;
        IO_STATUS_BLOCK iosb;
        ci.Port = existing;
        ci.Key = (PVOID)key;
        s = NtSetInformationFile(file, &iosb, &ci, sizeof(ci), KO_FILE_COMPLETION_INFORMATION);
        if (!NT_SUCCESS(s)) {
            ko_status(s);
            return NULL;
        }
        return existing;
    }
}

BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, DWORD *bytes, ULONG_PTR *key, void **overlapped, DWORD ms)
{
    LARGE_INTEGER t;
    IO_STATUS_BLOCK iosb;
    ULONG_PTR k = 0;
    PVOID ov = NULL;
    NTSTATUS s;
    memset(&iosb, 0, sizeof(iosb));
    s = NtRemoveIoCompletion(port, &k, &ov, &iosb, k32o_timeout(ms, &t));
    if (s == STATUS_TIMEOUT) {
        if (overlapped)
            *overlapped = NULL;
        SetLastError(K32O_WAIT_TIMEOUT);
        return FALSE;
    }
    if (!NT_SUCCESS(s)) {
        if (overlapped)
            *overlapped = NULL;
        ko_status(s);
        return FALSE;
    }
    if (bytes)
        *bytes = (DWORD)iosb.Information;
    if (key)
        *key = k;
    if (overlapped)
        *overlapped = ov;
    if (!NT_SUCCESS(iosb.DUMMYUNIONNAME.Status)) {
        /* the I/O itself failed: the call returns FALSE with the packet's data filled in */
        SetLastError(RtlNtStatusToDosError(iosb.DUMMYUNIONNAME.Status));
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, void *overlapped)
{
    NTSTATUS s = NtSetIoCompletion(port, key, overlapped, STATUS_SUCCESS, bytes);
    ko_status(s);
    return NT_SUCCESS(s);
}

/* OVERLAPPED_ENTRY: { ULONG_PTR lpCompletionKey; LPOVERLAPPED lpOverlapped; ULONG_PTR Internal; DWORD dwNumberOfBytesTransferred; } */
typedef struct {
    ULONG_PTR key;
    void *overlapped;
    ULONG_PTR internal;
    DWORD bytes;
} KO_OVERLAPPED_ENTRY;

/* FILE_IO_COMPLETION_INFORMATION as the kernel fills it: key, value, status, information */
typedef struct {
    ULONG_PTR Key;
    PVOID Value;
    NTSTATUS Status;
    ULONG_PTR Information;
} KO_IO_COMPLETION_INFO;

BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE port, KO_OVERLAPPED_ENTRY *entries, ULONG count, ULONG *removed, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER t;
    KO_IO_COMPLETION_INFO info[64];
    ULONG got = 0;
    NTSTATUS s;
    if (!entries || !removed || count == 0) {
        SetLastError(K32O_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (count > 64)
        count = 64;
    s = NtRemoveIoCompletionEx(port, info, count, &got, k32o_timeout(ms, &t), alertable ? TRUE : FALSE);
    if (s == STATUS_TIMEOUT) {
        SetLastError(K32O_WAIT_TIMEOUT);
        return FALSE;
    }
    if (s == K32O_STATUS_USER_APC || s == K32O_STATUS_ALERTED) {
        *removed = 0;
        SetLastError(K32O_WAIT_IO_COMPLETION);
        return FALSE;
    }
    if (!NT_SUCCESS(s)) {
        ko_status(s);
        return FALSE;
    }
    for (ULONG i = 0; i < got; ++i) {
        entries[i].key = info[i].Key;
        entries[i].overlapped = info[i].Value;
        entries[i].internal = (ULONG_PTR)info[i].Status;
        entries[i].bytes = (DWORD)info[i].Information;
    }
    *removed = got;
    SetLastError(0);
    return TRUE;
}

#endif /* __MINGW32__ */
