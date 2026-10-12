/*
 * test_unwind_pure.c - host tests for unwind code decoding (unwind_pure.h and
 * unwind_one_frame in unwind.c).
 *
 * Each UWOP_* case builds a one-function UNWIND_INFO in a host buffer, puts a
 * fake frame on a host stack, runs unwind_one_frame and checks the restored
 * RIP, RSP and nonvolatile registers against the x64 spec semantics:
 *  - CodeOffset is the offset after the instruction that did the operation,
 *    so an operation has run when ripOffset >= CodeOffset.
 *  - SAVE_NONVOL offsets are scaled by 8, SAVE_XMM128 by 16. The _FAR forms
 *    are unscaled 32-bit values stored low word first.
 *  - Unwinding undoes the codes in array order, which is last-executed first.
 */
#include "minitest.h"
#include "unwind.h"
#include "unwind_pure.h"

#include <stdint.h>
#include <string.h>

#define FAKE_BASE 0x140000000ULL
#define FN_BEGIN  0x1000u
#define FN_END    0x1100u
#define INFO_RVA  0x2000u
#define BODY_PC   (FAKE_BASE + FN_BEGIN + 0x80) /* past any prolog used here */

static uint8_t g_image[0x3000] __attribute__((aligned(16)));
static IMAGE_RUNTIME_FUNCTION_ENTRY g_fn;
static uint64_t g_stack[16] __attribute__((aligned(16)));
static uint64_t g_big[0x2010] __attribute__((aligned(16)));

/* UNWIND_CODE slot: byte 0 CodeOffset, byte 1 UnwindOp (low nibble) and
 * OpInfo (high nibble). */
static uint16_t uc(unsigned codeOffset, unsigned op, unsigned opinfo)
{
    return (uint16_t)(codeOffset | ((op | (opinfo << 4)) << 8));
}

/* Raw 16-bit operand slot (used for FrameOffset words). */
static uint16_t raw(unsigned v)
{
    return (uint16_t)v;
}

/* UNWIND_INFO header at image + off, then the slots. Version is 1. */
static void put_info(unsigned off, unsigned flags, unsigned prolog,
                     unsigned frameReg, unsigned frameOff,
                     const uint16_t *slots, unsigned n)
{
    uint8_t *p = g_image + off;
    p[0] = (uint8_t)(1 | (flags << 3));
    p[1] = (uint8_t)prolog;
    p[2] = (uint8_t)n;
    p[3] = (uint8_t)((frameReg & 0xF) | ((frameOff & 0xF) << 4));
    if (n)
        memcpy(p + 4, slots, n * sizeof(uint16_t));
}

static void fn_setup(void)
{
    memset(g_image, 0, sizeof(g_image));
    memset(&g_fn, 0, sizeof(g_fn));
    g_fn.BeginAddress = FN_BEGIN;
    g_fn.EndAddress = FN_END;
    g_fn.DUMMYUNIONNAME.UnwindInfoAddress = INFO_RVA;
    unwind_clear_modules();
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0x3000, &g_fn, 1, g_image) == 0);
}

static void fn_teardown(void)
{
    unwind_clear_modules();
}

static int run_frame(CONTEXT *c, uint64_t pc, ULONG64 *est)
{
    c->Rip = pc;
    return unwind_one_frame(c, FAKE_BASE, &g_fn, g_image, est);
}

static uint64_t addr_of(void *p)
{
    return (uint64_t)(uintptr_t)p;
}

/* ---- pure helpers ----------------------------------------------------- */

