/*
 * kernel32_tp.c - QueueUserWorkItem, the thread pool API (pools, work, timers, waits, cleanup groups, callback instances),
 * the timer queues and RegisterWaitForSingleObject, on the engine in k32_tp_pure.h.
 *
 * The engine is host-tested with pthreads; this file supplies its operating system operations from kernel32 (critical
 * sections, condition variables, CreateThread, events) and maps the Win32 objects and their callback conventions onto it.
 * Differences from Windows: a closed private pool is not freed (its idle workers may still look at it); callbacks submitted
 * with TrySubmitThreadpoolCallback are not tracked by a cleanup group; a wait object uses one thread while it is armed.
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
#include "k32_tp_pure.h"
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
typedef void VOID;
typedef PVOID LPVOID;
typedef HANDLE HMODULE;
typedef unsigned char BOOLEAN_;

#define KT_INFINITE 0xFFFFFFFFu
#define KT_INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define KT_ERROR_INVALID_PARAMETER 87u
#define KT_ERROR_NOT_ENOUGH_MEMORY 8u
#define KT_ERROR_IO_PENDING 997u
#define KT_ERROR_TIMEOUT 1460u
#define KT_WT_EXECUTEONLYONCE 0x8u

void SetLastError(DWORD e);
DWORD GetLastError(void);
HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
/* kernel32.def forwards these to ntdll, so they are called there directly. */
__declspec(dllimport) NTSTATUS WINAPI RtlInitializeCriticalSection(PVOID cs);
__declspec(dllimport) NTSTATUS WINAPI RtlEnterCriticalSection(PVOID cs);
__declspec(dllimport) NTSTATUS WINAPI RtlLeaveCriticalSection(PVOID cs);
__declspec(dllimport) VOID WINAPI RtlWakeConditionVariable(PVOID cond);
__declspec(dllimport) VOID WINAPI RtlWakeAllConditionVariable(PVOID cond);
#define InitializeCriticalSection(cs) ((void)RtlInitializeCriticalSection(cs))
#define EnterCriticalSection(cs) ((void)RtlEnterCriticalSection(cs))
#define LeaveCriticalSection(cs) ((void)RtlLeaveCriticalSection(cs))
#define WakeConditionVariable RtlWakeConditionVariable
#define WakeAllConditionVariable RtlWakeAllConditionVariable
BOOL WINAPI SleepConditionVariableCS(PVOID cond, PVOID cs, DWORD ms);
HANDLE WINAPI CreateThread(PVOID, SIZE_T, DWORD (WINAPI *)(PVOID), PVOID, DWORD, DWORD *);
BOOL WINAPI CloseHandle(HANDLE);
ULONGLONG WINAPI GetTickCount64(void);
HANDLE WINAPI CreateEventW(PVOID, BOOL, BOOL, const WCHAR *);
BOOL WINAPI SetEvent(HANDLE);
DWORD WINAPI WaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD);
BOOL WINAPI ReleaseSemaphore(HANDLE, LONG, LONG *);
BOOL WINAPI ReleaseMutex(HANDLE);
BOOL WINAPI FreeLibrary(HMODULE);
BOOL WINAPI DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
HANDLE WINAPI GetCurrentProcess(void);
VOID WINAPI GetSystemTimeAsFileTime(ULONGLONG *);

/* ---- The engine on kernel32 ---------------------------------------------------------------------------------------------- */

static void *kt_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, n ? n : 1); }
static void kt_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static void *kt_lock_new(void)
{
    void *cs = kt_alloc(64);
    if (cs)
        InitializeCriticalSection(cs);
    return cs;
}

static void kt_lock(void *l) { EnterCriticalSection(l); }
static void kt_unlock(void *l) { LeaveCriticalSection(l); }
static void *kt_cond_new(void) { return kt_alloc(16); }

static int kt_cond_wait(void *cond, void *lock, uint64_t ms)
{
    DWORD t = ms == KTP_FOREVER ? KT_INFINITE : (ms >= 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)ms);
    if (SleepConditionVariableCS(cond, lock, t))
        return 0;
    return GetLastError() == KT_ERROR_TIMEOUT ? 1 : 0;
}

