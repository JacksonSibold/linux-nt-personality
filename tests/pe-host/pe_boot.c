/*
 * pe_boot.c - the kernel's PE loader (src/kernel/fs/binfmt_pe.c) run in this process to start a PE program.
 *
 * binfmt_pe.c is compiled here against its userspace kernel model (src/kernel/fs/kshim.h), which maps real memory, with
 * CONFIG_NT_PERSONALITY set so that it registers what it maps with the NT memory manager and installs the standard
 * handles, exactly as in the kernel. Three things differ from the model's defaults and are redirected below: where the
 * loader finds ntdll.dll (under the runner's root directory), the standard handles (the NT I/O module has its own file
 * type), and the thread start (the runner needs the TEB address the loader would have put in the GS base).
 */
#define CONFIG_NT_PERSONALITY 1
#include "../../src/kernel/fs/kshim.h"

#include "pe_host.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>

/* The loader's kernel log lines are shown only when asked for. */
int pehost_verbose;
static int pehost_klog(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (!pehost_verbose)
        return 0;
    va_start(ap, fmt);
    n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return n;
}
#undef pr_err
#undef pr_warn
#undef pr_info
#define pr_err(fmt, ...) pehost_klog(fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) pehost_klog(fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) pehost_klog(fmt, ##__VA_ARGS__)

static unsigned long g_boot_gs;
static const char *g_boot_root = "";

/* Absolute paths the loader opens (ntdll.dll) are looked up under the root directory. */
static struct file *pehost_filp_open(const char *name, int flags, unsigned int mode)
{
    char path[PATH_MAX];
    if (name[0] == '/' && (strncmp(name, "/ntdll.dll", 10) == 0 || strncmp(name, "/windows/", 9) == 0)) {
        snprintf(path, sizeof path, "%s%s", g_boot_root, name);
        return filp_open(path, flags, mode);
    }
    return filp_open(name, flags, mode);
}

/* fd 0, 1 and 2 are handed to the NT I/O module by number. */
#define fget(fd) ((struct file *)(intptr_t)((fd) + 1))
#define ntio_install_std_file pehost_boot_std_install
#define ntio_release_std_handle pehost_boot_std_release
#define x86_gsbase_write_cpu_inactive(gs) (g_boot_gs = (gs))
#define filp_open pehost_filp_open
#define TASK_SIZE_MAX 0x7ffffffff000UL

#include "../../src/kernel/fs/binfmt_pe.c"

#undef filp_open

int pehost_boot_std_install(struct file *filp, u64 *out_handle)
{
    unsigned long long h = 0;
    int rc = pehost_std_install((int)(intptr_t)filp - 1, &h);
    if (rc == 0)
        *out_handle = h;
    return rc;
}

void pehost_boot_std_release(u64 h) { pehost_std_release(h); }

int pehost_boot(const char *root, const char *exe_path, int argc, const char *const *argv, const char *const *envp, struct pehost_start *out)
{
    struct linux_binprm bprm;
    struct cred cred;
    struct mm_struct mm;
    int envc = 0, rc;

    memset(&bprm, 0, sizeof bprm);
    memset(&cred, 0, sizeof cred);
    memset(&mm, 0, sizeof mm);
    g_boot_root = root;
    while (envp && envp[envc])
        ++envc;
    bprm.filename = exe_path;
    bprm.file = filp_open(exe_path, O_RDONLY, 0);
    if (IS_ERR(bprm.file))
        return (int)PTR_ERR(bprm.file);
    bprm.mm = &mm;
    bprm.cred = &cred;
    bprm.argc = argc;
    bprm.argv = argv;
    bprm.envc = envc;
    bprm.envp = envp;
    kshim_regs.ip = kshim_regs.sp = kshim_regs.cx = 0;
    g_boot_gs = 0;
    rc = load_pe_binary(&bprm);
    if (rc)
        return rc;
    if (!kshim_regs.ip || !g_boot_gs)
        return -ENOEXEC;
    out->ip = kshim_regs.ip;
    out->sp = kshim_regs.sp;
    out->cx = kshim_regs.cx;
    out->teb = g_boot_gs;
    return 0;
}
