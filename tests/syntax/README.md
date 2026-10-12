# Syntax checks for kernel-facing sources

`check.sh` runs `gcc -fsyntax-only -Wall -Wextra -std=c11` over every
`src/kernel/nt/*.c`, `src/loader/*.c`, `src/kernel/fs/*.c` and `arch/x86/nt/*.c`
file, with `-DUSERSPACE_KSHIM` (the userspace shim flavour), the headers in
`tests/syntax/shim/` on the include path, and `-iquote src/kernel/nt
-iquote arch/x86/nt` so kernel-tree headers resolve for `"..."` includes.
`-iquote` is used rather than `-I` on purpose: `-I src/kernel/nt` makes
MinGW's `<sys/stat.h>` pick up `src/kernel/nt/io.h` instead of `<io.h>`.
It prints PASS/FAIL per file and a warning count. Nothing is linked or run.
The script itself does not modify sources.

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
- `shim/linux/*.h`, `shim/linux/sched/*.h` and `shim/asm/*.h` are stub kernel
  headers for the `<linux/...>` and `<asm/...>` includes of the kernel-facing
  sources. Each declares only the types, functions and macros the sources use,
  with the mainline signature or layout where it is known. Some headers declare
  nothing of their own (`linux/highmem.h`, `linux/spinlock.h`,
  `linux/personality.h`, `asm/cpufeature.h`, `asm/tlb.h`), and
  `linux/sched/task_stack.h` only includes `linux/sched.h`: the sources include
  them and use nothing from them.

Two userspace models already define some of the same names, and their
definitions must not be redeclared in a way that conflicts:

- `src/kernel/nt/kshim.h` (guard `NTOBJ_KSHIM_H`) defines `kmalloc`,
  `kzalloc`, `kfree`, `list_head`, `rb_node`, `struct file` and more.
- `src/kernel/fs/kshim.h` (guard `KSHIM_H`) defines `task_struct`, `mm_struct`,
  `struct file`, `pt_regs`, `linux_binprm`, `PAGE_*`, `PROT_*`, `MAP_*`,
  `printk`, `kmalloc` and more.

Shim declarations of names the fs model defines are wrapped in `#ifndef KSHIM_H`.
The nt model's `kmalloc`/`kzalloc` prototypes are omitted from `linux/slab.h`
under `NTOBJ_KSHIM_H`, but that guard is not yet defined when `dispatch.c`
includes `linux/slab.h` first, so the conflict remains (see below).

Values that are not mainline values:

- `TIF_NTABI` and `TIF_NTSUSPEND` are not defined anywhere in this repository
  (`docs/phase1/research-1-nt-architecture.md` describes the flag as a kernel
  change). They are placeholders (32 and 33).
- `gfp_t` is `int` to match the `kshim.h` model's `int flags` arguments.
- `EXSTACK_DEFAULT` is 0. `DEFINE_PER_CPU`, `per_cpu` and `this_cpu_inc` model a
  single instance. `__user` is empty. `THIS_MODULE` is a null pointer.
  `get_user`, `put_user` and `__get_user` are plain assignments. `access_ok` is
  always 1.

The shims are for parsing and type-checking only. They are not a runtime model.

## Current result (MinGW gcc 16.2, UCRT64)

Start of the shim work (`5779442`): 28 of 37 files passed. Now: 32 of 37.
No file that passed at the start fails now.

| File | start | now | Warnings (now) |
|------|-------|-----|----------------|
| `arch/x86/nt/dispatch.c` | FAIL | FAIL | 49 |
| `arch/x86/nt/kernel_stubs.c` | FAIL | FAIL | 0 |
| `arch/x86/nt/mm.c` | FAIL | PASS | 4 |
| `arch/x86/nt/nt_io.c` | PASS | PASS | 8 |
| `arch/x86/nt/nt_services.c` | PASS | PASS | 17 |
| `arch/x86/nt/proc_thread.c` | PASS | PASS | 6 |
| `arch/x86/nt/weak.c` | FAIL | PASS | 4 |
| `src/kernel/fs/binfmt_pe.c` | FAIL | PASS | 82 |
| `src/kernel/fs/binfmt_pe_test.c` | FAIL | FAIL | 106 |
| `src/kernel/fs/kshim.c` | PASS | PASS | 10 |
| `src/kernel/nt/debugfs_tests.c` | PASS | PASS | 0 |
| `src/kernel/nt/dispatch.c` | FAIL | FAIL | 49 |
| `src/kernel/nt/handle.c` | PASS | PASS | 4 |
| `src/kernel/nt/i2_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/io.c` | PASS | PASS | 4 |
| `src/kernel/nt/io_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/iocp.c` | PASS | PASS | 4 |
| `src/kernel/nt/kshim.c` | FAIL | FAIL | 4 |
| `src/kernel/nt/mm.c` | PASS | PASS | 4 |
| `src/kernel/nt/mm_test.c` | PASS | PASS | 4 |
| `src/kernel/nt/nt_obj.c` | PASS | PASS | 4 |
| `src/kernel/nt/nt_services.c` | PASS | PASS | 17 |
| `src/kernel/nt/ntstatus_kernel.c` | PASS | PASS | 4 |
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
| `src/kernel/nt/weak.c` | FAIL | PASS | 4 |
| `src/loader/pe_parse.c` | PASS | PASS | 0 |
| `src/loader/pe_prot.c` | PASS | PASS | 0 |

