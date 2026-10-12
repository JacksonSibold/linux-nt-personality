# Syntax checks for kernel-facing sources

`check.sh` runs `gcc -fsyntax-only -Wall -Wextra -std=c11` over every
`src/kernel/nt/*.c`, `src/loader/*.c` and `src/kernel/fs/*.c` file, with
`-DUSERSPACE_KSHIM` (the userspace shim flavour) and the headers in
`tests/syntax/shim/` on the include path. It prints PASS/FAIL per file and a
warning count. Nothing is linked or run. Sources are not modified.

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

25 of 30 files pass. The five failures are not source errors on a Linux build:

| File | Reason |
|------|--------|
| `src/kernel/nt/dispatch.c` | needs `<linux/kernel.h>`, `<linux/sched.h>` and other kernel headers |
| `src/kernel/nt/weak.c` | needs `<linux/sched.h>` (`struct task_struct`) |
| `src/kernel/nt/kshim.c` | uses glibc `struct stat` members `st_atim`/`st_mtim`/`st_ctim`; MinGW has only `st_*time` |
| `src/kernel/fs/binfmt_pe.c` | needs kernel headers (`<linux/kernel.h>`, `<linux/fs.h>`, `<asm/...>`); only buildable in-tree |
| `src/kernel/fs/binfmt_pe_test.c` | includes `binfmt_pe.c`, so it fails for the same reason |

These need real kernel headers or a glibc sysroot. Adding them would mean
writing kernel-API stubs, which is out of scope here.

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
