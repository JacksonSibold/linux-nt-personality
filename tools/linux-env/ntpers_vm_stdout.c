/*
 * ntpers_vm_stdout.c - stdout channel for the *_direct probes inside the NT personality.
 *
 * The direct probes (tests/differential/probes/*_direct.c, built with -DNTPERS_VM) end by calling
 * linux_write(1, json, len). That helper is a raw `syscall` with rax=1, which the NT personality
 * dispatches as an NT service number, not as write(2). The NT-ABI way to write to standard output
 * is NtWriteFile on the console pseudo-handle -11, which the kernel console path
 * (arch/x86/nt/nt_io.c ntio_console_write) prints through pr_info() onto the serial console.
 *
 * Build: each probe is compiled with
 *   -Dlinux_open=ntpers_vm_open -Dlinux_write=ntpers_vm_write -Dlinux_close=ntpers_vm_close
 * so the probe source is unchanged. The 9p file writes in the probes then go to these stubs
 * (open and close return -1, write of any fd other than 1 returns -1), and no Linux-numbered
 * syscall is issued from a PE. The probe's own asm NtWriteFile stub (SSN as in the probe) is used.
 *
 * printk truncates one record at about 1 KB (the "nt console" prefix included), so stdout is written in chunks of at
 * most 700 bytes, split after a newline where possible. The serial framing is undone by
 * tools/linux-env/probe_results.py split.
 */
typedef long NTSTATUS;
typedef unsigned long ULONG_PTR_T;

/* Probe-side asm stub (defined by every *_direct probe with the NT SSN for NtWriteFile). */
extern NTSTATUS NtWriteFile();

#define NTPERS_CONSOLE_STDOUT ((void *)(long)-11)
#define NTPERS_CHUNK 700

long ntpers_vm_open(const char *path, int flags, int mode)
{
    (void)path; (void)flags; (void)mode;
    return -1;
}

long ntpers_vm_close(int fd)
{
    (void)fd;
    return -1;
}

long ntpers_vm_write(int fd, const void *buf, unsigned long len)
{
    const char *p = (const char *)buf;
    unsigned long off = 0;
    /* IO_STATUS_BLOCK is two 8-byte fields on x64 (Status, Information). */
    unsigned long long iosb[2];

    if (fd != 1)
        return -1;
    while (off < len) {
        unsigned long n = len - off;
        unsigned long k;
        if (n > NTPERS_CHUNK)
            n = NTPERS_CHUNK;
        if (off + n < len) {
            /* Prefer a split right after a newline so each console line stays whole. */
            for (k = n; k > 0 && p[off + k - 1] != '\n'; k--)
                ;
            if (k > 0)
                n = k;
        }
        iosb[0] = 0;
        iosb[1] = 0;
        NtWriteFile(NTPERS_CONSOLE_STDOUT, 0, 0, 0, iosb, (void *)(p + off), (ULONG_PTR_T)n, 0, 0);
        if (iosb[1] == 0)
            break;
        off += n;
    }
    return (long)off;
}