static void op_slots_follow_the_encoding(void)
{
    MT_EXPECT(uwp_op_slots(UWOP_PUSH_NONVOL, 0) == 1);
    MT_EXPECT(uwp_op_slots(UWOP_ALLOC_SMALL, 5) == 1);
    MT_EXPECT(uwp_op_slots(UWOP_SET_FPREG, 0) == 1);
    MT_EXPECT(uwp_op_slots(UWOP_PUSH_MACHFRAME, 1) == 1);
    MT_EXPECT(uwp_op_slots(UWOP_ALLOC_LARGE, 0) == 2);
    MT_EXPECT(uwp_op_slots(UWOP_ALLOC_LARGE, 1) == 3);
    MT_EXPECT(uwp_op_slots(UWOP_SAVE_NONVOL, 3) == 2);
    MT_EXPECT(uwp_op_slots(UWOP_SAVE_NONVOL_FAR, 3) == 3);
    MT_EXPECT(uwp_op_slots(UWOP_SAVE_XMM128, 6) == 2);
    MT_EXPECT(uwp_op_slots(UWOP_SAVE_XMM128_FAR, 6) == 3);
}

static void alloc_small_is_opinfo_times_8_plus_8(void)
{
    MT_EXPECT(uwp_alloc_small_size(0) == 8);
    MT_EXPECT(uwp_alloc_small_size(3) == 0x20);
    MT_EXPECT(uwp_alloc_small_size(15) == 0x80);
}

static void far_operand_is_low_word_first(void)
{
    MT_EXPECT(uwp_far_operand(0x3456, 0x0001) == 0x13456);
    MT_EXPECT(uwp_far_operand(0xFFFF, 0xFFFF) == 0xFFFFFFFFULL);
    MT_EXPECT(uwp_far_operand(0x0010, 0x0000) == 0x10);
}

static void alloc_large_slot_is_scaled_by_8(void)
{
    MT_EXPECT(uwp_alloc_large_size8(0x10) == 0x80);
}

static void save_offsets_are_scaled(void)
{
    MT_EXPECT(uwp_save_nonvol_offset(3) == 24);
    MT_EXPECT(uwp_save_xmm_offset(2) == 32);
}

static void frame_base_subtracts_scaled_frame_offset(void)
{
    MT_EXPECT(uwp_fp_base(0x8000, 0) == 0x8000);
    MT_EXPECT(uwp_fp_base(0x8000, 2) == 0x8000 - 32);
}

static void chain_slots_pad_to_even(void)
{
    MT_EXPECT(uwp_chain_slots(0) == 0);
    MT_EXPECT(uwp_chain_slots(1) == 2);
    MT_EXPECT(uwp_chain_slots(2) == 2);
    MT_EXPECT(uwp_chain_slots(3) == 4);
}

/* The prolog is [begin, begin + SizeOfProlog). The end is outside it. */
static void prolog_range_is_half_open(void)
{
    MT_EXPECT(uwp_in_prolog(0x1000, 0x1000, 8) == 1);
    MT_EXPECT(uwp_in_prolog(0x1007, 0x1000, 8) == 1);
    MT_EXPECT(uwp_in_prolog(0x1008, 0x1000, 8) == 0);
    MT_EXPECT(uwp_in_prolog(0x0FFF, 0x1000, 8) == 0);
    MT_EXPECT(uwp_in_prolog(0x1000, 0x1000, 0) == 0);
}

/* Codes are in descending CodeOffset order. The start index is the first one
 * that has completed; -1 means none has. */
static void prolog_start_is_first_completed_code(void)
{
    /* sub rsp,0x20 at 9; push rbx at 3 (descending array) */
    const UNWIND_CODE codes[2] = {
        { .DUMMYSTRUCTNAME = { .CodeOffset = 9, .UnwindOp = UWOP_ALLOC_SMALL, .OpInfo = 3 } },
        { .DUMMYSTRUCTNAME = { .CodeOffset = 3, .UnwindOp = UWOP_PUSH_NONVOL, .OpInfo = 3 } },
    };
    MT_EXPECT(uwp_prolog_start(codes, 2, 2) == -1);
    MT_EXPECT(uwp_prolog_start(codes, 2, 3) == 1);
    MT_EXPECT(uwp_prolog_start(codes, 2, 8) == 1);
    MT_EXPECT(uwp_prolog_start(codes, 2, 9) == 0);
}

/* An ALLOC_LARGE size slot whose bytes decode as UWOP_PUSH_MACHFRAME must not
 * be read as a code. The size slot is 0x0A00: low byte 0 (CodeOffset), high
 * byte 0x0A (UnwindOp 10 in the low nibble). */
