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
    ctx.Rip = 0x140001234ULL;        /* not inside any registered function */
    ctx.Rsp = 0x7fff0000ULL;

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

static const mt_case_t cases[] = {
    { "veh_continue_execution_stops_dispatch", veh_continue_execution_stops_dispatch },
    { "veh_continue_search_keeps_searching", veh_continue_search_keeps_searching },
    { "removed_veh_no_longer_runs", removed_veh_no_longer_runs },
};

int main(void)
{
    return mt_run("except", cases, sizeof(cases) / sizeof(cases[0]));
}
