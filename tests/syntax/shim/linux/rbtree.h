/*
 * linux/rbtree.h - syntax-check stub. Red-black tree API.
 * struct rb_node / struct rb_root and the inline rb_* helpers come from the
 * userspace models (src/kernel/nt/kshim.h, src/kernel/fs/kshim.h) and are
 * not redefined here. Only the out-of-line functions are declared.
 */
#ifndef SYNTAX_SHIM_LINUX_RBTREE_H
#define SYNTAX_SHIM_LINUX_RBTREE_H

#include <linux/types.h>

struct rb_node;
struct rb_root;

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);

#endif
