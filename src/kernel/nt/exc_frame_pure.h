/*
 * exc_frame_pure.h - pure helpers for NT user-mode exception delivery.
 *
 * No kernel and no libc: only stdint types, so tests/kernel-host can check the
 * arithmetic on the host. Used by src/kernel/nt/proc_thread.c, which owns the
 * copy between pt_regs and CONTEXT.
 *
 * Delivery model (x64, Windows ABI):
 *   a fault or trap taken in user mode by an NT task is turned into an
 *   EXCEPTION_RECORD and a CONTEXT, both written to the user stack, and the
 *   thread resumes at the ntdll export KiUserExceptionDispatcher with
 *   RCX = &record and RDX = &context. The dispatcher calls RtlDispatchException
 *   and then NtContinue(&context), which restores the registers from the
 *   CONTEXT (proc_thread.c). Nothing here touches the kernel.
 */
#ifndef EXC_FRAME_PURE_H
#define EXC_FRAME_PURE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Returned by NtContinue and NtRaiseException (kernel branch) after they have
 * rewritten the calling task's trap frame. do_nt_syscall keeps the RAX the call
 * set and does not store this value. Chosen so no service returns it. */
#define NT_CONTINUE_RESUMED ((uint32_t)0x7FFF0001u)

/* NTSTATUS codes the kernel delivers. Values match src/ntdll/ntstatus.h. */
#define EXC_PURE_STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094u
#define EXC_PURE_STATUS_INTEGER_OVERFLOW       0xC0000095u
#define EXC_PURE_STATUS_ILLEGAL_INSTRUCTION    0xC000001Du
#define EXC_PURE_STATUS_BREAKPOINT             0x80000003u
#define EXC_PURE_STATUS_ACCESS_VIOLATION       0xC0000005u

/* Linux x86 trap numbers (asm/trapnr.h) that have an NT equivalent here. */
#define EXC_PURE_TRAP_DE 0
#define EXC_PURE_TRAP_OF 4
#define EXC_PURE_TRAP_BP 3
#define EXC_PURE_TRAP_UD 6

/* Page-fault error-code bits (asm/traps.h, X86_PF_*). */
#define EXC_PURE_PF_WRITE 0x2ul
#define EXC_PURE_PF_INSTR 0x10ul

/* Bytes below the faulting stack pointer that the frame skips: the SysV red
 * zone. Windows code does not use one, but a leaf frame may, and skipping it
 * costs nothing. */
#define EXC_PURE_RED_ZONE 128u

/* Shadow space the Windows x64 ABI reserves above the return address. */
#define EXC_PURE_SHADOW 32u

/* EFLAGS bits a CONTEXT may change: CF PF AF ZF SF TF DF OF, plus IF. IF is
 * forced on when a context is installed. The reserved bit 1 is always set.
 * Everything else (IOPL, NT, RF, VM, AC, ID) keeps the value the thread already
 * has. */
#define EXC_PURE_EFLAGS_USER_MASK 0x0FD5ul
#define EXC_PURE_EFLAGS_FIXED     0x0002ul

static inline uint32_t exc_pure_status_for_trap(int trapnr)
{
    switch (trapnr) {
    case EXC_PURE_TRAP_DE: return EXC_PURE_STATUS_INTEGER_DIVIDE_BY_ZERO;
    case EXC_PURE_TRAP_OF: return EXC_PURE_STATUS_INTEGER_OVERFLOW;
    case EXC_PURE_TRAP_BP: return EXC_PURE_STATUS_BREAKPOINT;
    case EXC_PURE_TRAP_UD: return EXC_PURE_STATUS_ILLEGAL_INSTRUCTION;
    default:               return 0; /* not delivered as an NT exception */
    }
}

/* ExceptionInformation[0] for an access violation: 0 read, 1 write, 8 execute.
 * Matches the values src/ntdll/except.c reports on the user-mode path. */
static inline uint64_t exc_pure_access_type(unsigned long error_code)
{
    if (error_code & EXC_PURE_PF_INSTR) return 8;
    if (error_code & EXC_PURE_PF_WRITE) return 1;
    return 0;
}

/* Round n up to a multiple of 16. */
static inline uint64_t exc_pure_align16(uint64_t n)
{
    return (n + 15u) & ~(uint64_t)15u;
}

/* Layout of the dispatch frame on the user stack. The record and the context
 * sit below the red zone, 16-byte aligned (NtContinue requires an aligned
 * CONTEXT). new_sp is the stack pointer at KiUserExceptionDispatcher entry:
 * it holds a zero return address, and new_sp + 8 is the shadow space, so
 * new_sp is 8 mod 16 as the ABI requires.
 *
 * Returns 0 and fills the outputs, or -1 when sp is too small for the frame. */
struct exc_pure_frame {
    uint64_t rec_va;
    uint64_t ctx_va;
    uint64_t new_sp;
};

static inline int exc_pure_frame_layout(uint64_t sp, uint64_t rec_size,
                                        uint64_t ctx_size,
                                        struct exc_pure_frame *out)
{
    uint64_t base, ctx_va, rec_va, new_sp;
    uint64_t need = EXC_PURE_RED_ZONE + exc_pure_align16(ctx_size) +
                    exc_pure_align16(rec_size) + 16u + EXC_PURE_SHADOW + 16u;
    if (sp < need) return -1;
    base = (sp - EXC_PURE_RED_ZONE) & ~(uint64_t)15u;
    ctx_va = base - exc_pure_align16(ctx_size);
    rec_va = ctx_va - exc_pure_align16(rec_size);
    new_sp = rec_va - 8u - EXC_PURE_SHADOW;
    out->rec_va = rec_va;
    out->ctx_va = ctx_va;
    out->new_sp = new_sp;
    return 0;
}

/* EFLAGS to install from a CONTEXT, given the thread's current flags. */
static inline uint64_t exc_pure_eflags_merge(uint64_t context_flags, uint64_t current)
{
    uint64_t keep = current & ~EXC_PURE_EFLAGS_USER_MASK;
    return keep | (context_flags & EXC_PURE_EFLAGS_USER_MASK) | EXC_PURE_EFLAGS_FIXED | 0x200u;
}

#endif /* EXC_FRAME_PURE_H */
