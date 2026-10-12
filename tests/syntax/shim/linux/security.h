/* linux/security.h - syntax-check stub. LSM hooks used by the NT VM code. */
#ifndef SYNTAX_SHIM_LINUX_SECURITY_H
#define SYNTAX_SHIM_LINUX_SECURITY_H

struct vm_area_struct;

int security_file_mprotect(struct vm_area_struct *vma, unsigned long reqprot,
			   unsigned long prot);

#endif
