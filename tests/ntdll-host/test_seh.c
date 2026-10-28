/*
 * test_seh.c - structured exception handling end to end on the host: RtlDispatchException, __C_specific_handler,
 * RtlUnwindEx and RtlRestoreContext run on hand-assembled functions that have real x64 unwind data and scope tables.
 *
 * The call chain is  seh_call -> seh_outer (a __try/__except frame) -> [seh_mid (a __try/__finally frame)] ->
 * seh_raiser -> seh_trigger (C), which builds the context of the raiser's call site and dispatches an exception.
 * When a scope accepts, control must arrive at seh_except_target with the outer frame's registers and stack, having run
 * the termination handlers of the frames in between. The code is the System V flavour of the same thing the DLLs run
 * with the Microsoft calling convention; the unwind data is identical.
 *
 * Built without the sanitizers (see the Makefile): the test leaves C frames by a jump, which AddressSanitizer would
 * otherwise leave poisoned.
 */
#include "minitest.h"
#include "except.h"
#include "unwind.h"
#include "ntstatus.h"

#include <stdint.h>
#include <string.h>

extern char __executable_start;
extern EXCEPTION_DISPOSITION __C_specific_handler(PEXCEPTION_RECORD, PVOID, PCONTEXT, PVOID);

/* ---- the functions under test, in assembly ------------------------------------------------------------------------------- */

volatile uint64_t seh_rsp_at_target, seh_rbp_at_target, seh_rax_at_target, seh_reached, seh_outer_frame, seh_mid_rsp;
volatile uint64_t seh_unwind_target_ip, seh_unwind_retval;
CONTEXT g_scratch_ctx;

extern char seh_outer[], seh_outer_p1[], seh_outer_p2[], seh_outer_p3[], seh_try_begin[], seh_try_end[], seh_except_target[],
    seh_outer_done[], seh_outer_end[];
extern char seh_mid[], seh_mid_p1[], seh_mid_try_begin[], seh_mid_try_end[], seh_mid_end[];
extern char seh_raiser[], seh_raiser_p1[], seh_raiser_end[];
extern char seh_raiser2[], seh_raiser2_p1[], seh_raiser2_end[];
extern void seh_call(void *fn, void *a, void *b);