### Remaining failures (5)

- `src/kernel/nt/dispatch.c` and `arch/x86/nt/dispatch.c` (the two copies are
  identical). Both fail for the same reason, which is a conflict between the
  nt userspace model and the kernel headers, not a missing header. `dispatch.c`
  includes `<linux/slab.h>` before `sync.h` brings in `kshim.h`. The slab shim
  declares `kmalloc`/`kzalloc`/`kfree`, then `kshim.h` defines them `static
  inline`. `dispatch.c` calls `kzalloc` and `kfree` (lines 258 and 271), so
  the conflict cannot be avoided by leaving those prototypes out. Completing the `struct
  file_operations` that `dispatch.c` fills in needs `<linux/fs.h>`, and that
  header's `struct file` clashes with `kshim.h`'s `struct file`. Both were
  checked with probe copies (not committed). Fixing it needs a change to the
  model or to the include order, so it is left.
- `src/kernel/nt/kshim.c`: uses glibc `struct stat` members `st_atim`,
  `st_mtim` and `st_ctim`. MinGW has only `st_atime` and the others. Platform
  limit, unchanged from the start.
- `src/kernel/fs/binfmt_pe_test.c`: now gets past the loader (`binfmt_pe.c`
  itself passes). It fails on two things. (1) Names the test uses that are not
  defined anywhere in the repository: `pe_build_command_line`,
  `pe_build_environment`, `pe_utf8_to_utf16`, `pe_prot_to_mmap`, `WCHAR`,
  `PPEB`, `PTEB`, and a `pe` member of `struct pe_load_state`. The test has
  probably not compiled against the current loader. (2) The userspace model in
  `src/kernel/fs/kshim.h` does not match the kernel API the loader uses:
  `kernel_read()` takes `(f, u64 offset, buf, count)` where the kernel takes
  `(file, buf, count, pos)`; `struct file` has no `f_inode`; `struct
  task_struct` has no `mm` or `thread`; `linux_binprm` has no `cred`; and the
  model defines `nt_enter_personality`, which `binfmt_pe.c` also defines in its
  `!CONFIG_NT_PERSONALITY` branch. These need a test-model change, so they are
  left.
- `arch/x86/nt/kernel_stubs.c`: not C. The file is six bytes (`test` and CRLF),
  committed that way. It is not deleted; the owner decides.

## Defects fixed in this pass

- `arch/x86/nt/mm.c`: removed the `printk(KERN_EMERG ...)` debug line at the
  start of `NtProtectVirtualMemory`. `printk` and `KERN_EMERG` were undeclared
  there, and the `src` copy does not have the line.
- `src/kernel/fs/binfmt_pe_test.c`: added `#include "../../loader/pe_prot.h"`.
  `test_prot_mapping()` uses `pe_page_protection_t`, `pe_prot_from_section` and
  `PE_PROT_*`, which that header declares.

Found but not changed: `arch/x86/nt/mm.c:72` and `src/kernel/nt/mm.c:72`
(`kshim_mprotect`) log at `KERN_EMERG` with `printk`. That code is in the
`__KERNEL__` branch, so this script does not compile it.

## Warnings that are platform artifacts

- `-Wint-to-pointer-cast` / `-Wpointer-to-int-cast` (about 69 in `binfmt_pe.c`,
  and many in `registry.c`, `nt_services.c`, `kshim.h` and others): MinGW is
  LLP64, so `long` is 32-bit. The Linux kernel build is LP64, where these casts
  are the same size. `binfmt_pe.c` now compiles past its errors, so its casts
  are counted for the first time.
- `-Woverflow` on 5368709120 in `kshim.c` and `binfmt_pe_test.c`: same cause.
- `-Wshift-count-overflow` on `(1UL << 47)` in `TASK_SIZE_MAX`: same cause. A
  64-bit constant moves the warning to `-Woverflow`/`-Wtype-limits`, so the
  mainline form is kept.
- `'X' redefined` warnings for `KERN_*`, `pr_info`, `pr_err`, `pr_warn` and
  `BUG_ON` (about 12, in `dispatch.c`, `weak.c`, `mm.c`): the shim defines them
  first, then `kshim.h` defines them again with different bodies.

## Warnings left in the sources (not fixed)

- `security.c:1604`: `desired_for_restricted` is computed but never used. The
  restricted-token check that should use it is incomplete, so this is a logic
  question, not a trivial fix.
- `security_test.c:279`: `count` set but unused.
- `proc_thread.c:909,915`: unused parameter `Class` (`-Wextra` only).
- `binfmt_pe.c:330` (`size_image`) and `binfmt_pe.c:1073-1076` (`a2`..`a11`
  in the `NtTerminateProcess` override): unused parameters (`-Wextra` only).
