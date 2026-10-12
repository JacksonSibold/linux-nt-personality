# Syntax checks for kernel-facing sources

`check.sh` runs `gcc -fsyntax-only -Wall -Wextra -std=c11` over every
`src/kernel/nt/*.c`, `src/loader/*.c`, `src/kernel/fs/*.c` and `arch/x86/nt/*.c`
file, with `-DUSERSPACE_KSHIM` (the userspace shim flavour), the headers in
`tests/syntax/shim/` on the include path, and `-iquote src/kernel/nt
-iquote arch/x86/nt` so kernel-tree headers resolve for `"..."` includes.
`-iquote` is used rather than `-I` on purpose: `-I src/kernel/nt` makes
MinGW's `<sys/stat.h>` pick up `src/kernel/nt/io.h` instead of `<io.h>`.
It prints PASS/FAIL per file and a warning count. Nothing is linked or run.
Sources are not modified.

```sh
sh tests/syntax/check.sh            # exit status = number of failing files
CC=/c/msys64/ucrt64/bin/gcc.exe sh tests/syntax/check.sh
```

## Shims

- `shim/kernel_compat.h` is force-included. It supplies `loff_t`, `O_DIRECTORY`,
  `O_SYNC`, `pread`/`pwrite` declarations, a `mkdir` mapping to MinGW `_mkdir`,
  and an `fsync` stub.
- `shim/sys/mman.h` and `shim/sys/syscall.h` are minimal POSIX declarations
  (MinGW has neither).

The shims are for parsing and type-checking only. They are not a runtime model.

## Current result (MinGW gcc 16.2, UCRT64)

28 of 37 files pass at `haiku/recheck` HEAD. At the baseline commit `5eb8026`
(same script, same flags) 27 of 37 passed. No file that passed at `5eb8026`
fails at HEAD. `src/kernel/nt/ntstatus_kernel.c` went from FAIL to PASS.

| File | 5eb8026 | HEAD | Reason for a FAIL |
|------|---------|------|-------------------|
| `src/kernel/nt/dispatch.c` | FAIL | FAIL | needs `<linux/kernel.h>`, `<linux/sched.h>` and other kernel headers (fatal, so the body is not checked) |
| `src/kernel/nt/weak.c` | FAIL | FAIL | needs `<linux/sched.h>` (`struct task_struct`) |
| `src/kernel/nt/kshim.c` | FAIL | FAIL | uses glibc `struct stat` members `st_atim`/`st_mtim`/`st_ctim`; MinGW has only `st_*time` |
| `src/kernel/fs/binfmt_pe.c` | FAIL | FAIL | needs kernel headers (`<linux/kernel.h>`, `<linux/fs.h>`, `<asm/...>`); only buildable in-tree |
| `src/kernel/fs/binfmt_pe_test.c` | FAIL | FAIL | includes `binfmt_pe.c`, so it fails for the same reason |
| `arch/x86/nt/dispatch.c` | FAIL | FAIL | needs `<linux/kernel.h>` (same as the `src` copy) |
| `arch/x86/nt/weak.c` | FAIL | FAIL | needs `<linux/sched.h>` (same as the `src` copy) |
| `arch/x86/nt/kernel_stubs.c` | FAIL | FAIL | not C: the file is six bytes (`test` + CRLF), committed that way at `5eb8026` |
| `arch/x86/nt/mm.c` | FAIL | FAIL | `printk(KERN_EMERG ...)` debug line at 1272 uses kernel-only names; the `src` copy has no such line |

The `linux/*` and `asm/*` failures need real kernel headers, which means writing
kernel-API stubs. That is out of scope here. The `arch/x86/nt/kernel_stubs.c`
content and the `mm.c` debug `printk` are pre-existing and are left for the
owner to decide.

The other 28 files pass. Their warning counts at HEAD are in the table below.
`src/kernel/nt/io.c` went from 8 warnings at `5eb8026` to 4 at HEAD.

| File | 5eb8026 | HEAD | Warnings (HEAD) |
|------|---------|------|-----------------|
| `arch/x86/nt/nt_io.c` | PASS | PASS | 8 |
| `arch/x86/nt/nt_services.c` | PASS | PASS | 17 |
| `arch/x86/nt/proc_thread.c` | PASS | PASS | 6 |
| `src/kernel/fs/kshim.c` | PASS | PASS | 10 |
| `src/kernel/nt/debugfs_tests.c` | PASS | PASS | 0 |
| `src/kernel/nt/handle.c` | PASS | PASS | 4 |
| `src/kernel/nt/i2_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/io.c` | PASS | PASS | 4 |
| `src/kernel/nt/io_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/iocp.c` | PASS | PASS | 4 |
| `src/kernel/nt/mm.c` | PASS | PASS | 4 |
| `src/kernel/nt/mm_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/nt_obj.c` | PASS | PASS | 4 |
| `src/kernel/nt/nt_services.c` | PASS | PASS | 17 |
| `src/kernel/nt/ntstatus_kernel.c` | FAIL | PASS | 4 |
| `src/kernel/nt/object.c` | PASS | PASS | 4 |
| `src/kernel/nt/object_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/proc_thread.c` | PASS | PASS | 6 |
| `src/kernel/nt/proc_thread_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/registry.c` | PASS | PASS | 32 |
| `src/kernel/nt/registry_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/repro_mem_vad.c` | PASS | PASS | 4 |
| `src/kernel/nt/security.c` | PASS | PASS | 5 |
| `src/kernel/nt/security_test.c` | PASS | PASS | 5 |
| `src/kernel/nt/sync.c` | PASS | PASS | 4 |
| `src/kernel/nt/sync_test.c` | PASS | PASS | 4 |
| `src/loader/pe_parse.c` | PASS | PASS | 0 |
| `src/loader/pe_prot.c` | PASS | PASS | 0 |

Failures listed above that the script reports as "fatal" stop at the first
missing header, so the rest of those files is not checked. A partial check of
`src/kernel/nt/dispatch.c` with empty `linux/*` stubs (not committed) showed no
undefined identifier at the lines changed by the fix commits.

## Warnings that are platform artifacts

- `-Wint-to-pointer-cast` / `-Wpointer-to-int-cast` in `registry.c`,
  `nt_services.c`, `kshim.h` and others: MinGW is LLP64, so `long` is 32-bit.
  The Linux kernel build is LP64, where these casts are the same size.
- `-Woverflow` on 5368709120 in `kshim.c` and `binfmt_pe_test.c`: same cause.

## Warnings left in the sources (not fixed)

- `security.c:1604`: `desired_for_restricted` is computed but never used. The
  restricted-token check that should use it is incomplete, so this is a logic
  question, not a trivial fix.
- `security_test.c:279`: `count` set but unused.
- `proc_thread.c:909,915`: unused parameter `Class` (`-Wextra` only).
