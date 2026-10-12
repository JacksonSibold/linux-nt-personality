/*
 * linux/fs.h - syntax-check stub. VFS types and helpers used by the loader
 * and debugfs call sites. Only the members the sources touch are declared.
 */
#ifndef SYNTAX_SHIM_LINUX_FS_H
#define SYNTAX_SHIM_LINUX_FS_H

#include <stddef.h>
#include <linux/types.h>
#include <linux/compiler.h>

struct module;
struct inode {
	loff_t i_size;
	umode_t i_mode;
};

#ifndef KSHIM_H
struct file {
	struct inode *f_inode;
};
#endif

struct file_operations {
	struct module *owner;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
};

loff_t i_size_read(const struct inode *inode);
unsigned int i_uid_read(const struct inode *inode);
struct inode *file_inode(const struct file *f);
ssize_t kernel_read(struct file *file, void *buf, size_t count, loff_t *pos);
struct file *filp_open(const char *filename, int flags, umode_t mode);
int filp_close(struct file *filp, void *id);

#endif
