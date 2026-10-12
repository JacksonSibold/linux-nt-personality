/* linux/debugfs.h - syntax-check stub. debugfs API. */
#ifndef SYNTAX_SHIM_LINUX_DEBUGFS_H
#define SYNTAX_SHIM_LINUX_DEBUGFS_H

#include <linux/types.h>
#include <linux/compiler.h>

struct dentry;
struct file_operations;

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
struct dentry *debugfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops);
struct dentry *debugfs_lookup(const char *name, struct dentry *parent);

#endif