__asm__(
    ".text\n"
    ".globl seh_call\n"
    "seh_call:\n"                       /* seh_call(fn, a, b): fn(a, b) with the callee-saved registers preserved */
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n  subq $8, %rsp\n"
    "  movq %rdi, %rax\n  movq %rsi, %rdi\n  movq %rdx, %rsi\n  callq *%rax\n"
    "  addq $8, %rsp\n  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n"
    ".balign 16\n"
    ".globl seh_outer\n"
    "seh_outer:\n"                      /* seh_outer(a, b): __try { a(b) } __except(...) { ... } */
    "  pushq %rbp\n"
    ".globl seh_outer_p1\nseh_outer_p1:\n"
    "  subq $0x20, %rsp\n"
    ".globl seh_outer_p2\nseh_outer_p2:\n"
    "  leaq (%rsp), %rbp\n"
    ".globl seh_outer_p3\nseh_outer_p3:\n"
    "  movq %rbp, seh_outer_frame(%rip)\n"
    "  movq %rdi, %rax\n  movq %rsi, %rdi\n"
    ".globl seh_try_begin\nseh_try_begin:\n"
    "  callq *%rax\n"
    "  nop\n"                          /* the compiler keeps a call's return address inside the scope */
    ".globl seh_try_end\nseh_try_end:\n"
    "  jmp seh_outer_done\n"
    ".globl seh_except_target\nseh_except_target:\n"
    "  movq %rsp, seh_rsp_at_target(%rip)\n"
    "  movq %rbp, seh_rbp_at_target(%rip)\n"
    "  movq %rax, seh_rax_at_target(%rip)\n"
    "  movq $1, seh_reached(%rip)\n"
    ".globl seh_outer_done\nseh_outer_done:\n"
    "  addq $0x20, %rsp\n  popq %rbp\n  ret\n"
    ".globl seh_outer_end\nseh_outer_end:\n"
    ".balign 16\n"
    ".globl seh_mid\n"
    "seh_mid:\n"                        /* seh_mid(a): __try { a() } __finally { ... } */
    "  subq $0x28, %rsp\n"
    ".globl seh_mid_p1\nseh_mid_p1:\n"
    "  movq %rsp, seh_mid_rsp(%rip)\n"
    ".globl seh_mid_try_begin\nseh_mid_try_begin:\n"
    "  callq *%rdi\n"
    "  nop\n"
    ".globl seh_mid_try_end\nseh_mid_try_end:\n"
    "  addq $0x28, %rsp\n  ret\n"
    ".globl seh_mid_end\nseh_mid_end:\n"
    ".balign 16\n"
    ".globl seh_raiser\n"
    "seh_raiser:\n"                     /* raises through seh_trigger */
    "  subq $0x28, %rsp\n"
    ".globl seh_raiser_p1\nseh_raiser_p1:\n"
    "  callq seh_trigger@PLT\n"
    "  addq $0x28, %rsp\n  ret\n"
    ".globl seh_raiser_end\nseh_raiser_end:\n"
    ".balign 16\n"
    ".globl seh_raiser2\n"
    "seh_raiser2:\n"                    /* unwinds to the outer frame with RtlUnwindEx, as longjmp-style code does */
    "  subq $0x28, %rsp\n"
    ".globl seh_raiser2_p1\nseh_raiser2_p1:\n"
    "  movq seh_outer_frame(%rip), %rdi\n"
    "  movq seh_unwind_target_ip(%rip), %rsi\n"
    "  xorl %edx, %edx\n"
    "  movq seh_unwind_retval(%rip), %rcx\n"
    "  leaq g_scratch_ctx(%rip), %r8\n"
    "  xorl %r9d, %r9d\n"
    "  callq RtlUnwindEx@PLT\n"
    "  addq $0x28, %rsp\n  ret\n"
    ".globl seh_raiser2_end\nseh_raiser2_end:\n"
    ".balign 16\n");

/* ---- observation points ------------------------------------------------------------------------------------------------------ */

static LONG g_filter_result;
static int g_filter_calls;
static ULONG64 g_filter_frame;
static DWORD g_filter_code;
static ULONG64 g_filter_ctx_rip;
static int g_finally_calls[4];
static ULONG64 g_finally_frame[4];
static unsigned char g_finally_abnormal[4];
static int g_trigger_returned;
static EXCEPTION_DISPOSITION g_dispatch_result;
static LONG g_unhandled_calls;

static LONG filter_fn(PEXCEPTION_POINTERS p, ULONG64 frame)
{
    g_filter_calls++;
    g_filter_frame = frame;
    g_filter_code = p->ExceptionRecord->ExceptionCode;
    g_filter_ctx_rip = p->ContextRecord->Rip;
    return g_filter_result;
}

static void finally_mid(unsigned char abnormal, ULONG64 frame)
{
    g_finally_calls[0]++;
    g_finally_frame[0] = frame;
    g_finally_abnormal[0] = abnormal;
}

static void finally_outer(unsigned char abnormal, ULONG64 frame)
{
    g_finally_calls[1]++;
    g_finally_frame[1] = frame;
    g_finally_abnormal[1] = abnormal;
}

static LONG unhandled(PEXCEPTION_POINTERS p)
{
    (void)p;
    g_unhandled_calls++;
    return 0;
}

/* Builds the context of the raiser's call site and dispatches an exception from it. */
__attribute__((noinline, no_sanitize_address)) void seh_trigger(void)
{
    uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
    CONTEXT ctx;
    EXCEPTION_RECORD rec;
    memset(&ctx, 0, sizeof ctx);
    memset(&rec, 0, sizeof rec);
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = fp[1];
    ctx.Rsp = (ULONG64)(uintptr_t)(fp + 2);
    ctx.Rbp = fp[0];
    rec.ExceptionCode = 0xE0000001u;
    rec.ExceptionAddress = (PVOID)(uintptr_t)ctx.Rip;
    g_dispatch_result = RtlDispatchException(&rec, &ctx);
    g_trigger_returned = 1;
}

/* ---- unwind data -------------------------------------------------------------------------------------------------------------- */

