/* asm/mman.h - syntax-check stub. Architecture hooks for mmap protections. */
#ifndef SYNTAX_SHIM_ASM_MMAN_H
#define SYNTAX_SHIM_ASM_MMAN_H

#include <linux/types.h>

bool arch_validate_prot(unsigned long prot, unsigned long addr);
bool arch_validate_flags(unsigned long vm_flags);

#endif
