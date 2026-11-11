/*
 * ntpers_run.c - runs a PE program of this repository in a Linux process, on the kernel's own NT code.
 *
 *     ntpers_run [--root DIR] [--trace] [--status] program.exe [arguments...]
 *
 * The program is loaded by the kernel's PE loader (pe_boot.c). Its NT system calls are caught with Syscall User Dispatch:
 * the kernel raises SIGSYS for every `syscall` instruction executed while the per-thread selector says "block", and the
 * handler hands the call to the same service functions the kernel dispatches to (src/kernel/nt, built for user space as in
 * tests/kernel-host). The TEB is addressed through GS as on Windows; the host C library uses FS, so the two do not meet.
 *
 * What is the runner's own, and so not the kernel's behaviour: process exit (NtTerminateProcess on the current process ends
 * this process), the start of new threads (a PE start routine is entered from a host thread), and the root directory that
 * NT paths are resolved under. A fault in PE code is reported and ends the run; it is not yet delivered as an NT exception.
 */
#define _GNU_SOURCE
#include "object.h"
#include "handle.h"
#include "registry.h"
#include "../../src/ntdll/nt_types.h"
#include "../../src/ntdll/ntstatus.h"

#include "pe_host.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#define PR_SYS_DISPATCH_OFF 0
#define PR_SYS_DISPATCH_ON 1
#endif
#define SEL_ALLOW 0
#define SEL_BLOCK 1
#ifndef ARCH_SET_GS
#define ARCH_SET_GS 0x1001
#endif

#define SSN_NtTerminateProcess 0x2c
#define SSN_NtCreateThreadEx 0xc5
#define NR_SERVICES 0x200
#define MAX_ARGS 12

typedef NTSTATUS (*service_fn)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long,
                               unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);

/* A service the user-space build of the NT modules does not define stays NULL and is refused. */
#define NT_SERVICE(ssn, name, argc)                                                                                                    \
    extern NTSTATUS nt_svc_##name(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, \
                                  unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) __attribute__((weak));
#include "nt_service_list.h"
#undef NT_SERVICE

static const struct {
    service_fn fn;
    int argc;
    const char *name;
} g_services[NR_SERVICES] = {
#define NT_SERVICE(ssn, svc, nargs) [ssn] = {nt_svc_##svc, nargs, #svc},
#include "nt_service_list.h"
#undef NT_SERVICE
};

extern char g_kshim_root[PATH_MAX];
int ntio_install_std_file(struct file *filp, u64 *out_handle);
void ntio_release_std_handle(u64 h);

static __thread volatile char t_selector; /* SEL_ALLOW while host code runs, SEL_BLOCK while PE code runs */
static int g_trace, g_print_status;
static unsigned char g_reported[NR_SERVICES];

/* The return from the SIGSYS handler is itself a system call made while the selector already says "block" again; calls from
 * this range are exempt. The kernel compares the address after the syscall instruction, hence the padding. */
void pehost_restorer(void);
void pehost_restorer_end(void);
__asm__(".text\n"
        ".globl pehost_restorer\n"
        "pehost_restorer:\n"
        "    mov $15, %eax\n" /* rt_sigreturn */
        "    syscall\n"
        "    nop\n"
        ".globl pehost_restorer_end\n"
        "pehost_restorer_end:\n");

/* Leaves host code for PE code: the stack, the first argument, then the entry. Does not return. */
void pehost_enter(unsigned long ip, unsigned long sp, unsigned long cx) __attribute__((noreturn));
__asm__(".text\n"
        ".globl pehost_enter\n"
        "pehost_enter:\n"
        "    mov %rsi, %rsp\n"
        "    mov %rdx, %rcx\n"
        "    xor %ebp, %ebp\n"
        "    jmp *%rdi\n");

/* ---- Hooks from the loader ------------------------------------------------------------------------------------------------- */

int pehost_std_install(int fd, unsigned long long *out_handle)
{
    struct file *f;
    u64 h = 0;
    int copy = dup(fd), rc;
    if (copy < 0)
        return -EBADF;
    f = calloc(1, sizeof *f);
    if (!f) {
        close(copy);
        return -ENOMEM;
    }
    f->fd = copy;
    rc = ntio_install_std_file(f, &h);
    if (rc == 0)
        *out_handle = h;
    return rc;
}