static uint8_t g_unw_outer[128] __attribute__((aligned(4)));
static uint8_t g_unw_mid[128] __attribute__((aligned(4)));
static uint8_t g_unw_raiser[16] __attribute__((aligned(4)));
static IMAGE_RUNTIME_FUNCTION_ENTRY g_tbl[4];

static ULONG rva(const void *p) { return (ULONG)((uintptr_t)p - (uintptr_t)&__executable_start); }

typedef struct scope {
    const void *begin, *end;
    ULONG64 handler; /* a function, or 1 */
    const void *jump;
} scope;

static ULONG handler_rva(const void *fn) { return rva(fn); }

static uint8_t *put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
    return p + 4;
}

/* An unwind info with the language handler and a scope table after the codes. codes: pairs {code offset, op | info << 4}. */
static void build_info(uint8_t *out, uint8_t prolog, uint8_t frame_reg, const uint8_t (*codes)[2], int ncodes, const scope *scopes,
                       int nscopes)
{
    uint8_t *p = out;
    int slots = (ncodes + 1) & ~1;
    *p++ = 1 | (3 << 3); /* version 1, EHANDLER | UHANDLER */
    *p++ = prolog;
    *p++ = (uint8_t)ncodes;
    *p++ = frame_reg; /* register, scaled offset 0 */
    for (int i = 0; i < slots; ++i) {
        *p++ = i < ncodes ? codes[i][0] : 0;
        *p++ = i < ncodes ? codes[i][1] : 0;
    }
    p = put_u32(p, handler_rva((const void *)__C_specific_handler));
    p = put_u32(p, (uint32_t)nscopes);
    for (int i = 0; i < nscopes; ++i) {
        p = put_u32(p, rva(scopes[i].begin));
        p = put_u32(p, rva(scopes[i].end));
        p = put_u32(p, scopes[i].handler == 1 ? 1u : handler_rva((const void *)(uintptr_t)scopes[i].handler));
        p = put_u32(p, scopes[i].jump ? rva(scopes[i].jump) : 0);
    }
}

static void register_image(const scope *outer_scopes, int n_outer, const scope *mid_scopes, int n_mid)
{
    const uint8_t outer_codes[3][2] = {
        {(uint8_t)(seh_outer_p3 - seh_outer), 0x03},                 /* SET_FPREG */
        {(uint8_t)(seh_outer_p2 - seh_outer), 0x32},                 /* ALLOC_SMALL 0x20 */
        {(uint8_t)(seh_outer_p1 - seh_outer), 0x50},                 /* PUSH_NONVOL rbp */
    };
    const uint8_t mid_codes[1][2] = {{(uint8_t)(seh_mid_p1 - seh_mid), 0x42}}; /* ALLOC_SMALL 0x28 */
    const uint8_t raiser_codes[1][2] = {{(uint8_t)(seh_raiser_p1 - seh_raiser), 0x42}};
    unwind_clear_modules();
    memset(g_unw_outer, 0, sizeof g_unw_outer);
    memset(g_unw_mid, 0, sizeof g_unw_mid);
    build_info(g_unw_outer, (uint8_t)(seh_outer_p3 - seh_outer), 5, outer_codes, 3, outer_scopes, n_outer);
    build_info(g_unw_mid, (uint8_t)(seh_mid_p1 - seh_mid), 0, mid_codes, 1, mid_scopes, n_mid);
    /* the raiser has no handler: flags 0 */
    g_unw_raiser[0] = 1;
    g_unw_raiser[1] = (uint8_t)(seh_raiser_p1 - seh_raiser);
    g_unw_raiser[2] = 1;
    g_unw_raiser[3] = 0;
    g_unw_raiser[4] = raiser_codes[0][0];
    g_unw_raiser[5] = raiser_codes[0][1];
    memset(g_tbl, 0, sizeof g_tbl);
    g_tbl[0].BeginAddress = rva(seh_outer);
    g_tbl[0].EndAddress = rva(seh_outer_end);
    g_tbl[0].DUMMYUNIONNAME.UnwindInfoAddress = rva(g_unw_outer);
    g_tbl[1].BeginAddress = rva(seh_mid);
    g_tbl[1].EndAddress = rva(seh_mid_end);
    g_tbl[1].DUMMYUNIONNAME.UnwindInfoAddress = rva(g_unw_mid);
    g_tbl[2].BeginAddress = rva(seh_raiser);
    g_tbl[2].EndAddress = rva(seh_raiser_end);
    g_tbl[2].DUMMYUNIONNAME.UnwindInfoAddress = rva(g_unw_raiser);
    /* raiser2 shares the raiser's shape */
    g_tbl[3].BeginAddress = rva(seh_raiser2);
    g_tbl[3].EndAddress = rva(seh_raiser2_end);
    g_tbl[3].DUMMYUNIONNAME.UnwindInfoAddress = rva(g_unw_raiser);
    unwind_register_module((ULONG64)(uintptr_t)&__executable_start, 0x40000000u, g_tbl, 4, (uint8_t *)&__executable_start);
}