static void operand_slots_are_not_read_as_codes(void)
{
    const uint16_t slots[3] = {
        uc(8, UWOP_ALLOC_LARGE, 0),
        raw(0x0A00),                     /* size / 8, looks like MACHFRAME */
        uc(1, UWOP_PUSH_NONVOL, 3),
    };
    UNWIND_CODE codes[3];
    memcpy(codes, slots, sizeof(slots));
    MT_EXPECT(uwp_has_machframe(codes, 3) == 0);
    MT_EXPECT(uwp_prolog_start(codes, 3, 1) == 2);

    const uint16_t mf[2] = { uc(4, UWOP_PUSH_MACHFRAME, 0), uc(0, 0, 0) };
    UNWIND_CODE mcodes[2];
    memcpy(mcodes, mf, sizeof(mf));
    MT_EXPECT(uwp_has_machframe(mcodes, 1) == 1);
}

static void machframe_layout_offsets(void)
{
    MT_EXPECT(uwp_machframe_size(0) == 40);
    MT_EXPECT(uwp_machframe_size(1) == 48);
    MT_EXPECT(uwp_machframe_rip_offset(0) == 0);
    MT_EXPECT(uwp_machframe_rsp_offset(0) == 24);
    MT_EXPECT(uwp_machframe_rip_offset(1) == 8);
    MT_EXPECT(uwp_machframe_rsp_offset(1) == 32);
}

/* sizeOfImage 0 bounds only by the 4 GiB RVA limit; a non-zero size bounds
 * the range. */
static void module_rva_range_checks(void)
{
    ULONG rva = 0;
    MT_EXPECT(uwp_module_rva(FAKE_BASE, 0, FAKE_BASE + 0x12345, &rva) == 1);
    MT_EXPECT(rva == 0x12345);
    MT_EXPECT(uwp_module_rva(FAKE_BASE, 0, FAKE_BASE + 0x1FFFFFFFFULL, &rva) == 0);
    MT_EXPECT(uwp_module_rva(FAKE_BASE, 0x3000, FAKE_BASE + 0x2FFF, &rva) == 1);
    MT_EXPECT(uwp_module_rva(FAKE_BASE, 0x3000, FAKE_BASE + 0x3000, &rva) == 0);
    MT_EXPECT(uwp_module_rva(FAKE_BASE, 0x3000, FAKE_BASE - 1, &rva) == 0);
}

static void function_search_is_bounded_by_table(void)
{
    IMAGE_RUNTIME_FUNCTION_ENTRY t[3];
    memset(t, 0, sizeof(t));
    t[0].BeginAddress = 0x1000; t[0].EndAddress = 0x1100;
    t[1].BeginAddress = 0x1200; t[1].EndAddress = 0x1300;
    t[2].BeginAddress = 0x1400; t[2].EndAddress = 0x1407;

    MT_EXPECT(uwp_find_function(t, 3, 0x1000) == 0);
    MT_EXPECT(uwp_find_function(t, 3, 0x10FF) == 0);
    MT_EXPECT(uwp_find_function(t, 3, 0x1100) == -1);
    MT_EXPECT(uwp_find_function(t, 3, 0x1250) == 1);
    MT_EXPECT(uwp_find_function(t, 3, 0x1406) == 2);
    MT_EXPECT(uwp_find_function(t, 3, 0x1407) == -1);
    MT_EXPECT(uwp_find_function(t, 0, 0x1000) == -1);
}

/* ---- UWOP execution through unwind_one_frame --------------------------- */

/* PUSH_NONVOL: pops the saved register from [RSP] and the return address. */
static void uwop_push_nonvol_restores_register(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 2, 0, 0, (const uint16_t[]){ uc(1, UWOP_PUSH_NONVOL, 3) }, 1);
    g_stack[0] = 0xB0B0; g_stack[1] = 0x1234;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]); c.Rbx = 0;
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rbx == 0xB0B0);
    MT_EXPECT(c.Rip == 0x1234);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[2]));
    fn_teardown();
}

