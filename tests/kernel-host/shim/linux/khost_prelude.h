/* khost_prelude.h (Linux) - force-included (-include) before every host-built
 * src/kernel/nt translation unit when the Makefile runs on Linux (KHOST_OS=linux).
 *
 * On Linux the kernel-side sources see the real glibc names (open, pread, mkdir,
 * rename, fsync, opendir, system and sys/mman.h), so this prelude maps nothing.
 * The Windows shim (../khost_prelude.h, posix_host.c, mman_win.c, posix_raw.c)
 * is not built here. It only declares the scratch-root hook that the test
 * programs call (implemented in khost_linux.c).
 */
#ifndef KHOST_PRELUDE_LINUX_H
#define KHOST_PRELUDE_LINUX_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1  /* loff_t, O_DIRECTORY, O_SYNC, MAP_FIXED_NOREPLACE, REG_* */
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

/* Test scratch roots: remove posix_root at exit, unless KEEP_SCRATCH=1.
 * Implemented in khost_linux.c. */
void khost_scratch_arm_cleanup(const char *posix_root);
#define KHOST_SCRATCH_CLEANUP 1

#endif