static void reset(void)
{
    seh_rsp_at_target = seh_rbp_at_target = seh_rax_at_target = seh_reached = 0;
    g_filter_result = 1;
    g_filter_calls = 0;
    g_filter_frame = 0;
    g_filter_code = 0;
    g_filter_ctx_rip = 0;
    memset(g_finally_calls, 0, sizeof g_finally_calls);
    memset(g_finally_frame, 0, sizeof g_finally_frame);
    memset(g_finally_abnormal, 0, sizeof g_finally_abnormal);
    g_trigger_returned = 0;
    g_dispatch_result = (EXCEPTION_DISPOSITION)-1;
    g_unhandled_calls = 0;
    SetUnhandledExceptionFilter(unhandled);
}

static scope except_scope(ULONG64 handler)
{
    scope s = {seh_try_begin, seh_try_end, handler, seh_except_target};
    return s;
}

/* ---- tests ---------------------------------------------------------------------------------------------------------------------- */

static void constant_filter_accepts(void)
{
    scope o[1];
    o[0] = except_scope(1);
    reset();
    register_image(o, 1, NULL, 0);
    seh_call(seh_outer, seh_raiser, 0);
    MT_EXPECT(seh_reached == 1);
    MT_EXPECT(!g_trigger_returned);
    MT_EXPECT(seh_rsp_at_target == seh_rbp_at_target); /* the frame's stack and frame register as the prologue left them */
    MT_EXPECT(seh_rbp_at_target == seh_outer_frame);
    MT_EXPECT_EQ((long)seh_rax_at_target, (long)0xE0000001L); /* the exception code travels in Rax */
    MT_EXPECT_EQ(g_unhandled_calls, 0);
}

static void filter_function_sees_frame_and_record(void)
{
    scope o[1];
    o[0] = except_scope((ULONG64)(uintptr_t)filter_fn);
    reset();
    register_image(o, 1, NULL, 0);
    seh_call(seh_outer, seh_raiser, 0);
    MT_EXPECT(seh_reached == 1);
    MT_EXPECT_EQ(g_filter_calls, 1);
    MT_EXPECT(g_filter_frame == seh_outer_frame); /* the establisher is the base of the fixed frame */
    MT_EXPECT(g_filter_code == 0xE0000001u);
    /* the context the filter sees is the original one: inside the raiser, not the outer frame */
    MT_EXPECT(g_filter_ctx_rip >= (ULONG64)(uintptr_t)seh_raiser && g_filter_ctx_rip < (ULONG64)(uintptr_t)seh_raiser_end);
}

static void filter_continue_search_leaves_exception_unhandled(void)
{
    scope o[1];
    o[0] = except_scope((ULONG64)(uintptr_t)filter_fn);
    reset();
    g_filter_result = 0;
    register_image(o, 1, NULL, 0);
    seh_call(seh_outer, seh_raiser, 0);
    MT_EXPECT(seh_reached == 0);
    MT_EXPECT_EQ(g_filter_calls, 1);
    MT_EXPECT(g_trigger_returned);
    MT_EXPECT(g_dispatch_result == ExceptionContinueSearch);
    MT_EXPECT_EQ(g_unhandled_calls, 1);
}