static void uwop_alloc_small_adds_fixed_size(void)
{
    fn_setup();
    /* sub rsp,0x18 at offset 4: opinfo 2 gives 0x18 */
    put_info(INFO_RVA, 0, 5, 0, 0, (const uint16_t[]){ uc(4, UWOP_ALLOC_SMALL, 2) }, 1);
    g_stack[3] = 0x4321;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rip == 0x4321);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[4]));
    fn_teardown();
}

/* ALLOC_LARGE OpInfo 0: the next slot is size / 8. */
static void uwop_alloc_large_small_form_scales_slot(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 8, 0, 0,
             (const uint16_t[]){ uc(8, UWOP_ALLOC_LARGE, 0), raw(8) }, 2);
    g_stack[8] = 0x5555;   /* 8 * 8 = 64 bytes = 8 slots above RSP */

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rip == 0x5555);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[9]));
    fn_teardown();
}

/* ALLOC_LARGE OpInfo 1: two slots, unscaled 32-bit, low word first.
 * 0x10020 bytes = hi 0x0001, lo 0x0020. */
static void uwop_alloc_large_far_form_is_unscaled(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 8, 0, 0,
             (const uint16_t[]){ uc(8, UWOP_ALLOC_LARGE, 1), raw(0x0020), raw(0x0001) }, 3);
    uint64_t ret_slot = 0x10020 / 8;
    g_big[ret_slot] = 0x7777;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_big[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rip == 0x7777);
    MT_EXPECT(c.Rsp == addr_of(&g_big[ret_slot + 1]));
    fn_teardown();
}

/* SET_FPREG: RSP = FrameReg - FrameOffset*16. The saved RBP sits at the frame
 * base, and the return address is above it. */
static void uwop_set_fpreg_restores_rsp_from_frame_register(void)
{
    fn_setup();
    /* push rbp (1); mov rbp,rsp (4, FrameReg 5 offset 0); sub rsp,0x20 (8) */
    put_info(INFO_RVA, 0, 9, 5, 0,
             (const uint16_t[]){ uc(8, UWOP_ALLOC_SMALL, 3),
                                 uc(4, UWOP_SET_FPREG, 0),
                                 uc(1, UWOP_PUSH_NONVOL, 5) }, 3);
    g_stack[4] = 0xBEEF;   /* saved RBP at the frame base */
    g_stack[5] = 0x1234;   /* return address */

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rbp = addr_of(&g_stack[4]);
    c.Rsp = addr_of(&g_stack[4]) - 0x20;   /* body RSP after sub rsp,0x20 */
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rbp == 0xBEEF);
    MT_EXPECT(c.Rip == 0x1234);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[6]));
    fn_teardown();
}

/* SAVE_NONVOL: scaled by 8. RBX is stored at RSP + 2*8 in the body. */
static void uwop_save_nonvol_offset_is_scaled_by_8(void)
{
    fn_setup();
    /* sub rsp,0x18 (4); mov [rsp+0x10],rbx (8) */
    put_info(INFO_RVA, 0, 9, 0, 0,
             (const uint16_t[]){ uc(8, UWOP_SAVE_NONVOL, 3), raw(2),
                                 uc(4, UWOP_ALLOC_SMALL, 2) }, 3);
    g_stack[2] = 0xAAAA;   /* saved RBX at RSP+0x10 */
    g_stack[3] = 0x9999;   /* return address above the 0x18 allocation */

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rbx == 0xAAAA);
    MT_EXPECT(c.Rip == 0x9999);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[4]));
    fn_teardown();
}

/* SAVE_NONVOL_FAR: the 32-bit offset is unscaled. Offset 0x10 must read
 * RSP+0x10, not RSP+0x80 as a scaled slot value of 0x10 would. */
