/*
 * linux/binfmts.h - syntax-check stub. Executable-format registration and
 * the exec sequence the loader follows. Guarded where src/kernel/fs/kshim.h
 * defines the same names (KSHIM_H).
 */
#ifndef SYNTAX_SHIM_LINUX_BINFMTS_H
#define SYNTAX_SHIM_LINUX_BINFMTS_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/cred.h>

#ifndef KSHIM_H
#define BINPRM_BUF_SIZE 256
#endif

#define EXSTACK_DEFAULT 0

#ifndef KSHIM_H
struct linux_binprm {
	char buf[BINPRM_BUF_SIZE];
	struct mm_struct *mm;
	struct file *file;
	struct cred *cred;
	const char *filename;
	unsigned long p;
};
#endif /* KSHIM_H */

struct linux_binfmt {
	struct module *module;
	int (*load_binary)(struct linux_binprm *);
};

int register_binfmt(struct linux_binfmt *fmt);
void unregister_binfmt(struct linux_binfmt *fmt);
void set_binfmt(struct linux_binfmt *new);
int begin_new_exec(struct linux_binprm *bprm);
void setup_new_exec(struct linux_binprm *bprm);
int setup_arg_pages(struct linux_binprm *bprm, unsigned long stack_top,
		    int executable_stack);
void finalize_exec(struct linux_binprm *bprm);

#endif
