/*
 * asm/processor.h - syntax-check stub. x86-64 task and stack limits and the
 * start_thread() entry hook. Values follow mainline x86-64 where they are
 * simple constants.
 */
#ifndef SYNTAX_SHIM_ASM_PROCESSOR_H
#define SYNTAX_SHIM_ASM_PROCESSOR_H

#include <linux/types.h>
#include <asm/ptrace.h>

struct task_struct;

#define TASK_SIZE_MAX ((1UL << 47) - 4096UL)
#define STACK_TOP TASK_SIZE_MAX

void start_thread(struct pt_regs *regs, unsigned long new_ip, unsigned long new_sp);
struct pt_regs *task_pt_regs(struct task_struct *tsk);

#endif