static void uwop_save_nonvol_far_offset_is_unscaled(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 9, 0, 0,
             (const uint16_t[]){ uc(8, UWOP_SAVE_NONVOL_FAR, 3), raw(0x0010), raw(0),
                                 uc(4, UWOP_ALLOC_SMALL, 2) }, 4);
    g_stack[2] = 0xCCCC;
    g_stack[3] = 0x8888;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rbx == 0xCCCC);
    MT_EXPECT(c.Rip == 0x8888);
    fn_teardown();
}

/* SAVE_XMM128: scaled by 16. XMM6 is stored at RSP + 1*16. */
static void uwop_save_xmm128_offset_is_scaled_by_16(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 9, 0, 0,
             (const uint16_t[]){ uc(8, UWOP_SAVE_XMM128, 6), raw(1),
                                 uc(4, UWOP_ALLOC_SMALL, 3) }, 3);
    uint8_t xmm[16];
    for (int i = 0; i < 16; i++) xmm[i] = (uint8_t)(0xA0 + i);
    memcpy(&g_stack[2], xmm, 16);
    g_stack[4] = 0x6666;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(memcmp(&c.DUMMYUNIONNAME.FltSave.XmmRegisters[6], xmm, 16) == 0);
    MT_EXPECT(c.Rip == 0x6666);
    fn_teardown();
}

/* SAVE_XMM128_FAR: unscaled 32-bit offset, here 0x20 (not 0x20*16). */
static void uwop_save_xmm128_far_offset_is_unscaled(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 9, 0, 0,
             (const uint16_t[]){ uc(8, UWOP_SAVE_XMM128_FAR, 7), raw(0x0020), raw(0),
                                 uc(4, UWOP_ALLOC_SMALL, 2) }, 4);
    uint8_t xmm[16];
    for (int i = 0; i < 16; i++) xmm[i] = (uint8_t)(0x40 + i);
    memcpy((uint8_t *)g_stack + 0x20, xmm, 16);
    g_stack[3] = 0x5151;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(memcmp(&c.DUMMYUNIONNAME.FltSave.XmmRegisters[7], xmm, 16) == 0);
    MT_EXPECT(c.Rip == 0x5151);
    fn_teardown();
}

/* A save with a frame register uses FrameReg - FrameOffset*16 as its base.
 * With FrameOffset 1, SET_FPREG also sets RSP to RBP - 16, so the frame is:
 *   RSP at SET_FPREG = RBP - 16 = &s[2]: saved RBP (PUSH_NONVOL) at s[2],
 *                                         return address at s[3]
 *   SAVE_NONVOL base = RBP - 16 = &s[2], offset 2*8: saved RBX at s[4] = RBP */
static void uwop_save_uses_frame_register_base(void)
{
    fn_setup();
    /* push rbp (1); mov rbp,rsp (4, FrameReg 5, FrameOffset 1); mov [base+16],rbx (8) */
    put_info(INFO_RVA, 0, 9, 5, 1,
             (const uint16_t[]){ uc(8, UWOP_SAVE_NONVOL, 3), raw(2),
                                 uc(4, UWOP_SET_FPREG, 0),
                                 uc(1, UWOP_PUSH_NONVOL, 5) }, 4);
    uint64_t fp = addr_of(&g_stack[4]);
    g_stack[2] = 0xFACE;   /* saved RBP */
    g_stack[3] = 0x7070;   /* return address */
    g_stack[4] = 0xDDDD;   /* saved RBX, at base + 16 */

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rbp = fp;
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rbx == 0xDDDD);
    MT_EXPECT(c.Rbp == 0xFACE);
    MT_EXPECT(c.Rip == 0x7070);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[4]));
    fn_teardown();
}

/* PUSH_MACHFRAME without an error code: [RIP][CS][EFLAGS][RSP][SS]. The
 * return address is not popped again. */
static void uwop_push_machframe_restores_rip_and_rsp(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 0, 0, 0, (const uint16_t[]){ uc(0, UWOP_PUSH_MACHFRAME, 0) }, 1);
    g_stack[0] = 0x3300;   /* RIP */
    g_stack[3] = 0x7FF000; /* old RSP */

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rip == 0x3300);
    MT_EXPECT(c.Rsp == 0x7FF000);
    fn_teardown();
}

