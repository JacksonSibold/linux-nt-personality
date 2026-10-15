// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include "nt_internal.h"
struct nt_thread;
#include "sync.h"

void __weak nt_handle_table_destroy(struct task_struct *tsk)
{
    (void)tsk;
}

void __weak nt_abandon_owned_mutants(struct nt_thread *thr)
{
    (void)thr;
}

/* Exception hooks called from arch/x86/kernel/traps.c and arch/x86/mm/fault.c. A strong
 * definition replaces these; the default declines the exception, so the kernel handles
 * it as before. Without them the build fails to link when the tree has the call sites
 * but no implementation. */
struct pt_regs;
bool __weak nt_exc_user_trap(struct pt_regs *regs, int trapnr)
{
    (void)regs;
    (void)trapnr;
    return false;
}

bool __weak nt_exc_user_fault(struct pt_regs *regs, unsigned long address, unsigned long error_code)
{
    (void)regs;
    (void)address;
    (void)error_code;
    return false;
}

char RtlUserThreadStart = 0;