void pehost_std_release(unsigned long long handle) { ntio_release_std_handle((u64)handle); }

/* The user-space build keeps files and the other objects in two handle tables, each with its own NtClose. */
NTSTATUS pehost_ob_NtClose(HANDLE h);
NTSTATUS pehost_io_NtClose(HANDLE h);

NTSTATUS nt_svc_NtClose(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5,
                        unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    NTSTATUS st = pehost_ob_NtClose((HANDLE)a0);
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (st == STATUS_INVALID_HANDLE)
        st = pehost_io_NtClose((HANDLE)a0);
    return st;
}

/* ---- Threads ------------------------------------------------------------------------------------------------------------------ */

static void thread_dispatch_on(void)
{
    t_selector = SEL_ALLOW;
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, (unsigned long)pehost_restorer,
              (unsigned long)((char *)pehost_restorer_end - (char *)pehost_restorer), &t_selector) != 0) {
        perror("pehost: PR_SET_SYSCALL_USER_DISPATCH");
        exit(125);
    }
}

struct pe_thread_start {
    ULONG(__attribute__((ms_abi)) * routine)(PVOID);
    PVOID arg;
};

/* The start routine the NT thread code calls on its new host thread: it enters the PE routine with dispatch switched on. */
static ULONG pe_thread_entry(PVOID p)
{
    struct pe_thread_start s = *(struct pe_thread_start *)p;
    ULONG r;
    free(p);
    thread_dispatch_on();
    t_selector = SEL_BLOCK;
    r = s.routine(s.arg);
    t_selector = SEL_ALLOW;
    return r;
}

/* ---- System calls ----------------------------------------------------------------------------------------------------------- */

static void finish_process(NTSTATUS status)
{
    int code = status == 0 ? 0 : ((status & 0xFF) ? (int)(status & 0xFF) : 255);
    fflush(NULL);
    if (g_print_status)
        fprintf(stderr, "pehost: exit status 0x%08x\n", (unsigned)status);
    _exit(code);
}

static void on_sigsys(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    greg_t *r = uc->uc_mcontext.gregs;
    unsigned long args[MAX_ARGS] = {0};
    unsigned ssn = (unsigned)r[REG_RAX];
    NTSTATUS status;
    int i, argc;
    (void)sig;
    (void)si;
    t_selector = SEL_ALLOW;

    if (ssn >= NR_SERVICES || !g_services[ssn].name) {
        if (g_trace)
            fprintf(stderr, "pehost: service 0x%x is not in the table\n", ssn);
        r[REG_RAX] = (greg_t)(uint32_t)STATUS_INVALID_SYSTEM_SERVICE;
        t_selector = SEL_BLOCK;
        return;
    }
    argc = g_services[ssn].argc;
    args[0] = (unsigned long)r[REG_R10];
    args[1] = (unsigned long)r[REG_RDX];
    args[2] = (unsigned long)r[REG_R8];
    args[3] = (unsigned long)r[REG_R9];
    for (i = 4; i < argc && i < MAX_ARGS; ++i)
        args[i] = *(const unsigned long *)(uintptr_t)(r[REG_RSP] + 0x28 + 8 * (i - 4));

    if (ssn == SSN_NtTerminateProcess && (args[0] == 0 || args[0] == (unsigned long)-1))
        finish_process((NTSTATUS)args[1]);

    if (!g_services[ssn].fn) {
        if (!g_reported[ssn]) {
            g_reported[ssn] = 1;
            fprintf(stderr, "pehost: service 0x%x (%s) has no user-space implementation\n", ssn, g_services[ssn].name);
        }
        status = STATUS_NOT_IMPLEMENTED;
    } else {
        if (ssn == SSN_NtCreateThreadEx && args[4]) {
            struct pe_thread_start *s = malloc(sizeof *s);
            if (s) {
                s->routine = (ULONG(__attribute__((ms_abi)) *)(PVOID))args[4];
                s->arg = (PVOID)args[5];
                args[4] = (unsigned long)pe_thread_entry;
                args[5] = (unsigned long)s;
            }
        }
        status = g_services[ssn].fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
    }
    if (g_trace)
        fprintf(stderr, "pehost: %-28s -> 0x%08x\n", g_services[ssn].name, (unsigned)status);
    r[REG_RAX] = (greg_t)(uint32_t)status;
    t_selector = SEL_BLOCK;
}

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    t_selector = SEL_ALLOW;
    fprintf(stderr, "pehost: signal %d at rip 0x%llx, address %p (rsp 0x%llx)\n", sig, (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
            si->si_addr, (unsigned long long)uc->uc_mcontext.gregs[REG_RSP]);
    fflush(NULL);
    _exit(128 + sig);
}

