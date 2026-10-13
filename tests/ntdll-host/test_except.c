/*
 * test_except.c - host tests for vectored exception handler dispatch in
 * except.c.
 *
 * A vectored handler returns LONG. Windows defines
 *   EXCEPTION_CONTINUE_EXECUTION = -1
 *   EXCEPTION_CONTINUE_SEARCH    = 0
 * and vectored handlers use no other value. RtlDispatchException must stop
 * and resume on -1 and keep searching on 0.
 *
 * No frame in these tests is registered, so the frame walk finds nothing and
 * only the vectored stage decides the result.
 */
#include "minitest.h"
#include "except.h"

#include <string.h>

#define TEST_CONTINUE_EXECUTION (-1L)
#define TEST_CONTINUE_SEARCH    (0L)

static LONG veh_continue_execution(PEXCEPTION_POINTERS info)
{
    (void)info;
    return TEST_CONTINUE_EXECUTION;
}

static LONG veh_continue_search(PEXCEPTION_POINTERS info)
{
    (void)info;
    return TEST_CONTINUE_SEARCH;
}

static void dispatch_with(PVECTORED_EXCEPTION_HANDLER handler, EXCEPTION_DISPOSITION *out)
{
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof(rec));
    memset(&ctx, 0, sizeof(ctx));
    rec.ExceptionCode = 0xC0000005L; /* STATUS_ACCESS_VIOLATION */
    /* Not inside any registered function: a leaf frame. Its return address
     * (stack[0]) is 0, so the walk ends after one frame (EXC-2 pops it). */
    ULONG64 stack[2] = { 0, 0 };
    ctx.Rip = 0x140001234ULL;
    ctx.Rsp = (ULONG64)(uintptr_t)stack;

    PVOID h = AddVectoredExceptionHandler(1, handler);
    MT_ASSERT(h != NULL);
    *out = RtlDispatchException(&rec, &ctx);
    MT_EXPECT(RemoveVectoredExceptionHandler(h) == 1);
}

static void veh_continue_execution_stops_dispatch(void)
{
    EXCEPTION_DISPOSITION d = ExceptionContinueSearch;
    dispatch_with(veh_continue_execution, &d);
    MT_EXPECT(d == ExceptionContinueExecution);
}

static void veh_continue_search_keeps_searching(void)
{
    EXCEPTION_DISPOSITION d = ExceptionContinueExecution;
    dispatch_with(veh_continue_search, &d);
    MT_EXPECT(d == ExceptionContinueSearch);
}

static void removed_veh_no_longer_runs(void)
{
    EXCEPTION_DISPOSITION d = ExceptionContinueExecution;
    PVOID h = AddVectoredExceptionHandler(1, veh_continue_execution);
    MT_ASSERT(h != NULL);
    MT_ASSERT(RemoveVectoredExceptionHandler(h) == 1);

    dispatch_with(veh_continue_search, &d);
    MT_EXPECT(d == ExceptionContinueSearch);
}

#ifndef __MINGW32__
#include "ntstatus.h"
#include <signal.h>
#include <ucontext.h>

/* A SIGSEGV from a plain Linux process must map to STATUS_ACCESS_VIOLATION with
 * the fault address in ExceptionInformation[1]. A plain process has no TEB and
 * %gs has base 0, so the guard-page check must not read %gs:0x30 (LNX-05). A
 * read there faults inside the handler. */
static void sigsegv_maps_access_violation_without_teb(void)
{
    siginfo_t si;
    ucontext_t uc;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    memset(&si, 0, sizeof si);
    memset(&uc, 0, sizeof uc);
    si.si_signo = SIGSEGV;
    si.si_code = SEGV_MAPERR;
    si.si_addr = (void *)0x1234;
    uc.uc_mcontext.gregs[REG_RIP] = 0x401000;
    signal_to_exception(SIGSEGV, &si, &uc, &rec, &ctx);
    /* ExceptionCode is unsigned; STATUS_ACCESS_VIOLATION is a negative int. */
    MT_EXPECT_EQ((unsigned long)rec.ExceptionCode, (unsigned long)(ULONG)STATUS_ACCESS_VIOLATION);
    MT_EXPECT_EQ(rec.NumberParameters, 2);
    MT_EXPECT_EQ(rec.ExceptionInformation[1], 0x1234);
}
#endif

static const mt_case_t cases[] = {
    { "veh_continue_execution_stops_dispatch", veh_continue_execution_stops_dispatch },
    { "veh_continue_search_keeps_searching", veh_continue_search_keeps_searching },
    { "removed_veh_no_longer_runs", removed_veh_no_longer_runs },
#ifndef __MINGW32__
    { "sigsegv_maps_access_violation_without_teb", sigsegv_maps_access_violation_without_teb },
#endif
};

int main(void)
{
    return mt_run("except", cases, sizeof(cases) / sizeof(cases[0]));
}
