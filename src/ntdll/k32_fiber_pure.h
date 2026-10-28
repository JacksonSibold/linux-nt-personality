/*
 * k32_fiber_pure.h - the context switch behind fibers: k32_fiber_swap saves the callee-saved state of the running fiber on
 * its stack and continues another, and k32_fiber_prepare builds the first frame of a new fiber so that the first switch
 * to it enters a function on its fresh stack.
 *
 * Saved state: rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15, MXCSR and the x87 control word (the callee-saved set of the Microsoft
 * x64 convention, which is a superset of the System V one). The code is the same under both calling conventions; only the
 * argument registers differ. This header defines the assembly, so include it in one translation unit only.
 * Host tests: tests/ntdll-host/test_k32_fiber_pure.c.
 */
#ifndef K32_FIBER_PURE_H
#define K32_FIBER_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__MINGW32__)
#define KFB_A0 "%rcx"
#define KFB_A1 "%rdx"
#define KFB_FUNC(name) ".globl " #name "\n\t.def " #name "; .scl 2; .type 32; .endef\n" #name ":\n\t"
#define KFB_SHADOW "subq $0x20, %rsp\n\t"
#else
#define KFB_A0 "%rdi"
#define KFB_A1 "%rsi"
#define KFB_FUNC(name) ".globl " #name "\n\t.type " #name ",@function\n" #name ":\n\t"
#define KFB_SHADOW ""
#endif

/* k32_fiber_swap(uint64_t *save_sp, uint64_t new_sp): stores the current stack pointer at *save_sp, then runs on new_sp. */
void k32_fiber_swap(uint64_t *save_sp, uint64_t new_sp);
/* The address a new fiber's first frame returns to; it calls r12(r13) and never returns. */
void k32_fiber_start(void);

__asm__(".text\n\t"
        KFB_FUNC(k32_fiber_swap)
        "pushq %rbp\n\tpushq %rbx\n\tpushq %rdi\n\tpushq %rsi\n\tpushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
        "subq $0xB0, %rsp\n\t"
        "movups %xmm6, 0(%rsp)\n\tmovups %xmm7, 0x10(%rsp)\n\tmovups %xmm8, 0x20(%rsp)\n\tmovups %xmm9, 0x30(%rsp)\n\t"
        "movups %xmm10, 0x40(%rsp)\n\tmovups %xmm11, 0x50(%rsp)\n\tmovups %xmm12, 0x60(%rsp)\n\tmovups %xmm13, 0x70(%rsp)\n\t"
        "movups %xmm14, 0x80(%rsp)\n\tmovups %xmm15, 0x90(%rsp)\n\t"
        "stmxcsr 0xA0(%rsp)\n\tfnstcw 0xA4(%rsp)\n\t"
        "movq %rsp, (" KFB_A0 ")\n\t"
        "movq " KFB_A1 ", %rsp\n\t"
        "ldmxcsr 0xA0(%rsp)\n\tfldcw 0xA4(%rsp)\n\t"
        "movups 0(%rsp), %xmm6\n\tmovups 0x10(%rsp), %xmm7\n\tmovups 0x20(%rsp), %xmm8\n\tmovups 0x30(%rsp), %xmm9\n\t"
        "movups 0x40(%rsp), %xmm10\n\tmovups 0x50(%rsp), %xmm11\n\tmovups 0x60(%rsp), %xmm12\n\tmovups 0x70(%rsp), %xmm13\n\t"
        "movups 0x80(%rsp), %xmm14\n\tmovups 0x90(%rsp), %xmm15\n\t"
        "addq $0xB0, %rsp\n\t"
        "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\tpopq %rsi\n\tpopq %rdi\n\tpopq %rbx\n\tpopq %rbp\n\t"
        "ret\n\t");

__asm__(".text\n\t"
        KFB_FUNC(k32_fiber_start)
        "andq $-16, %rsp\n\t"
        KFB_SHADOW
        "movq %r13, " KFB_A0 "\n\t"
        "callq *%r12\n\t"
        "ud2\n\t");

#define KFB_FRAME_BYTES 0x100u

/* Builds the first frame of a fiber whose stack ends at stack_top (16-byte aligned): the first k32_fiber_swap to the
 * returned stack pointer calls entry(arg) on that stack. entry must not return. */
static inline uint64_t k32_fiber_prepare(uint8_t *stack_top, void (*entry)(void *), void *arg)
{
    uint8_t *sp = stack_top - KFB_FRAME_BYTES;
    uint64_t v;
    memset(sp, 0, KFB_FRAME_BYTES);
    {
        uint32_t mxcsr = 0x1F80u;
        uint16_t fpcw = 0x027Fu; /* 53-bit precision, as a Windows process starts */
        memcpy(sp + 0xA0, &mxcsr, 4);
        memcpy(sp + 0xA4, &fpcw, 2);
    }
    v = (uint64_t)(uintptr_t)arg;
    memcpy(sp + 0xC0, &v, 8); /* r13 */
    v = (uint64_t)(uintptr_t)entry;
    memcpy(sp + 0xC8, &v, 8); /* r12 */
    v = (uint64_t)(uintptr_t)&k32_fiber_start;
    memcpy(sp + 0xF0, &v, 8); /* the return address */
    return (uint64_t)(uintptr_t)sp;
}

#endif /* K32_FIBER_PURE_H */
