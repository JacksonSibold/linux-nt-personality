/*
 * asm/ptrace.h - syntax-check stub. x86-64 struct pt_regs, field order and
 * names as in the mainline arch/x86/include/asm/ptrace.h.
 */
#ifndef SYNTAX_SHIM_ASM_PTRACE_H
#define SYNTAX_SHIM_ASM_PTRACE_H

#include <linux/types.h>

#ifndef KSHIM_H
struct pt_regs {
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long bp;
	unsigned long bx;
	unsigned long r11;
	unsigned long r10;
	unsigned long r9;
	unsigned long r8;
	unsigned long ax;
	unsigned long cx;
	unsigned long dx;
	unsigned long si;
	unsigned long di;
	unsigned long orig_ax;
	unsigned long ip;
	unsigned long cs;
	unsigned long flags;
	unsigned long sp;
	unsigned long ss;
};
#endif

#endif
