/* linux/seq_file.h - syntax-check stub. seq_file API. */
#ifndef SYNTAX_SHIM_LINUX_SEQ_FILE_H
#define SYNTAX_SHIM_LINUX_SEQ_FILE_H

#include <stddef.h>
#include <linux/types.h>

struct file;
struct seq_file;
struct inode;

int seq_printf(struct seq_file *m, const char *fmt, ...);
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
loff_t seq_lseek(struct file *file, loff_t offset, int whence);
int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data);
int single_release(struct inode *inode, struct file *file);

#endif
