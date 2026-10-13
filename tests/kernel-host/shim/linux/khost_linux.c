/* khost_linux.c - the only host shim unit on Linux (tests/kernel-host, KHOST_OS=linux).
 *
 * The Windows shim (../posix_host.c) emulates POSIX file calls on UCRT. On Linux
 * the libc provides them, so the only thing left is the scratch-root cleanup that
 * the test programs arm: the root is removed at exit, after a pass or a failure,
 * unless KEEP_SCRATCH=1. Same contract as posix_host.c.
 */
#define _GNU_SOURCE 1
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KHOST_PATH_MAX PATH_MAX

static char g_scratch_cleanup[KHOST_PATH_MAX];

static void scratch_cleanup_at_exit(void)
{
    const char *keep = getenv("KEEP_SCRATCH");
    char cmd[KHOST_PATH_MAX + 32];
    if (!g_scratch_cleanup[0])
        return;
    if (keep && strcmp(keep, "1") == 0) {
        fprintf(stderr, "khost: KEEP_SCRATCH=1, keeping %s\n", g_scratch_cleanup);
        return;
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_scratch_cleanup);
    if (system(cmd) == -1)
        fprintf(stderr, "khost: cleanup of %s failed\n", g_scratch_cleanup);
}

void khost_scratch_arm_cleanup(const char *posix_root)
{
    static int registered;
    if (!posix_root || !*posix_root)
        return;
    snprintf(g_scratch_cleanup, sizeof(g_scratch_cleanup), "%s", posix_root);
    if (!registered) {
        registered = 1;
        atexit(scratch_cleanup_at_exit);
    }
}
