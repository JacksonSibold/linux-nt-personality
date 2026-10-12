/*
 * test_except_pure.c - host tests for exception dispatch bookkeeping
 * (except_pure.h) and the dispatch order in except.c.
 *
 * Pure cases: list insertion and removal, the resume decisions for vectored
 * and frame handler results, the nesting limit and the frame-walk end test.
 *
 * Dispatch cases: vectored handler order and the vectored continue handlers.
 * Windows calls vectored continue handlers whenever an exception is continued,
 * whether the resume came from a vectored handler or a frame handler.
 *
 * No frame is registered, so the frame walk is a single leaf frame whose
 * return address is 0 (EXC-2 pops it), and only the vectored stages decide.
 */
#include "minitest.h"
#include "except.h"
#include "except_pure.h"

#include <string.h>

#define TEST_CONTINUE_EXECUTION (-1L)
#define TEST_CONTINUE_SEARCH    (0L)

/* ---- call log shared by the handlers ---------------------------------- */

static char g_log[16];
static int g_log_n;

static void log_reset(void) { g_log_n = 0; memset(g_log, 0, sizeof(g_log)); }

static void log_push(char c)
{
    if (g_log_n < (int)sizeof(g_log) - 1)
        g_log[g_log_n++] = c;
}

static LONG veh_a(PEXCEPTION_POINTERS info) { (void)info; log_push('a'); return TEST_CONTINUE_SEARCH; }
static LONG veh_b(PEXCEPTION_POINTERS info) { (void)info; log_push('b'); return TEST_CONTINUE_SEARCH; }
static LONG veh_c(PEXCEPTION_POINTERS info) { (void)info; log_push('c'); return TEST_CONTINUE_SEARCH; }
static LONG veh_resume(PEXCEPTION_POINTERS info) { (void)info; log_push('R'); return TEST_CONTINUE_EXECUTION; }
static LONG vch_mark(PEXCEPTION_POINTERS info) { (void)info; log_push('V'); return TEST_CONTINUE_SEARCH; }

static void dispatch_leaf(EXCEPTION_DISPOSITION *out)
{
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof(rec));
    memset(&ctx, 0, sizeof(ctx));
    rec.ExceptionCode = 0xC0000005L; /* STATUS_ACCESS_VIOLATION */
    ULONG64 stack[2] = { 0, 0 };
    ctx.Rip = 0x140001234ULL;
    ctx.Rsp = (ULONG64)(uintptr_t)stack;
    *out = RtlDispatchException(&rec, &ctx);
}

/* ---- pure list management --------------------------------------------- */

static void list_insert_first_prepends(void)
{
    exc_veh_node_t a = {0}, b = {0}, c = {0};
    a.handle = (PVOID)1; b.handle = (PVOID)2; c.handle = (PVOID)3;
    exc_veh_node_t *head = NULL;
    exc_list_insert(&head, &a, 0);
    exc_list_insert(&head, &b, 1);
    exc_list_insert(&head, &c, 1);
    MT_ASSERT(head == &c);
    MT_EXPECT(c.next == &b);
    MT_EXPECT(b.prev == &c);
    MT_EXPECT(b.next == &a);
    MT_EXPECT(a.prev == &b);
    MT_EXPECT(a.next == NULL);
    MT_EXPECT(c.prev == NULL);
}

static void list_insert_last_appends(void)
{
    exc_veh_node_t a = {0}, b = {0}, c = {0};
    a.handle = (PVOID)1; b.handle = (PVOID)2; c.handle = (PVOID)3;
    exc_veh_node_t *head = NULL;
    exc_list_insert(&head, &a, 0);
    exc_list_insert(&head, &b, 0);
    exc_list_insert(&head, &c, 0);
    MT_ASSERT(head == &a);
    MT_EXPECT(a.next == &b);
    MT_EXPECT(b.next == &c);
    MT_EXPECT(c.next == NULL);
    MT_EXPECT(c.prev == &b);
    MT_EXPECT(a.prev == NULL);
}

static void list_remove_head_middle_tail(void)
{
    exc_veh_node_t a = {0}, b = {0}, c = {0};
    a.handle = (PVOID)1; b.handle = (PVOID)2; c.handle = (PVOID)3;
    exc_veh_node_t *head = NULL;
    exc_list_insert(&head, &a, 0);
    exc_list_insert(&head, &b, 0);
    exc_list_insert(&head, &c, 0);

    MT_EXPECT(exc_list_remove(&head, (PVOID)2) == &b);
    MT_EXPECT(a.next == &c);
    MT_EXPECT(c.prev == &a);

    MT_EXPECT(exc_list_remove(&head, (PVOID)1) == &a);
    MT_ASSERT(head == &c);
    MT_EXPECT(c.prev == NULL);

    MT_EXPECT(exc_list_remove(&head, (PVOID)3) == &c);
    MT_EXPECT(head == NULL);
}

static void list_remove_missing_handle_changes_nothing(void)
{
    exc_veh_node_t a = {0};
    a.handle = (PVOID)7;
    exc_veh_node_t *head = NULL;
    exc_list_insert(&head, &a, 0);
    MT_EXPECT(exc_list_remove(&head, (PVOID)8) == NULL);
    MT_EXPECT(head == &a);
    MT_EXPECT(exc_list_remove(&head, (PVOID)7) == &a);
    MT_EXPECT(exc_list_remove(&head, (PVOID)7) == NULL);
}

