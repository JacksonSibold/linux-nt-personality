/*
 * kernel32_fiber.c - fibers: ConvertThreadToFiber, CreateFiber, SwitchToFiber, DeleteFiber and friends.
 *
 * A fiber is a record that starts with the fiber data (GetFiberData reads it through the TEB). A switch saves the running
 * context on its stack (k32_fiber_pure.h) and carries the stack bounds and fiber pointer in the TEB over to the fiber being
 * entered, so exception dispatch and stack checks see the right stack. A new fiber's function returning ends the thread, as
 * on Windows.
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
#include "k32_fiber_pure.h"
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

#define KF_ERROR_INVALID_PARAMETER 87u
#define KF_ERROR_NOT_ENOUGH_MEMORY 8u
#define KF_ERROR_INVALID_HANDLE 6u
#define KF_DEFAULT_STACK (1024u * 1024u)
#define KF_MIN_STACK (16u * 1024u)

void SetLastError(DWORD e);
HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
PVOID WINAPI VirtualAlloc(PVOID, SIZE_T, DWORD, DWORD);
BOOL WINAPI VirtualFree(PVOID, SIZE_T, DWORD);
void WINAPI ExitThread(DWORD);

typedef VOID (WINAPI *KF_START)(PVOID);

typedef struct kf_fiber {
    PVOID fiber_data;       /* must be first: GetFiberData reads it */
    PVOID exception_list;
    PVOID stack_base, stack_limit, dealloc_stack;
    uint64_t sp;            /* saved stack pointer while not running */
    KF_START start;
    int owns_stack;
    int is_thread_fiber;
} kf_fiber;

static PTEB kf_teb(void)
{
    PTEB t;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(t) :: "memory");
    return t;
}

static kf_fiber *kf_current(void)
{
    PTEB t = kf_teb();
    return (kf_fiber *)t->NtTib.DUMMYUNIONNAME.FiberData;
}

static int kf_thread_is_fiber(void)
{
    PTEB t = kf_teb();
    /* FiberData doubles as the Version field of a thread that is not a fiber: a small number then */
    return (uintptr_t)t->NtTib.DUMMYUNIONNAME.FiberData > 0xFFFFu;
}

LPVOID WINAPI ConvertThreadToFiberEx(PVOID data, DWORD flags)
{
    PTEB t = kf_teb();
    kf_fiber *f;
    (void)flags;
    if (kf_thread_is_fiber()) {
        SetLastError(1280u); /* ERROR_ALREADY_FIBER */
        return NULL;
    }
    f = (kf_fiber *)HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, sizeof *f);
    if (!f) {
        SetLastError(KF_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    f->fiber_data = data;
    f->stack_base = t->NtTib.StackBase;
    f->stack_limit = t->NtTib.StackLimit;
    f->dealloc_stack = t->DeallocationStack;
    f->is_thread_fiber = 1;
    t->NtTib.DUMMYUNIONNAME.FiberData = f;
    return f;
}

LPVOID WINAPI ConvertThreadToFiber(PVOID data) { return ConvertThreadToFiberEx(data, 0); }

BOOL WINAPI IsThreadAFiber(void) { return kf_thread_is_fiber() ? TRUE : FALSE; }

BOOL WINAPI ConvertFiberToThread(void)
{
    PTEB t = kf_teb();
    kf_fiber *f;
    if (!kf_thread_is_fiber()) {
        SetLastError(KF_ERROR_INVALID_PARAMETER); /* not a fiber */
        return FALSE;
    }
    f = kf_current();
    if (!f->is_thread_fiber) {
        SetLastError(KF_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    t->NtTib.DUMMYUNIONNAME.FiberData = NULL;
    HeapFree(GetProcessHeap(), 0, f);
    return TRUE;
}

/* The function every new fiber enters: its start routine, then the end of the thread. */
static void kf_main(void *arg)
{
    kf_fiber *f = (kf_fiber *)arg;
    f->start(f->fiber_data);
    ExitThread(0);
    for (;;) {
    }
}

LPVOID WINAPI CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, KF_START start, PVOID data)
{
    kf_fiber *f;
    SIZE_T size = reserve ? reserve : (commit ? commit : KF_DEFAULT_STACK);
    uint8_t *stack;
    (void)flags;
    if (!start) {
        SetLastError(KF_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (size < KF_MIN_STACK)
        size = KF_MIN_STACK;
    size = (size + 0xFFFu) & ~(SIZE_T)0xFFFu;
    f = (kf_fiber *)HeapAlloc(GetProcessHeap(), 8, sizeof *f);
    if (!f) {
        SetLastError(KF_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    stack = (uint8_t *)VirtualAlloc(NULL, size, 0x3000 /* MEM_COMMIT | MEM_RESERVE */, 4 /* PAGE_READWRITE */);
    if (!stack) {
        HeapFree(GetProcessHeap(), 0, f);
        SetLastError(KF_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    f->fiber_data = data;
    f->start = start;
    f->stack_base = stack + size;
    f->stack_limit = stack;
    f->dealloc_stack = stack;
    f->owns_stack = 1;
    f->sp = k32_fiber_prepare(stack + size, kf_main, f);
    return f;
}

LPVOID WINAPI CreateFiber(SIZE_T size, KF_START start, PVOID data) { return CreateFiberEx(size, 0, 0, start, data); }

VOID WINAPI SwitchToFiber(PVOID target)
{
    PTEB t = kf_teb();
    kf_fiber *from, *to = (kf_fiber *)target;
    if (!to || !kf_thread_is_fiber()) {
        SetLastError(KF_ERROR_INVALID_PARAMETER);
        return;
    }
    from = kf_current();
    if (from == to)
        return;
    /* leave this fiber's bounds with it and take the other's */
    from->exception_list = t->NtTib.ExceptionList;
    from->stack_base = t->NtTib.StackBase;
    from->stack_limit = t->NtTib.StackLimit;
    from->dealloc_stack = t->DeallocationStack;
    t->NtTib.ExceptionList = (struct _EXCEPTION_REGISTRATION_RECORD *)to->exception_list;
    t->NtTib.StackBase = to->stack_base;
    t->NtTib.StackLimit = to->stack_limit;
    t->DeallocationStack = to->dealloc_stack;
    t->NtTib.DUMMYUNIONNAME.FiberData = to;
    k32_fiber_swap(&from->sp, to->sp);
}

VOID WINAPI DeleteFiber(PVOID fiber)
{
    kf_fiber *f = (kf_fiber *)fiber;
    if (!f) {
        SetLastError(KF_ERROR_INVALID_PARAMETER);
        return;
    }
    if (f == kf_current() && kf_thread_is_fiber()) {
        /* deleting the running fiber ends the thread */
        ExitThread(0);
    }
    if (f->owns_stack)
        VirtualFree(f->dealloc_stack, 0, 0x8000 /* MEM_RELEASE */);
    HeapFree(GetProcessHeap(), 0, f);
}

PVOID WINAPI GetCurrentFiber(void) { return kf_thread_is_fiber() ? kf_current() : NULL; }
PVOID WINAPI GetFiberData(void) { return kf_thread_is_fiber() ? kf_current()->fiber_data : NULL; }

#endif /* __MINGW32__ */