/* With an error code the layout is [error][RIP][CS][EFLAGS][RSP][SS]. */
static void uwop_push_machframe_with_error_code(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 0, 0, 0, (const uint16_t[]){ uc(0, UWOP_PUSH_MACHFRAME, 1) }, 1);
    g_stack[0] = 0xE77;    /* error code */
    g_stack[1] = 0x4400;   /* RIP */
    g_stack[4] = 0x7FE000; /* old RSP */

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rip == 0x4400);
    MT_EXPECT(c.Rsp == 0x7FE000);
    fn_teardown();
}

/* Mid-prologue: pc before the first operation has run. Nothing is undone and
 * the return address is popped from RSP. */
static void prolog_start_pops_only_return_address(void)
{
    fn_setup();
    /* push rbx (1); sub rsp,0x20 (5); prolog size 5 */
    put_info(INFO_RVA, 0, 5, 0, 0,
             (const uint16_t[]){ uc(5, UWOP_ALLOC_SMALL, 3), uc(1, UWOP_PUSH_NONVOL, 3) }, 2);
    g_stack[0] = 0xBADD;
    g_stack[1] = 0x1111;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]); c.Rbx = 0x55;
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, FAKE_BASE + FN_BEGIN, &est) == 0);
    MT_EXPECT(c.Rbx == 0x55);
    MT_EXPECT(c.Rip == 0xBADD);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[1]));
    fn_teardown();
}

/* Mid-prologue after the push but before the sub: only the push is undone. */
static void prolog_partial_undoes_completed_codes_only(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 5, 0, 0,
             (const uint16_t[]){ uc(5, UWOP_ALLOC_SMALL, 3), uc(1, UWOP_PUSH_NONVOL, 3) }, 2);
    g_stack[0] = 0xCAFE;   /* saved RBX, at RSP because sub has not run */
    g_stack[1] = 0x2222;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, FAKE_BASE + FN_BEGIN + 2, &est) == 0);
    MT_EXPECT(c.Rbx == 0xCAFE);
    MT_EXPECT(c.Rip == 0x2222);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[2]));
    fn_teardown();
}

/* At the end of the prolog every code has run, so the body rule applies. */
static void prolog_end_undoes_all_codes(void)
{
    fn_setup();
    put_info(INFO_RVA, 0, 5, 0, 0,
             (const uint16_t[]){ uc(5, UWOP_ALLOC_SMALL, 3), uc(1, UWOP_PUSH_NONVOL, 3) }, 2);
    g_stack[4] = 0xFEED;   /* saved RBX above the 0x20 allocation */
    g_stack[5] = 0x3131;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, FAKE_BASE + FN_BEGIN + 5, &est) == 0);
    MT_EXPECT(c.Rbx == 0xFEED);
    MT_EXPECT(c.Rip == 0x3131);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[6]));
    fn_teardown();
}

/* A chained entry (UNW_FLAG_CHAININFO) with no codes of its own. The chained
 * RUNTIME_FUNCTION follows the padded code array and names the primary info.
 * The primary's codes must still be applied. */
static void chained_info_applies_primary_codes(void)
{
    fn_setup();
    /* Entry info: CHAININFO, zero codes, chained RF at +4 -> primary info. */
    put_info(INFO_RVA, UNW_FLAG_CHAININFO, 0, 0, 0, NULL, 0);
    IMAGE_RUNTIME_FUNCTION_ENTRY chained = { FN_BEGIN, FN_END, { 0x2100 } };
    memcpy(g_image + INFO_RVA + 4, &chained, sizeof(chained));
    /* Primary info at 0x2100: push rbx at offset 1. */
    put_info(0x2100, 0, 2, 0, 0, (const uint16_t[]){ uc(1, UWOP_PUSH_NONVOL, 3) }, 1);
    g_stack[0] = 0xC4A1;
    g_stack[1] = 0x6161;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rbx == 0xC4A1);
    MT_EXPECT(c.Rip == 0x6161);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[2]));
    fn_teardown();
}

