# Test matrix

Recorded by `tools/run_all_checks.sh` (see [DEVELOPMENT.md](../DEVELOPMENT.md#running-every-local-check-at-once)).

- Date: 2026-10-12
- Commit: 21aa5e6 (clean tree at run time)
- Host: Windows 11, Git Bash, MSYS2 UCRT64 gcc 16.2.0, WinPython 3.13.12
- Result: PASS=11 FAIL=1 KNOWN-FAIL=2 SKIP=0 (exit 1 because of the syntax row)

```
CHECK                      STATUS      NOTE
unit                       PASS
check_repo                 PASS
check_doc_links            PASS
provenance                 PASS
lint                       PASS
syntax                     FAIL        exit 5 (5 files, see below)
host                       PASS
kernel-host:object_test    PASS
kernel-host:security_test  PASS
kernel-host:registry_test  PASS
kernel-host:i2_test        PASS
kernel-host:io_test        KNOWN-FAIL  rc=1 (listed in tests/kernel-host/README.md)
kernel-host:mm_test        KNOWN-FAIL  rc=1 (listed in tests/kernel-host/README.md)
kernel-host:sync_test      PASS
```

## Notes

- **syntax (FAIL).** Five files fail `gcc -fsyntax-only`: `src/kernel/fs/binfmt_pe.c`,
  `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`,
  `arch/x86/nt/kernel_stubs.c` and `src/kernel/nt/dispatch.c`. This matches the
  "Remaining failures (5)" list in `tests/syntax/README.md`. They are model and
  shim gaps, not stale expectations, so they are left as FAIL.
- **kernel-host build fix.** Before this run, `kshim.o` did not build on the host
  (`st_atime` against the Linux-layout shim), so `make check` ran no module. Fixed
  by defining `KHOST_LINUX_STAT` in the kernel-host CFLAGS (commit 8a08ecf).
- **kernel-host KNOWN-FAIL.** `mm_test` (2 of 14, `STATUS_NO_MEMORY` on reserve)
  and `io_test` (intermittent; this run failed at `io_test.c:420`) are the modules
  listed under `known-fail:` in `tests/kernel-host/README.md`.
- `proc_thread_test` is build-only and is not part of this matrix.
