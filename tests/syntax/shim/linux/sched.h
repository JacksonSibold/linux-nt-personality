/*
 * linux/sched.h - syntax-check stub (not a runtime model).
 * struct task_struct is opaque here except for the members the sources
 * touch. Guarded where src/kernel/fs/kshim.h defines the same struct.
 */
#ifndef SYNTAX_SHIM_LINUX_SCHED_H
#define SYNTAX_SHIM_LINUX_SCHED_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/thread_info.h>
#include <linux/preempt.h>
#include <asm/ptrace.h>

#ifndef KSHIM_H
struct mm_struct;

struct thread_struct {
	unsigned short gsindex;
	unsigned long gsbase;
};

struct task_struct {
	int pid;
	char comm[16];
	struct mm_struct *mm;
	unsigned long personality;
	struct thread_struct thread;
};
#endif /* KSHIM_H */

#ifndef KSHIM_H
#define current get_current()
#endif
struct task_struct *get_current(void);

#endif