struct kernel_sigaction {
    void (*handler)(int, siginfo_t *, void *);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
};

static void install_handler(int sig, void (*fn)(int, siginfo_t *, void *))
{
    struct kernel_sigaction ksa;
    memset(&ksa, 0, sizeof ksa);
    ksa.handler = fn;
    ksa.flags = SA_SIGINFO | 0x04000000UL /* SA_RESTORER */ | SA_NODEFER;
    ksa.restorer = pehost_restorer;
    if (syscall(SYS_rt_sigaction, sig, &ksa, NULL, 8) != 0) {
        perror("pehost: rt_sigaction");
        exit(125);
    }
}

/* ---- Start ---------------------------------------------------------------------------------------------------------------------- */

extern char **environ;

int main(int argc, char **argv)
{
    const char *root = NULL;
    struct pehost_start st;
    struct nt_eprocess *proc = NULL;
    struct nt_ethread *thr = NULL;
    char root_abs[PATH_MAX], exe_abs[PATH_MAX];
    int i = 1, rc;

    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(argv[i], "--trace") == 0)
            g_trace = pehost_verbose = 1;
        else if (strcmp(argv[i], "--status") == 0)
            g_print_status = 1;
        else
            break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: ntpers_run [--root DIR] [--trace] [--status] program.exe [arguments...]\n");
        return 124;
    }
    if (!root)
        root = getenv("PEHOST_ROOT");
    if (!root || !realpath(root, root_abs)) {
        fprintf(stderr, "pehost: no root directory (use --root DIR; it holds windows/system32/*.dll)\n");
        return 124;
    }
    if (!realpath(argv[i], exe_abs)) {
        fprintf(stderr, "pehost: cannot find %s\n", argv[i]);
        return 124;
    }
    snprintf(g_kshim_root, PATH_MAX, "%s", root_abs);

    if (ObInitObjectManager() != STATUS_SUCCESS || ntreg_init() != STATUS_SUCCESS) {
        fprintf(stderr, "pehost: NT object manager or registry did not start\n");
        return 125;
    }
    if (ObCreateProcessObject(&proc, (ULONG)getpid(), "pehost") != STATUS_SUCCESS || ObCreateThreadObject(&thr, proc, (ULONG)getpid()) != STATUS_SUCCESS) {
        fprintf(stderr, "pehost: cannot create the process and thread objects\n");
        return 125;
    }
    kshim_set_current_process(proc);
    kshim_set_current_thread(thr);

    rc = pehost_boot(root_abs, exe_abs, argc - i, (const char *const *)(argv + i), (const char *const *)environ, &st);
    if (rc) {
        fprintf(stderr, "pehost: the loader refused %s (%d)\n", exe_abs, rc);
        return 126;
    }
    thr->teb = (struct _TEB *)(uintptr_t)st.teb;
    thr->teb_base = st.teb;
    if (g_trace)
        fprintf(stderr, "pehost: entry 0x%lx sp 0x%lx rcx 0x%lx teb 0x%lx\n", st.ip, st.sp, st.cx, st.teb);

    install_handler(SIGSYS, on_sigsys);
    install_handler(SIGSEGV, on_fault);
    install_handler(SIGBUS, on_fault);
    install_handler(SIGILL, on_fault);
    install_handler(SIGFPE, on_fault);
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, st.teb) != 0) {
        perror("pehost: ARCH_SET_GS");
        return 125;
    }
    fflush(NULL);
    thread_dispatch_on();
    t_selector = SEL_BLOCK;
    pehost_enter(st.ip, st.sp, st.cx);
}