static void kt_cond_signal(void *c) { WakeConditionVariable(c); }
static void kt_cond_broadcast(void *c) { WakeAllConditionVariable(c); }

typedef struct {
    void (*fn)(void *);
    void *arg;
} kt_spawn_arg;

static DWORD WINAPI kt_thread_main(PVOID p)
{
    kt_spawn_arg a = *(kt_spawn_arg *)p;
    kt_free(p);
    a.fn(a.arg);
    return 0;
}

static int kt_spawn(void (*fn)(void *), void *arg)
{
    kt_spawn_arg *a = (kt_spawn_arg *)kt_alloc(sizeof *a);
    HANDLE h;
    if (!a)
        return -1;
    a->fn = fn;
    a->arg = arg;
    h = CreateThread(NULL, 0, kt_thread_main, a, 0, NULL);
    if (!h) {
        kt_free(a);
        return -1;
    }
    CloseHandle(h);
    return 0;
}

static uint64_t kt_now(void) { return GetTickCount64(); }
static void *kt_event_new(void) { return CreateEventW(NULL, TRUE, FALSE, NULL); }
static void kt_event_set(void *e) { SetEvent((HANDLE)e); }
static void kt_event_free(void *e) { CloseHandle((HANDLE)e); }

static int kt_wait2(void *h, void *cancel, uint64_t ms)
{
    HANDLE hs[2];
    DWORD r, t = ms == KTP_FOREVER ? KT_INFINITE : (ms >= 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)ms);
    hs[0] = (HANDLE)h;
    hs[1] = (HANDLE)cancel;
    r = WaitForMultipleObjects(2, hs, FALSE, t);
    if (r == 0)
        return 0;
    if (r == 258) /* WAIT_TIMEOUT */
        return 1;
    return 2; /* the cancel event, or a failed wait: the arming is over */
}

static const ktp_os g_kt_os = {kt_lock_new, kt_lock, kt_unlock, kt_cond_new, kt_cond_wait, kt_cond_signal, kt_cond_broadcast, kt_spawn,
                               kt_now, kt_alloc, kt_free, kt_event_new, kt_event_set, kt_event_free, kt_wait2};

/* The default pool, created on first use. */
static ktp g_kt_default;
static volatile LONG g_kt_state; /* 0 not started, 1 starting, 2 ready */

static ktp *kt_default(void)
{
    if (g_kt_state != 2) {
        if (__sync_bool_compare_and_swap(&g_kt_state, 0, 1)) {
            ktp_init(&g_kt_default, &g_kt_os);
            __sync_synchronize();
            g_kt_state = 2;
        } else {
            while (g_kt_state != 2)
                __asm__ volatile("pause");
        }
    }
    return &g_kt_default;
}

/* ---- Callback environment, pools, instances --------------------------------------------------------------------------------- */

typedef struct {
    DWORD Version;
    PVOID Pool;
    PVOID CleanupGroup;
    PVOID CleanupGroupCancelCallback;
    PVOID RaceDll;
    PVOID ActivationContext;
    PVOID FinalizationCallback;
    DWORD Flags;
} KT_ENV;

typedef struct kt_pool {
    ktp tp;
} kt_pool;

static ktp *kt_pool_of(const KT_ENV *env) { return env && env->Pool ? &((kt_pool *)env->Pool)->tp : kt_default(); }