/* ---- pure decisions ---------------------------------------------------- */

static void veh_resumes_only_on_continue_execution(void)
{
    MT_EXPECT(exc_veh_resumes(-1) == 1);
    MT_EXPECT(exc_veh_resumes(0) == 0);
}

static void frame_resumes_only_on_continue_execution(void)
{
    MT_EXPECT(exc_frame_resumes(ExceptionContinueExecution) == 1);
    MT_EXPECT(exc_frame_resumes(ExceptionContinueSearch) == 0);
    MT_EXPECT(exc_frame_resumes(ExceptionNestedException) == 0);
    MT_EXPECT(exc_frame_resumes(ExceptionCollidedUnwind) == 0);
}

static void nesting_limit_is_sixteen(void)
{
    MT_EXPECT(exc_depth_exceeded(0) == 0);
    MT_EXPECT(exc_depth_exceeded(EXC_MAX_NESTED_DISPATCH) == 0);
    MT_EXPECT(exc_depth_exceeded(EXC_MAX_NESTED_DISPATCH + 1) == 1);
}

static void walk_ends_on_zero_return_or_stack(void)
{
    MT_EXPECT(exc_walk_ended(0, 0x7000) == 1);
    MT_EXPECT(exc_walk_ended(0x140001000ULL, 0) == 1);
    MT_EXPECT(exc_walk_ended(0x140001000ULL, 0x7000) == 0);
}

/* ---- dispatch ordering ------------------------------------------------ */

/* Insertion order decides the call order: First=1 goes to the front. The
 * handlers return EXCEPTION_CONTINUE_SEARCH, so every one runs. */
static void veh_handlers_run_in_list_order(void)
{
    log_reset();
    PVOID ha = AddVectoredExceptionHandler(0, veh_a);
    PVOID hb = AddVectoredExceptionHandler(1, veh_b);
    PVOID hc = AddVectoredExceptionHandler(0, veh_c);
    MT_ASSERT(ha && hb && hc);

    EXCEPTION_DISPOSITION d = ExceptionContinueExecution;
    dispatch_leaf(&d);
    MT_EXPECT(d == ExceptionContinueSearch);
    MT_EXPECT(strcmp(g_log, "bac") == 0);

    MT_EXPECT(RemoveVectoredExceptionHandler(ha) == 1);
    MT_EXPECT(RemoveVectoredExceptionHandler(hb) == 1);
    MT_EXPECT(RemoveVectoredExceptionHandler(hc) == 1);
}

/* A vectored handler that resumes stops the list: handlers after it are not
 * called. */
static void veh_resume_stops_the_list(void)
{
    log_reset();
    PVOID hr = AddVectoredExceptionHandler(1, veh_resume);
    PVOID ha = AddVectoredExceptionHandler(0, veh_a);
    MT_ASSERT(hr && ha);

    EXCEPTION_DISPOSITION d = ExceptionContinueSearch;
    dispatch_leaf(&d);
    MT_EXPECT(d == ExceptionContinueExecution);
    MT_EXPECT(strcmp(g_log, "R") == 0);

    MT_EXPECT(RemoveVectoredExceptionHandler(hr) == 1);
    MT_EXPECT(RemoveVectoredExceptionHandler(ha) == 1);
}

/* Failing first (EXC-8): a vectored handler that resumes execution must
 * still run the vectored continue handlers, as it does when a frame handler
 * resumes. Until it does, the continue handler is skipped on this path. */
static void vch_runs_when_veh_resumes(void)
{
    log_reset();
    PVOID hv = AddVectoredExceptionHandler(1, veh_resume);
    PVOID hc = AddVectoredContinueHandler(1, vch_mark);
    MT_ASSERT(hv && hc);

    EXCEPTION_DISPOSITION d = ExceptionContinueSearch;
    dispatch_leaf(&d);
    MT_EXPECT(d == ExceptionContinueExecution);
    MT_EXPECT(strcmp(g_log, "RV") == 0);

    MT_EXPECT(RemoveVectoredContinueHandler(hc) == 1);
    MT_EXPECT(RemoveVectoredExceptionHandler(hv) == 1);
}

static const mt_case_t cases[] = {
    { "list_insert_first_prepends", list_insert_first_prepends },
    { "list_insert_last_appends", list_insert_last_appends },
    { "list_remove_head_middle_tail", list_remove_head_middle_tail },
    { "list_remove_missing_handle_changes_nothing", list_remove_missing_handle_changes_nothing },
    { "veh_resumes_only_on_continue_execution", veh_resumes_only_on_continue_execution },
    { "frame_resumes_only_on_continue_execution", frame_resumes_only_on_continue_execution },
    { "nesting_limit_is_sixteen", nesting_limit_is_sixteen },
    { "walk_ends_on_zero_return_or_stack", walk_ends_on_zero_return_or_stack },
    { "veh_handlers_run_in_list_order", veh_handlers_run_in_list_order },
    { "veh_resume_stops_the_list", veh_resume_stops_the_list },
    { "vch_runs_when_veh_resumes", vch_runs_when_veh_resumes },
};

int main(void)
{
    return mt_run("except_pure", cases, sizeof(cases) / sizeof(cases[0]));
}