static void filter_continue_execution_resumes(void)
{
    scope o[1];
    o[0] = except_scope((ULONG64)(uintptr_t)filter_fn);
    reset();
    g_filter_result = -1;
    register_image(o, 1, NULL, 0);
    seh_call(seh_outer, seh_raiser, 0);
    MT_EXPECT(seh_reached == 0);
    MT_EXPECT(g_trigger_returned);
    MT_EXPECT(g_dispatch_result == ExceptionContinueExecution);
}

static void finally_runs_in_the_frames_between(void)
{
    scope o[1], m[1];
    o[0] = except_scope(1);
    m[0].begin = seh_mid_try_begin;
    m[0].end = seh_mid_try_end;
    m[0].handler = (ULONG64)(uintptr_t)finally_mid;
    m[0].jump = NULL;
    reset();
    register_image(o, 1, m, 1);
    seh_call(seh_outer, seh_mid, seh_raiser);
    MT_EXPECT(seh_reached == 1);
    MT_EXPECT_EQ(g_finally_calls[0], 1);
    MT_EXPECT(g_finally_abnormal[0] == 1);
    MT_EXPECT(g_finally_frame[0] == seh_mid_rsp); /* the mid frame's establisher is its stack pointer after the prologue */
    MT_EXPECT(!g_trigger_returned);
}

static void finally_scope_in_the_target_frame_runs_when_it_excludes_the_target(void)
{
    scope o[2];
    o[0].begin = seh_try_begin;
    o[0].end = seh_try_end;
    o[0].handler = (ULONG64)(uintptr_t)finally_outer;
    o[0].jump = NULL;
    o[1] = except_scope(1);
    reset();
    register_image(o, 2, NULL, 0);
    seh_call(seh_outer, seh_raiser, 0);
    MT_EXPECT(seh_reached == 1);
    MT_EXPECT_EQ(g_finally_calls[1], 1); /* the __finally nested inside the __try runs before the __except body */
    MT_EXPECT(g_finally_frame[1] == seh_outer_frame);
}

static void finally_scope_holding_the_target_does_not_run(void)
{
    scope o[2];
    o[0].begin = seh_try_begin;
    o[0].end = seh_outer_done; /* covers the call and the __except body, so the unwind target lies inside it */
    o[0].handler = (ULONG64)(uintptr_t)finally_outer;
    o[0].jump = NULL;
    o[1] = except_scope(1);
    reset();
    register_image(o, 2, NULL, 0);
    seh_call(seh_outer, seh_raiser, 0);
    MT_EXPECT(seh_reached == 1);
    MT_EXPECT_EQ(g_finally_calls[1], 0);
}

static void rtl_unwind_ex_with_a_scratch_context(void)
{
    scope o[1], m[1];
    o[0] = except_scope(1);
    m[0].begin = seh_mid_try_begin;
    m[0].end = seh_mid_try_end;
    m[0].handler = (ULONG64)(uintptr_t)finally_mid;
    m[0].jump = NULL;
    reset();
    register_image(o, 1, m, 1);
    seh_unwind_target_ip = (uint64_t)(uintptr_t)seh_except_target;
    seh_unwind_retval = 0x1234;
    /* seh_outer stores its frame in seh_outer_frame before calling; the raiser unwinds to it */
    seh_call(seh_outer, seh_mid, seh_raiser2);
    MT_EXPECT(seh_reached == 1);
    MT_EXPECT_EQ((long)seh_rax_at_target, 0x1234L);
    MT_EXPECT_EQ(g_finally_calls[0], 1);
    MT_EXPECT(seh_rsp_at_target == seh_rbp_at_target);
}

static const mt_case_t cases[] = {
    MT_CASE(constant_filter_accepts), MT_CASE(filter_function_sees_frame_and_record),
    MT_CASE(filter_continue_search_leaves_exception_unhandled), MT_CASE(filter_continue_execution_resumes),
    MT_CASE(finally_runs_in_the_frames_between), MT_CASE(finally_scope_in_the_target_frame_runs_when_it_excludes_the_target),
    MT_CASE(finally_scope_holding_the_target_does_not_run), MT_CASE(rtl_unwind_ex_with_a_scratch_context),
};

int main(void)
{
    return MT_RUN("seh", cases);
}