PVOID WINAPI CreateThreadpool(PVOID reserved)
{
    kt_pool *p;
    (void)reserved;
    p = (kt_pool *)kt_alloc(sizeof *p);
    if (!p) {
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (ktp_init(&p->tp, &g_kt_os)) {
        kt_free(p);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return p;
}

VOID WINAPI SetThreadpoolThreadMaximum(PVOID pool, DWORD max)
{
    ktp *tp = &((kt_pool *)pool)->tp;
    ktp_set_limits(tp, tp->min_workers, max);
}

BOOL WINAPI SetThreadpoolThreadMinimum(PVOID pool, DWORD min)
{
    ktp *tp = &((kt_pool *)pool)->tp;
    ktp_set_limits(tp, min, tp->max_workers);
    return TRUE;
}

/* A closed pool is left allocated: idle workers of it may still be looking at it. */
VOID WINAPI CloseThreadpool(PVOID pool) { (void)pool; }

typedef struct {
    SIZE_T StackReserve, StackCommit;
} KT_STACK_INFO;

BOOL WINAPI SetThreadpoolStackInformation(PVOID pool, KT_STACK_INFO *info)
{
    (void)pool;
    (void)info;
    return TRUE;
}

BOOL WINAPI QueryThreadpoolStackInformation(PVOID pool, KT_STACK_INFO *info)
{
    (void)pool;
    if (!info) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    info->StackReserve = 0;
    info->StackCommit = 0;
    return TRUE;
}

/* The state a callback can ask to have done when it returns. */
typedef struct {
    HANDLE evt, sem, mutex;
    LONG sem_count;
    PVOID cs;
    HMODULE lib;
} KT_INSTANCE;

static void kt_instance_finish(KT_INSTANCE *i)
{
    if (i->cs)
        LeaveCriticalSection(i->cs);
    if (i->mutex)
        ReleaseMutex(i->mutex);
    if (i->sem)
        ReleaseSemaphore(i->sem, i->sem_count, NULL);
    if (i->evt)
        SetEvent(i->evt);
    if (i->lib)
        FreeLibrary(i->lib);
}

VOID WINAPI SetEventWhenCallbackReturns(KT_INSTANCE *i, HANDLE e) { if (i) i->evt = e; }
VOID WINAPI ReleaseSemaphoreWhenCallbackReturns(KT_INSTANCE *i, HANDLE s, DWORD n) { if (i) { i->sem = s; i->sem_count = (LONG)n; } }
VOID WINAPI ReleaseMutexWhenCallbackReturns(KT_INSTANCE *i, HANDLE m) { if (i) i->mutex = m; }
VOID WINAPI LeaveCriticalSectionWhenCallbackReturns(KT_INSTANCE *i, PVOID cs) { if (i) i->cs = cs; }
VOID WINAPI FreeLibraryWhenCallbackReturns(KT_INSTANCE *i, HMODULE lib) { if (i) i->lib = lib; }
BOOL WINAPI CallbackMayRunLong(KT_INSTANCE *i) { (void)i; return TRUE; }
VOID WINAPI DisassociateCurrentThreadFromCallback(KT_INSTANCE *i) { (void)i; }

/* ---- Cleanup groups ---------------------------------------------------------------------------------------------------------- */

enum { KT_WORK, KT_TIMER, KT_WAIT };

typedef struct kt_member {
    struct kt_member *next, *prev;
    int type;
    void *object;       /* the kt_work, kt_timer or kt_wait */
    PVOID user_ctx;     /* what the cancel callback receives */
    PVOID cancel_cb;    /* from the environment */
    struct kt_group *group;
} kt_member;

typedef struct kt_group {
    PVOID lock;
    kt_member *head;
} kt_group;

PVOID WINAPI CreateThreadpoolCleanupGroup(void)
{
    kt_group *g = (kt_group *)kt_alloc(sizeof *g);
    if (!g) {
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    g->lock = kt_lock_new();
    return g;
}

static void kt_group_add(kt_group *g, kt_member *m)
{
    m->group = g;
    kt_lock(g->lock);
    m->prev = NULL;
    m->next = g->head;
    if (g->head)
        g->head->prev = m;
    g->head = m;
    kt_unlock(g->lock);
}

static void kt_group_remove(kt_member *m)
{
    kt_group *g = m->group;
    if (!g)
        return;
    kt_lock(g->lock);
    if (m->prev)
        m->prev->next = m->next;
    else
        g->head = m->next;
    if (m->next)
        m->next->prev = m->prev;
    kt_unlock(g->lock);
    m->group = NULL;
}

/* ---- Work ---------------------------------------------------------------------------------------------------------------------- */

typedef VOID (WINAPI *KT_WORK_CB)(KT_INSTANCE *, PVOID, PVOID);
typedef VOID (WINAPI *KT_TIMER_CB)(KT_INSTANCE *, PVOID, PVOID);
typedef VOID (WINAPI *KT_WAIT_CB)(KT_INSTANCE *, PVOID, PVOID, ULONG);
typedef VOID (WINAPI *KT_SIMPLE_CB)(KT_INSTANCE *, PVOID);
typedef VOID (WINAPI *KT_CANCEL_CB)(PVOID, PVOID);
typedef VOID (WINAPI *KT_FINAL_CB)(KT_INSTANCE *, PVOID);

typedef struct kt_work {
    ktp_work *w;
    KT_WORK_CB cb;
    PVOID ctx;
    kt_member member;
    KT_FINAL_CB final;
} kt_work;

static void kt_work_run(void *ctx, ktp_work *w)
{
    kt_work *k = (kt_work *)ctx;
    KT_INSTANCE inst;
    (void)w;
    memset(&inst, 0, sizeof inst);
    k->cb(&inst, k->ctx, k);
    kt_instance_finish(&inst);
}

PVOID WINAPI CreateThreadpoolWork(KT_WORK_CB cb, PVOID ctx, const KT_ENV *env)
{
    kt_work *k = (kt_work *)kt_alloc(sizeof *k);
    if (!k || !cb) {
        kt_free(k);
        SetLastError(cb ? KT_ERROR_NOT_ENOUGH_MEMORY : KT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    k->cb = cb;
    k->ctx = ctx;
    k->w = ktp_work_new(kt_pool_of(env), kt_work_run, k);
    if (!k->w) {
        kt_free(k);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (env && env->CleanupGroup) {
        k->member.type = KT_WORK;
        k->member.object = k;
        k->member.cancel_cb = env->CleanupGroupCancelCallback;
        k->member.user_ctx = ctx;
        kt_group_add((kt_group *)env->CleanupGroup, &k->member);
    }
    return k;
}

VOID WINAPI SubmitThreadpoolWork(PVOID work) { ktp_work_submit(((kt_work *)work)->w); }
VOID WINAPI WaitForThreadpoolWorkCallbacks(PVOID work, BOOL cancel) { ktp_work_wait(((kt_work *)work)->w, cancel ? 1 : 0); }

VOID WINAPI CloseThreadpoolWork(PVOID work)
{
    kt_work *k = (kt_work *)work;
    kt_group_remove(&k->member);
    ktp_work_close(k->w);
    kt_free(k);
}

typedef struct {
    KT_SIMPLE_CB cb;
    PVOID ctx;
} kt_simple;

static void kt_simple_run(void *arg)
{
    kt_simple s = *(kt_simple *)arg;
    KT_INSTANCE inst;
    kt_free(arg);
    memset(&inst, 0, sizeof inst);
    s.cb(&inst, s.ctx);
    kt_instance_finish(&inst);
}

BOOL WINAPI TrySubmitThreadpoolCallback(KT_SIMPLE_CB cb, PVOID ctx, const KT_ENV *env)
{
    kt_simple *s;
    if (!cb) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    s = (kt_simple *)kt_alloc(sizeof *s);
    if (!s) {
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    s->cb = cb;
    s->ctx = ctx;
    if (ktp_submit(kt_pool_of(env), kt_simple_run, s)) {
        kt_free(s);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return TRUE;
}

typedef struct {
    DWORD (WINAPI *fn)(PVOID);
    PVOID ctx;
} kt_uwi;

static void kt_uwi_run(void *arg)
{
    kt_uwi u = *(kt_uwi *)arg;
    kt_free(arg);
    u.fn(u.ctx);
}

BOOL WINAPI QueueUserWorkItem(DWORD (WINAPI *fn)(PVOID), PVOID ctx, ULONG flags)
{
    kt_uwi *u;
    (void)flags;
    if (!fn) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    u = (kt_uwi *)kt_alloc(sizeof *u);
    if (!u) {
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    u->fn = fn;
    u->ctx = ctx;
    if (ktp_submit(kt_default(), kt_uwi_run, u)) {
        kt_free(u);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return TRUE;
}

/* ---- Timers (thread pool objects) ----------------------------------------------------------------------------------------------- */

typedef struct kt_timer {
    ktp_timer *t;
    KT_TIMER_CB cb;
    PVOID ctx;
    kt_member member;
} kt_timer;

static void kt_timer_run(void *ctx, ktp_timer *t)
{
    kt_timer *k = (kt_timer *)ctx;
    KT_INSTANCE inst;
    (void)t;
    memset(&inst, 0, sizeof inst);
    k->cb(&inst, k->ctx, k);
    kt_instance_finish(&inst);
}

PVOID WINAPI CreateThreadpoolTimer(KT_TIMER_CB cb, PVOID ctx, const KT_ENV *env)
{
    kt_timer *k = (kt_timer *)kt_alloc(sizeof *k);
    if (!k || !cb) {
        kt_free(k);
        SetLastError(cb ? KT_ERROR_NOT_ENOUGH_MEMORY : KT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    k->cb = cb;
    k->ctx = ctx;
    k->t = ktp_timer_new(kt_pool_of(env), kt_timer_run, k);
    if (!k->t) {
        kt_free(k);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (env && env->CleanupGroup) {
        k->member.type = KT_TIMER;
        k->member.object = k;
        k->member.cancel_cb = env->CleanupGroupCancelCallback;
        k->member.user_ctx = ctx;
        kt_group_add((kt_group *)env->CleanupGroup, &k->member);
    }
    return k;
}

/* A FILETIME timeout as milliseconds from now: negative values are relative in 100 ns units, positive ones absolute (UTC). */
static int64_t kt_filetime_to_ms(int64_t ft)
{
    if (ft < 0)
        return (-ft) / 10000;
    {
        ULONGLONG now;
        GetSystemTimeAsFileTime(&now);
        if ((ULONGLONG)ft <= now)
            return 0;
        return (int64_t)(((ULONGLONG)ft - now) / 10000);
    }
}

BOOL WINAPI SetThreadpoolTimerEx(PVOID timer, const int64_t *due, DWORD period, DWORD window)
{
    kt_timer *k = (kt_timer *)timer;
    (void)window;
    return ktp_timer_set(k->t, due ? kt_filetime_to_ms(*due) : -1, period) == 0 ? TRUE : FALSE;
}

VOID WINAPI SetThreadpoolTimer(PVOID timer, const int64_t *due, DWORD period, DWORD window)
{
    SetThreadpoolTimerEx(timer, due, period, window);
}

BOOL WINAPI IsThreadpoolTimerSet(PVOID timer) { return ktp_timer_is_set(((kt_timer *)timer)->t) ? TRUE : FALSE; }
VOID WINAPI WaitForThreadpoolTimerCallbacks(PVOID timer, BOOL cancel) { ktp_timer_wait(((kt_timer *)timer)->t, cancel ? 1 : 0); }

VOID WINAPI CloseThreadpoolTimer(PVOID timer)
{
    kt_timer *k = (kt_timer *)timer;
    kt_group_remove(&k->member);
    ktp_timer_close(k->t);
    kt_free(k);
}

/* ---- Waits (thread pool objects) ----------------------------------------------------------------------------------------------- */

typedef struct kt_wait {
    ktp_wait *w;
    KT_WAIT_CB cb;
    PVOID ctx;
    kt_member member;
} kt_wait;

static void kt_wait_run(void *ctx, ktp_wait *w, int result)
{
    kt_wait *k = (kt_wait *)ctx;
    KT_INSTANCE inst;
    (void)w;
    memset(&inst, 0, sizeof inst);
    k->cb(&inst, k->ctx, k, result == 1 ? 258u : 0u); /* WAIT_TIMEOUT or WAIT_OBJECT_0 */
    kt_instance_finish(&inst);
}

PVOID WINAPI CreateThreadpoolWait(KT_WAIT_CB cb, PVOID ctx, const KT_ENV *env)
{
    kt_wait *k = (kt_wait *)kt_alloc(sizeof *k);
    if (!k || !cb) {
        kt_free(k);
        SetLastError(cb ? KT_ERROR_NOT_ENOUGH_MEMORY : KT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    k->cb = cb;
    k->ctx = ctx;
    k->w = ktp_wait_new(kt_pool_of(env), kt_wait_run, k, 0);
    if (!k->w) {
        kt_free(k);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (env && env->CleanupGroup) {
        k->member.type = KT_WAIT;
        k->member.object = k;
        k->member.cancel_cb = env->CleanupGroupCancelCallback;
        k->member.user_ctx = ctx;
        kt_group_add((kt_group *)env->CleanupGroup, &k->member);
    }
    return k;
}

VOID WINAPI SetThreadpoolWait(PVOID wait, HANDLE h, const int64_t *timeout)
{
    kt_wait *k = (kt_wait *)wait;
    ktp_wait_set(k->w, h, h && timeout ? (uint64_t)kt_filetime_to_ms(*timeout) : KTP_FOREVER);
}

BOOL WINAPI SetThreadpoolWaitEx(PVOID wait, HANDLE h, const int64_t *timeout, PVOID reserved)
{
    (void)reserved;
    SetThreadpoolWait(wait, h, timeout);
    return TRUE;
}

VOID WINAPI WaitForThreadpoolWaitCallbacks(PVOID wait, BOOL cancel) { ktp_wait_wait(((kt_wait *)wait)->w, cancel ? 1 : 0); }

VOID WINAPI CloseThreadpoolWait(PVOID wait)
{
    kt_wait *k = (kt_wait *)wait;
    kt_group_remove(&k->member);
    ktp_wait_close(k->w);
    kt_free(k);
}

/* Closes every member of the group: callbacks that have not started are dropped when cancel is set (and the cancel callback
 * is told), running ones are waited for. */
VOID WINAPI CloseThreadpoolCleanupGroupMembers(PVOID group, BOOL cancel, PVOID cleanup_ctx)
{
    kt_group *g = (kt_group *)group;
    for (;;) {
        kt_member *m;
        kt_lock(g->lock);
        m = g->head;
        kt_unlock(g->lock);
        if (!m)
            break;
        if (cancel && m->cancel_cb)
            ((KT_CANCEL_CB)m->cancel_cb)(m->user_ctx, cleanup_ctx);
        switch (m->type) {
        case KT_WORK: CloseThreadpoolWork(m->object); break;
        case KT_TIMER: CloseThreadpoolTimer(m->object); break;
        default: CloseThreadpoolWait(m->object); break;
        }
    }
}

VOID WINAPI CloseThreadpoolCleanupGroup(PVOID group) { kt_free(group); }

/* ---- Timer queues -------------------------------------------------------------------------------------------------------------- */

typedef VOID (WINAPI *KT_WAITORTIMER)(PVOID, BOOLEAN_);

typedef struct kt_tq kt_tq;

typedef struct kt_tqt {
    ktp_timer *t;
    KT_WAITORTIMER cb;
    PVOID ctx;
    kt_tq *q;
    struct kt_tqt *next;
} kt_tqt;

struct kt_tq {
    kt_tqt *timers;
    int deleting;
};

static kt_tq g_kt_default_queue;
static PVOID g_kt_tq_lock;
static volatile LONG g_kt_tq_state;

/* One lock guards every queue's list of timers. */
static PVOID kt_tq_lock(void)
{
    if (g_kt_tq_state != 2) {
        if (__sync_bool_compare_and_swap(&g_kt_tq_state, 0, 1)) {
            g_kt_tq_lock = kt_lock_new();
            __sync_synchronize();
            g_kt_tq_state = 2;
        } else {
            while (g_kt_tq_state != 2)
                __asm__ volatile("pause");
        }
    }
    return g_kt_tq_lock;
}

static void kt_tqt_run(void *ctx, ktp_timer *t)
{
    kt_tqt *k = (kt_tqt *)ctx;
    (void)t;
    k->cb(k->ctx, 1);
}

HANDLE WINAPI CreateTimerQueue(void)
{
    kt_tq *q = (kt_tq *)kt_alloc(sizeof *q);
    if (!q)
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
    return q;
}

BOOL WINAPI CreateTimerQueueTimer(HANDLE *out, HANDLE queue, KT_WAITORTIMER cb, PVOID ctx, DWORD due, DWORD period, ULONG flags)
{
    kt_tq *q = queue ? (kt_tq *)queue : &g_kt_default_queue;
    kt_tqt *k;
    if (!out || !cb) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    k = (kt_tqt *)kt_alloc(sizeof *k);
    if (!k) {
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    k->cb = cb;
    k->ctx = ctx;
    k->q = q;
    k->t = ktp_timer_new(kt_default(), kt_tqt_run, k);
    if (!k->t) {
        kt_free(k);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    kt_lock(kt_tq_lock());
    k->next = q->timers;
    q->timers = k;
    kt_unlock(kt_tq_lock());
    ktp_timer_set(k->t, (int64_t)due, (flags & KT_WT_EXECUTEONLYONCE) ? 0 : period);
    *out = k;
    return TRUE;
}

BOOL WINAPI ChangeTimerQueueTimer(HANDLE queue, HANDLE timer, ULONG due, ULONG period)
{
    kt_tqt *k = (kt_tqt *)timer;
    (void)queue;
    if (!k) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return ktp_timer_set(k->t, (int64_t)due, period) == 0 ? TRUE : FALSE;
}

BOOL WINAPI CancelTimerQueueTimer(HANDLE queue, HANDLE timer)
{
    kt_tqt *k = (kt_tqt *)timer;
    (void)queue;
    if (!k) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ktp_timer_set(k->t, -1, 0);
    return TRUE;
}

static void kt_tqt_unlink(kt_tqt *k)
{
    kt_tqt **pp;
    kt_lock(kt_tq_lock());
    for (pp = &k->q->timers; *pp; pp = &(*pp)->next)
        if (*pp == k) {
            *pp = k->next;
            break;
        }
    kt_unlock(kt_tq_lock());
}

static unsigned kt_outstanding(ktp_timer *t)
{
    unsigned n;
    t->tp->os->lock(t->tp->lock);
    n = t->outstanding;
    t->tp->os->unlock(t->tp->lock);
    return n;
}

/* A deletion whose callbacks are still running finishes on a pool thread, which then signals the event, if one was given. */
typedef struct {
    kt_tqt *k;
    kt_tq *q_to_free;
    HANDLE evt;
} kt_tq_finish;

static void kt_tq_finish_run(void *arg)
{
    kt_tq_finish *f = (kt_tq_finish *)arg;
    if (f->k) {
        ktp_timer_close(f->k->t);
        kt_free(f->k);
    }
    kt_free(f->q_to_free);
    if (f->evt)
        SetEvent(f->evt);
    kt_free(f);
}

BOOL WINAPI DeleteTimerQueueTimer(HANDLE queue, HANDLE timer, HANDLE completion)
{
    kt_tqt *k = (kt_tqt *)timer;
    (void)queue;
    if (!k) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ktp_timer_set(k->t, -1, 0);
    kt_tqt_unlink(k);
    if (completion == KT_INVALID_HANDLE || kt_outstanding(k->t) == 0) {
        ktp_timer_close(k->t); /* waits for a running callback */
        kt_free(k);
        if (completion && completion != KT_INVALID_HANDLE)
            SetEvent(completion);
        return TRUE;
    }
    {
        kt_tq_finish *f = (kt_tq_finish *)kt_alloc(sizeof *f);
        if (f) {
            f->k = k;
            f->evt = completion;
            ktp_submit(kt_default(), kt_tq_finish_run, f);
        }
    }
    SetLastError(KT_ERROR_IO_PENDING);
    return FALSE;
}

BOOL WINAPI DeleteTimerQueueEx(HANDLE queue, HANDLE completion)
{
    kt_tq *q = (kt_tq *)queue;
    int pending = 0;
    if (!q) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (;;) {
        kt_tqt *k;
        kt_lock(kt_tq_lock());
        k = q->timers;
        if (k)
            q->timers = k->next;
        kt_unlock(kt_tq_lock());
        if (!k)
            break;
        ktp_timer_set(k->t, -1, 0);
        if (completion == KT_INVALID_HANDLE || kt_outstanding(k->t) == 0) {
            ktp_timer_close(k->t);
            kt_free(k);
        } else {
            kt_tq_finish *f = (kt_tq_finish *)kt_alloc(sizeof *f);
            pending = 1;
            if (f) {
                f->k = k;
                ktp_submit(kt_default(), kt_tq_finish_run, f);
            }
        }
    }
    if (pending) {
        if (completion && completion != KT_INVALID_HANDLE) {
            kt_tq_finish *f = (kt_tq_finish *)kt_alloc(sizeof *f);
            if (f) {
                f->evt = completion; /* a final task after the timer tasks, in submission order */
                ktp_submit(kt_default(), kt_tq_finish_run, f);
            }
        }
        if (q != &g_kt_default_queue)
            kt_free(q);
        SetLastError(KT_ERROR_IO_PENDING);
        return FALSE;
    }
    if (q != &g_kt_default_queue)
        kt_free(q);
    if (completion && completion != KT_INVALID_HANDLE)
        SetEvent(completion);
    return TRUE;
}

BOOL WINAPI DeleteTimerQueue(HANDLE queue) { return DeleteTimerQueueEx(queue, NULL); }

/* ---- RegisterWaitForSingleObject ------------------------------------------------------------------------------------------------ */

typedef struct kt_rw {
    ktp_wait *w;
    KT_WAITORTIMER cb;
    PVOID ctx;
    HANDLE own; /* the duplicate of the caller's handle */
} kt_rw;

static void kt_rw_run(void *ctx, ktp_wait *w, int result)
{
    kt_rw *k = (kt_rw *)ctx;
    (void)w;
    k->cb(k->ctx, result == 1 ? 1 : 0);
}

BOOL WINAPI RegisterWaitForSingleObject(HANDLE *out, HANDLE h, KT_WAITORTIMER cb, PVOID ctx, ULONG ms, ULONG flags)
{
    kt_rw *k;
    HANDLE self = GetCurrentProcess();
    if (!out || !cb || !h) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    k = (kt_rw *)kt_alloc(sizeof *k);
    if (!k) {
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    k->cb = cb;
    k->ctx = ctx;
    if (!DuplicateHandle(self, h, self, &k->own, 0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */)) {
        kt_free(k);
        return FALSE;
    }
    k->w = ktp_wait_new(kt_default(), kt_rw_run, k, (flags & KT_WT_EXECUTEONLYONCE) ? 0 : 1);
    if (!k->w || ktp_wait_set(k->w, k->own, ms == KT_INFINITE ? KTP_FOREVER : (uint64_t)ms) != 0) {
        if (k->w)
            ktp_wait_close(k->w);
        CloseHandle(k->own);
        kt_free(k);
        SetLastError(KT_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    *out = k;
    return TRUE;
}

typedef struct {
    kt_rw *k;
    HANDLE evt;
} kt_rw_finish;

static void kt_rw_finish_do(kt_rw *k)
{
    ktp_wait_close(k->w);
    CloseHandle(k->own);
    kt_free(k);
}

static void kt_rw_finish_run(void *arg)
{
    kt_rw_finish *f = (kt_rw_finish *)arg;
    kt_rw_finish_do(f->k);
    if (f->evt)
        SetEvent(f->evt);
    kt_free(f);
}

BOOL WINAPI UnregisterWaitEx(HANDLE wait, HANDLE completion)
{
    kt_rw *k = (kt_rw *)wait;
    if (!k) {
        SetLastError(KT_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ktp_wait_set(k->w, NULL, 0); /* no new callbacks */
    if (completion == KT_INVALID_HANDLE) {
        kt_rw_finish_do(k); /* blocks until a running callback is done */
        return TRUE;
    }
    {
        kt_rw_finish *f = (kt_rw_finish *)kt_alloc(sizeof *f);
        if (!f) {
            kt_rw_finish_do(k);
            return TRUE;
        }
        f->k = k;
        f->evt = completion;
        ktp_submit(kt_default(), kt_rw_finish_run, f);
    }
    SetLastError(KT_ERROR_IO_PENDING);
    return FALSE;
}

BOOL WINAPI UnregisterWait(HANDLE wait)
{
    BOOL r = UnregisterWaitEx(wait, NULL);
    /* UnregisterWait reports success when the cancellation has been started */
    return r || GetLastError() == KT_ERROR_IO_PENDING;
}

#endif /* __MINGW32__ */
