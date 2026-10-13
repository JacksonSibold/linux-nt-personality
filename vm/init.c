/*
 * init.c - minimal hand-rolled init for the ntpers VM test rig.
 *
 * Runs inside an initramfs.  Mounts the minimum virtual filesystems,
 * mounts the QEMU 9p virtfs share, executes the test script named on
 * the kernel command line, writes the script exit status back to the
 * share, and powers the VM off cleanly.
 *
 * With dodtransport=initrd-serial on the command line (VM_TRANSPORT=initrd-serial,
 * docs/vm/TRANSPORTS.md) there is no 9p mount: the payload is packed at /mnt/share in
 * the initramfs, and after the result is written the framed emitter
 * /emit-results.sh prints the .dodresult/ files on the serial console.
 *
 * This init deliberately does NOT depend on the host distro
 * initramfs-tools; it is compiled and packaged by vm/mkinitramfs.sh.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#define SHARE_MOUNT "/mnt/share"
#define RESULT_SUBDIR ".dodresult"
#define RESULT_FILE RESULT_SUBDIR "/result.exitcode"
#define DEFAULT_TEST "/mnt/share/vmtests/smoke.sh"
/* initrd-serial transport: framed result emitter, packed at the initramfs root. */
#define EMIT_SCRIPT "/emit-results.sh"

static void die(const char *msg)
{
    int saved = errno;
    fprintf(stderr, "init: %s: %s\n", msg, saved ? strerror(saved) : "ok");
    fflush(stderr);
    _exit(127);
}

static void write_sysrq(const char *cmd)
{
    int fd = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, cmd, strlen(cmd)) < 0) { /* ignore */ }
        close(fd);
    }
}

static void power_off(void)
{
    sync();
    /* Try the sysrq poweroff first, then the reboot syscall as fallback. */
    write_sysrq("o");
    sleep(1);
    (void)reboot(RB_POWER_OFF);
    /* If we are still alive, halt the CPU. */
    for (;;) pause();
}

static void mkdir_p(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return;
    die("mkdir failed");
}

static void do_mount(const char *src, const char *target,
                     const char *fstype, unsigned long flags,
                     const void *data)
{
    mkdir_p(target);
    if (mount(src, target, fstype, flags, data) != 0 && errno != EBUSY)
        die("mount failed");
}

static char *get_cmdline_value(const char *key, char *buf, size_t bufsz)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';

    size_t klen = strlen(key);
    char *p = buf;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            char *end = p;
            while (*end && *end != ' ' && *end != '\n')
                end++;
            *end = '\0';
            return p;
        }
        while (*p && *p != ' ')
            p++;
        while (*p == ' ')
            p++;
    }
    return NULL;
}

static void enable_dynamic_debug(void)
{
    // Re-enable pr_debug demoted binfmt_pe lines for diagnostic visibility.
    // This is safe even if kernel still uses pr_info; it just ensures
    // binfmt_pe: entry logs appear in serial/dmesg when dyndbg is available.
    const char *control = "/sys/kernel/debug/dynamic_debug/control";
    int fd = open(control, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const char *cmds[] = {
        "file binfmt_pe.c +p\n",
        "file nt_mm.c +p\n",
        "file dispatch.c +p\n",
        "file nt_services.c +p\n",
        NULL
    };
    for (int i=0; cmds[i]; i++) {
        ssize_t n = write(fd, cmds[i], strlen(cmds[i]));
        (void)n;
    }
    close(fd);
    // Also try module param style if control not present (kernel cmdline dyndbg handles it)
}

static int run_test(const char *path)
{
    pid_t pid = fork();
    if (pid < 0)
        die("fork failed");

    if (pid == 0) {
        /* Child: run the test script through /bin/sh so shell scripts work. */
        execl("/bin/sh", "sh", path, (char *)NULL);
        /* If the script is executable, try direct exec. */
        execl(path, path, (char *)NULL);
        die("exec test failed");
    }

    int status;
    if (waitpid(pid, &status, 0) < 0)
        die("waitpid failed");

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 126;
}

static void write_result(int code)
{
    mkdir_p(SHARE_MOUNT "/" RESULT_SUBDIR);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SHARE_MOUNT, RESULT_FILE);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "init: cannot write result file %s: %s\n",
                path, strerror(errno));
        return;
    }

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", code);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
    sync();
}

/* True when runvm.sh asked for VM_TRANSPORT=initrd-serial (dodtransport= on the cmdline). */
static int transport_is_initrd_serial(void)
{
    char buf[4096];
    const char *value = get_cmdline_value("dodtransport", buf, sizeof(buf));
    return value && strcmp(value, "initrd-serial") == 0;
}

/* initrd-serial: print the result files as framed base64 on the console.
 * The emitter is a shell script with busybox applets (vm/emit-results.sh).
 * Its stdout is inherited, so it lands on the serial log. */
static void run_emit(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: fork for emit failed\n");
        return;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", EMIT_SCRIPT, SHARE_MOUNT, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0)
        fprintf(stderr, "init: waitpid for emit failed\n");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Mount the bare minimum virtual filesystems. */
    do_mount("proc", "/proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);
    do_mount("sysfs", "/sys", "sysfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);
    do_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL);
    /* Debugfs/tracefs for ARCH ftrace instrumentation (non-fatal if not available) */
    mkdir_p("/sys/kernel/debug");
    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
    mkdir_p("/sys/kernel/tracing");
    mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL);
    /* Also try debugfs at /sys/kernel/debug/tracing for older kernels */
    if (access("/sys/kernel/debug/tracing", F_OK) == 0) {
        mkdir_p("/sys/kernel/debug/tracing");
        /* already via debugfs */
    }
    // Enable dynamic debug for binfmt_pe (pr_debug demoted) - must be after debugfs mount
    enable_dynamic_debug();

    int serial = transport_is_initrd_serial();
    mkdir_p(SHARE_MOUNT);
    if (serial) {
        /* initrd-serial: the payload is packed into the initramfs at /mnt/share
         * (vm/mkinitramfs.sh), so there is no 9p mount.  Results go to the same
         * .dodresult/ and are framed on the console by run_emit() below. */
        mkdir_p(SHARE_MOUNT "/" RESULT_SUBDIR);
    } else {
        /* The QEMU 9p virtfs share.  Tag must match runvm.sh. */
        if (mount("share", SHARE_MOUNT, "9p", 0,
                  "trans=virtio,version=9p2000.L") != 0) {
            die("failed to mount 9p share");
        }
    }
    // Try again after 9p mount (in case debugfs was delayed)
    enable_dynamic_debug();

    /* Locate the test script from the kernel command line. */
    char cmdline[4096];
    char *test_path = get_cmdline_value("dodtest", cmdline, sizeof(cmdline));
    if (!test_path || !*test_path)
        test_path = (char *)DEFAULT_TEST;

    printf("init: transport: %s\n", serial ? "initrd-serial" : "9p");
    printf("init: running test script: %s\n", test_path);
    fflush(stdout);

    int code = run_test(test_path);

    printf("init: test exited with code %d\n", code);
    fflush(stdout);

    write_result(code);

    if (serial)
        run_emit();

    power_off();
    return 0; /* never reached */
}
