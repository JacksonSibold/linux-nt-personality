/* linux/rwsem.h - syntax-check stub. Reader/writer semaphore API. */
#ifndef SYNTAX_SHIM_LINUX_RWSEM_H
#define SYNTAX_SHIM_LINUX_RWSEM_H

struct rw_semaphore;

void init_rwsem(struct rw_semaphore *sem);
void down_read(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);

#endif