/* A chain that loops back to itself must stop at MAX_CHAIN and return. */
static void chained_info_cycle_is_bounded(void)
{
    fn_setup();
    put_info(INFO_RVA, UNW_FLAG_CHAININFO, 0, 0, 0, NULL, 0);
    IMAGE_RUNTIME_FUNCTION_ENTRY self = { FN_BEGIN, FN_END, { INFO_RVA } };
    memcpy(g_image + INFO_RVA + 4, &self, sizeof(self));
    g_stack[0] = 0x9090;
    g_stack[1] = 0x5C5C;

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.Rsp = addr_of(&g_stack[0]);
    ULONG64 est = 0;
    MT_EXPECT(run_frame(&c, BODY_PC, &est) == 0);
    MT_EXPECT(c.Rip == 0x9090);
    MT_EXPECT(c.Rsp == addr_of(&g_stack[1]));
    fn_teardown();
}

static const mt_case_t cases[] = {
    { "op_slots_follow_the_encoding", op_slots_follow_the_encoding },
    { "alloc_small_is_opinfo_times_8_plus_8", alloc_small_is_opinfo_times_8_plus_8 },
    { "far_operand_is_low_word_first", far_operand_is_low_word_first },
    { "alloc_large_slot_is_scaled_by_8", alloc_large_slot_is_scaled_by_8 },
    { "save_offsets_are_scaled", save_offsets_are_scaled },
    { "frame_base_subtracts_scaled_frame_offset", frame_base_subtracts_scaled_frame_offset },
    { "chain_slots_pad_to_even", chain_slots_pad_to_even },
    { "prolog_range_is_half_open", prolog_range_is_half_open },
    { "prolog_start_is_first_completed_code", prolog_start_is_first_completed_code },
    { "operand_slots_are_not_read_as_codes", operand_slots_are_not_read_as_codes },
    { "machframe_layout_offsets", machframe_layout_offsets },
    { "module_rva_range_checks", module_rva_range_checks },
    { "function_search_is_bounded_by_table", function_search_is_bounded_by_table },
    { "uwop_push_nonvol_restores_register", uwop_push_nonvol_restores_register },
    { "uwop_alloc_small_adds_fixed_size", uwop_alloc_small_adds_fixed_size },
    { "uwop_alloc_large_small_form_scales_slot", uwop_alloc_large_small_form_scales_slot },
    { "uwop_alloc_large_far_form_is_unscaled", uwop_alloc_large_far_form_is_unscaled },
    { "uwop_set_fpreg_restores_rsp_from_frame_register", uwop_set_fpreg_restores_rsp_from_frame_register },
    { "uwop_save_nonvol_offset_is_scaled_by_8", uwop_save_nonvol_offset_is_scaled_by_8 },
    { "uwop_save_nonvol_far_offset_is_unscaled", uwop_save_nonvol_far_offset_is_unscaled },
    { "uwop_save_xmm128_offset_is_scaled_by_16", uwop_save_xmm128_offset_is_scaled_by_16 },
    { "uwop_save_xmm128_far_offset_is_unscaled", uwop_save_xmm128_far_offset_is_unscaled },
    { "uwop_save_uses_frame_register_base", uwop_save_uses_frame_register_base },
    { "uwop_push_machframe_restores_rip_and_rsp", uwop_push_machframe_restores_rip_and_rsp },
    { "uwop_push_machframe_with_error_code", uwop_push_machframe_with_error_code },
    { "prolog_start_pops_only_return_address", prolog_start_pops_only_return_address },
    { "prolog_partial_undoes_completed_codes_only", prolog_partial_undoes_completed_codes_only },
    { "prolog_end_undoes_all_codes", prolog_end_undoes_all_codes },
    { "chained_info_applies_primary_codes", chained_info_applies_primary_codes },
    { "chained_info_cycle_is_bounded", chained_info_cycle_is_bounded },
};

int main(void)
{
    return mt_run("unwind_pure", cases, sizeof(cases) / sizeof(cases[0]));
}
