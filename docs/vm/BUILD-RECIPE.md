# Build recipe: NT personality kernel in a fresh Debian guest

This recipe is derived from the repository's scripts and patches only:
`vm/rebuild_kdir.sh`, `vm/kbuild.sh`, `vm/mkinitramfs.sh`, `vm/freshness.sh`,
`vm/runvm.sh`, `vm/init.c`, `vm/check_m0.sh`, `vm/test_m0.sh`,
`tests/kernel/check_kernel_complete.sh`, `src/kernel/patches/*.patch`, and
`docs/vm/OVERVIEW.md`. Nothing was built or booted while writing it, and the
kernel tarball was not downloaded, so every statement about the kernel tree is
a claim to be checked on the first run. Where the repository's own sources
disagree, the disagreement is recorded in **Known gaps** at the end. A run that
reaches the end of this document unchanged is not yet expected to pass; the
gaps say what must be decided first.

Companion files:

- `vm/guest-bootstrap.sh` runs steps 0 to 11 below as one script.
- `vm/make-kernel-config.sh` is step 6.
- `docs/vm/FIRST-BOOT-CHECKLIST.md` lists the output each step should print.
- `tools/ntpers_kpatch.py` makes every kernel-tree edit in sections 3 to 5.

## 0. Status at a glance

| Step | Works from repo alone? | Blocking gap |
|---|---|---|
| 1 Fetch and verify tarball | Procedure yes, value no | G1 (SHA256 not in repo) |
| 2 Extract | yes | none |
| 3 Apply patches (replaced by the script) | yes, `tools/ntpers_kpatch.py` | G3 (enter.c todo), G4 (bit check) |
| 4 Copy NT sources | yes, in the script | G8, G24 |
| 5 Kconfig/Kbuild/fs wiring and header edits | yes, in the script | G4, G6 (closed) |
| 6 `.config` | yes, via `make-kernel-config.sh` | G7, G19 |
| 7 Build and completeness check | expected | G8, G10 |
| 8 PE binaries | yes, needs mingw-w64 | G12 |
| 9 Initramfs | yes | G15 |
| 10 Freshness stamp | yes | G13, G14 |
| 11 Boot and M0 gate | untested | G11, G16, G17 |

## 1. Kernel source

- Version: Linux **6.18**. The tarball is `linux-6.18.tar.xz` and it unpacks to
  a directory named `linux-6.18`. `vm/rebuild_kdir.sh` and `vm/kbuild.sh` both
  assume that directory name (`TARBALL` default `~/ntpers/linux-6.18.tar.xz`,
  `KDIR` default `~/ntpers/linux-6.18`).
- URL pattern: `https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.tar.xz`.
  `guest-bootstrap.sh` uses this as `KERNEL_URL`.
- Checksum procedure:
  1. Download `sha256sums.asc` from the same `v6.x/` directory.
  2. Verify it with `gpg --verify sha256sums.asc` against a kernel.org signing
     key that you have checked out of band.
  3. Read the line for `linux-6.18.tar.xz` from the verified file.
  4. Pass that 64-character lowercase value as `KERNEL_SHA256`. The script
     refuses to extract on any mismatch.
  The repository contains no checksum, so this step is a decision (G1).
- Extraction: `tar -xf linux-6.18.tar.xz -C ~/ntpers`, which produces
  `~/ntpers/linux-6.18`. This is the only tree the scripts use.

## 2. Preconditions on the checkout

- Use an LF checkout. The index stores LF, but a Windows checkout with
  `core.autocrlf=true` has CRLF in the working tree (`git ls-files --eol`
  shows `i/lf w/crlf`). The script copies CRLF sources as LF, so a CRLF
  checkout builds the same tree (G23).
- The repository root is the 9p share that `runvm.sh` exports as
  `mount_tag=share`. It must sit on the guest's own disk.

## 3. Kernel-tree edits (patches deprecated)

The patch files in `src/kernel/patches/` do not apply, so `git apply` is no
longer used. `tools/ntpers_kpatch.py` makes the same edits directly, one
anchored edit at a time. Run it from the repository root:

```sh
python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18 --check   # report only
python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18           # apply
```

`--check` prints each edit as `applied`, `applicable`, `blocked` or `todo`. The
apply run writes nothing unless every edit is applied or applicable. The
patches are marked deprecated in `src/kernel/patches/README.md`.

Why the patches fail (kept as a record; G2, G3):

- 0001, the `syscall_64.c` hunk: the header says `-84,10 +84,22`, but the body
  has 11 old and 25 new lines (14 added). Counted by hand, not by `git apply`.
- 0001 carries placeholder `index 1111111..2222222` lines.
- 0002 has three bare `@@` headers. Its `audit.h` hunk expects `EM_X86_64` in
  that header; the script anchors on the `AUDIT_ARCH_X86_64` line instead.

Where the script departs from the patch text:

- The NT branch in `do_syscall_64()` calls `enter_from_user_mode()` before
  `do_nt_syscall()` (section 5i). Patch 0001 omits it, so its exit would run
  without the matching entry.
- The TIF bits are checked against the tree and not assumed free (G4).
- The `enter.c` hooks from patch 0002 are a `todo` (G3).

The patch header says "Verified against linux-6.18". Nothing in the repository
supports that (G20).

## 4. Copy the NT sources (done by `ntpers_kpatch.py`)

1. `arch/x86/nt/*` (11 files) is copied to `KDIR/arch/x86/nt/`, with CRLF
   converted to LF.
2. The `src/kernel/nt/` files that the build needs are copied as well. The set
   comes from the `obj-$(CONFIG_NT_PERSONALITY) += X.o` lines in
   `arch/x86/nt/Makefile`, followed through quoted `#include "x.h"` lines in
   both directories. The names `dispatch.c nt_io.c nt_services.c proc_thread.c
   nt_internal.h weak.c Makefile` always come from arch. Any other name found in
   `src/kernel/nt/` overwrites the arch copy, as `rebuild_kdir.sh` does. Each
   overwrite is reported as `WARN G24`.
3. `arch/x86/nt/stubs.c` is written empty (generated).
4. `src/ntdll/*.h` is copied to `KDIR/arch/ntdll/`.
5. `src/kernel/fs/binfmt_pe.c` is copied to `KDIR/fs/binfmt_pe.c`.

Not copied: `registry.c` (no object in the Makefile reaches it, G9), the
`*_test.c` files, and every other file in `src/kernel/nt/` that the build does
not need.

## 5. Wiring and header edits (done by `ntpers_kpatch.py`)

Each edit is anchored. An anchor must match exactly once, and any text the
edit depends on must match its expected context. If an anchor is missing, the
run stops and names the file and the gap.

**5a. `KDIR/arch/x86/Kconfig.nt`** (written whole). The `NT_PERSONALITY` option.
The script's copy has a comment line in place of an SPDX tag; the provenance
scan reads an SPDX tag in that text as malformed.

**5b. `KDIR/arch/x86/Kconfig`**: insert `source "arch/x86/Kconfig.nt"` after the
line `endif # HYPERVISOR_GUEST` (decision G5).

**5c. `KDIR/arch/x86/Kbuild`**: append `obj-$(CONFIG_NT_PERSONALITY) += nt/`
unless it is present.

**5d. `KDIR/fs/Makefile`**: append `obj-$(CONFIG_BINFMT_PE)` followed by a tab,
`+= binfmt_pe.o`, unless `binfmt_pe.o` is present.

**5e. `KDIR/fs/Kconfig.binfmt`**: append the `config BINFMT_PE` block. It is
appended, not inserted before an `endmenu`, because `rebuild_kdir.sh` used a
marker that this file may not have. An appended `config` is valid Kconfig.

**5f. `KDIR/arch/x86/include/asm/thread_info.h`**: `TIF_NTABI` (bit 29) and
`TIF_NTSUSPEND` (bit 30) go after the `TIF_ADDR32` line. `_TIF_NTABI` and
`_TIF_NTSUSPEND` go after `_TIF_FORCED_TF`. Each bit is checked against every
`#define TIF_* n` line; a taken bit blocks the run (G4). `proc_thread.c` falls
back to 30 for `TIF_NTSUSPEND`, so 30 is the value the sources already assume.

**5g. `KDIR/include/linux/sched.h`**: after `struct thread_struct thread;`
(inside `task_struct`), add `#ifdef CONFIG_NT_PERSONALITY`, `void *nt_thread;`,
`#endif`. `object.c` reads `tsk->nt_thread`, so this field is required.

**5h. `KDIR/kernel/fork.c`**: after `err = arch_dup_task_struct(tsk, orig);`,
add `tsk->nt_thread = NULL;` under the same `#ifdef`. The arch copy would
otherwise carry the parent's pointer into the child. `rebuild_kdir.sh` placed
this before `alloc_thread_stack_node()`; the script places it after the copy.

**5i. `KDIR/arch/x86/entry/syscall_64.c`** (G6). Prototypes for `do_nt_syscall`
and `nt_can_sysret` (both defined in `arch/x86/nt/dispatch.c`) go before
`do_syscall_64`. After `add_random_kstack_offset();`, the NT branch is:
`enter_from_user_mode(regs)`, `instrumentation_begin()`,
`do_nt_syscall(regs, (unsigned int)nr)`, `instrumentation_end()`,
`syscall_exit_to_user_mode(regs)`, then `return nt_can_sysret(regs)`. The
argument is `nr`, as in `rebuild_kdir.sh`. The enter and exit calls follow the
Linux path's own bracket.

**5j. `KDIR/include/uapi/linux/audit.h` and `KDIR/arch/x86/include/asm/syscall.h`**
(patch 0002's audit change). After the `AUDIT_ARCH_X86_64` line, define
`EM_NTX86_64` (0x9040) and `AUDIT_ARCH_NTX86_64`. The run refuses if
`elf-em.h` already defines that value. In `syscall_get_arch()`, a `TIF_NTABI`
task reports `AUDIT_ARCH_NTX86_64`. This changes what seccomp and audit see for
NT tasks, but NT syscalls do not reach seccomp yet (G3).

**5k. `KDIR/arch/x86/nt/kernel_stubs.c`** (G10). Adds `#include <linux/init.h>`
and a `late_initcall(nt_banner_initcall)` that calls `nt_banner_init()`.

**5l. `KDIR/arch/x86/entry/nt/enter.c`: still a todo** (G3). Patch 0002's enter
hooks for NT syscalls (seccomp, audit and ptrace). The run stops on this edit.

Check before building: `TIF_NTABI` and `TIF_NTSUSPEND` exist in `thread_info.h`,
and `do_nt_syscall` is in `syscall_64.c`. Step 5 of `guest-bootstrap.sh` checks
these, and it also checks `nt_thread` and `nt_banner_initcall`.

## 6. Kernel `.config`

`vm/kernel-config-ntpers` does not exist in the repository, though
`kbuild.sh` and `rebuild_kdir.sh` copy it into `$KDIR/.config`. Instead, run:

```sh
KDIR=~/ntpers/linux-6.18 bash vm/make-kernel-config.sh
```

It starts from `make defconfig` for x86_64 and enables these options with
`scripts/config --enable`:

| Option | Reason (from the repository) |
|---|---|
| NT_PERSONALITY, BINFMT_PE | the NT ABI and PE loader (`kbuild.sh` required list) |
| VIRTIO, VIRTIO_PCI, VIRTIO_BLK | QEMU devices used by `runvm.sh` |
| NET_9P, NET_9P_VIRTIO, 9P_FS | the `share` mount in `init.c` |
| DEVTMPFS, DEVTMPFS_MOUNT | `/dev` mounted by `init.c` |
| SERIAL_8250, SERIAL_8250_CONSOLE | `console=ttyS0` in `runvm.sh` |
| EARLY_PRINTK | `earlyprintk=serial,ttyS0` in `runvm.sh` (not in `kbuild.sh`; G19) |
| BLK_DEV_INITRD, RD_GZIP | initramfs is a gzip cpio (`mkinitramfs.sh` uses `gzip -9`; RD_GZIP is not in `kbuild.sh`) |
| BINFMT_ELF | busybox and `init` are static ELF |
| PCI | prerequisite of VIRTIO_PCI |
| TMPFS, PROC_FS, SYSFS | mounted by `init.c` |
| MAGIC_SYSRQ | `init.c` powers off through `/proc/sysrq-trigger` |
| SECCOMP, AUDIT | the seccomp and audit ABI that patch 0002 is about |
| DEBUG_FS, DYNAMIC_DEBUG | `/sys/kernel/debug` used by `init.c` and guest tests |

After `make olddefconfig` the script requires every listed symbol to read
`CONFIG_X=y`, because olddefconfig silently drops options whose dependencies
are unmet. The config is written to `$KDIR/.config`, and the script takes an
optional path for a copy.

`tinyconfig` is not used. It lacks NET, PCI, VIRTIO and 9P, and enabling
those with their dependencies one at a time is error-prone (decision G19).

## 7. Build

```sh
cd ~/ntpers/linux-6.18
make olddefconfig
make prepare -j1          # fixdep race: if this fails, rm scripts/basic/fixdep and retry
make -j"$(nproc)" bzImage # builds vmlinux as well
bash ~/repos/ntpers/tests/kernel/check_kernel_complete.sh ~/ntpers/linux-6.18/vmlinux
```

Outputs: `arch/x86/boot/bzImage`, `vmlinux`. The completeness check looks for
`do_nt_syscall`, `nt_service_table`, `nt_enter_personality`,
`nt_exit_personality`, `ObCreateHandle`, `ObReferenceObjectByHandle`,
`nt_create_event`, `se_access_check`, `nt_svc_NtAllocateVirtualMemory`,
`nt_svc_NtCreateFile`, `nt_svc_NtCreateIoCompletion`, `nt_svc_NtCreateMutant`,
`nt_svc_NtQueryInformationFile`, `nt_banner_init`. It counts `nt_svc_*` symbols
(at least 350) and the members of `arch/x86/nt/built-in.a` (at least 17, with
17 named objects).

`kbuild.sh` and `rebuild_kdir.sh` run a full `make -j$(nproc)` that also
builds modules. Only `bzImage` is needed for the VM.

## 8. PE test binaries

`tests/kernel/bin/` is untracked and empty. `tests/kernel/Makefile` builds it
with `x86_64-w64-mingw32-gcc` (Debian package `gcc-mingw-w64-x86-64`):

```sh
make -C ~/repos/ntpers/tests/kernel
```

`mkinitramfs.sh` packs `tests/kernel/bin/*.exe` into `/tests_kernel/`. The M0
gate needs `/tests_kernel/minimal.exe`, or `minimal0.exe` as fallback
(`vm/test_m0.sh`).

## 9. Initramfs

```sh
bash ~/repos/ntpers/vm/mkinitramfs.sh ~/repos/ntpers/vm/initramfs.cpio.gz
```

What it needs:

- `gcc -static` with glibc static archives (`libc6-dev`) to compile `vm/init.c`.
- `strip`, `cpio` (newc), `gzip`, `sha256sum`, `awk`.
- `busybox` static at `/bin/busybox` or on PATH (`busybox-static`). It creates
  applet symlinks for `sh`, `mount`, `cat`, `timeout`, `dmesg`, and others.
- Optional inputs: `/tmp/pe_min.exe`, `/tmp/minimal*.exe`, `/tmp/pe_min*.exe`,
  `/tmp/{ntdll,kernel32,…}.dll`, `build/*.dll`, `tests/kernel/bin/*.exe`,
  `tests/kernel/bin/pmu_elf`. Files under `/tmp` make the output depend on
  host state (OVERVIEW known problem 11). Keep `/tmp` empty for a clean build.

Outputs: `vm/initramfs.cpio.gz`, `vm/initramfs.digest`, `vm/initramfs.sha256`,
and `vm/initramfs.identity.json`. The last file is tracked in git, so it shows
as modified after every build. The archive also contains
`ntpers.identity.json`, which has a timestamp. Two builds of the same sources
therefore give different archive bytes (G15).

## 10. Freshness stamp

```sh
NTPERS_ALLOW_WRITE=1 KDIR=~/ntpers/linux-6.18 bash ~/repos/ntpers/vm/freshness.sh write
```

`write` refuses unless `NTPERS_ALLOW_WRITE=1`, runs the completeness check on
`$KDIR/vmlinux`, and requires `bzImage` to exist. It then:

- computes a manifest of SHA256 values over `src/kernel/nt/*`, `src/kernel/fs/*`,
  `src/ntdll/*.h`, `src/ntdll/syscall_table.def`, and `arch/x86/nt/*` (minus
  generated names), and writes `.ntpers_build.{manifest,digest,identity.json}`
  to both `$KDIR` and the repo root;
- writes `.ntpers_bzimage.sha256`, `vm/initramfs.{digest,sha256}`.

`check` (run by `runvm.sh` before every guest run) requires the two manifests to
match, recomputes the manifest, and fails if any source file is newer than
`bzImage`. It does **not** compare the initramfs or bzImage hashes, and it
does not hash `src/kernel/patches/*.patch`, `vm/`, `tests/kernel/` or `build/`.
Edits to those do not make the build stale (G14).

A fresh tree has no manifest, so `check` fails until `write` has run once.
This is why the stamp must come before the M0 gate (G13).

## 11. Boot and the M0 gate

`runvm.sh` runs the freshness check, then starts QEMU with the arguments it
builds. The equivalent raw command is:

```sh
REPO=~/repos/ntpers; KDIR=~/ntpers/linux-6.18; LOG=$REPO/.dodresult/m0-serial.log
mkdir -p $REPO/.dodresult
qemu-system-x86_64 -accel kvm -accel tcg -cpu host -m 4G -smp 4 -nographic \
  -kernel $KDIR/arch/x86/boot/bzImage \
  -initrd $REPO/vm/initramfs.cpio.gz \
  -append "console=ttyS0,115200n8 earlyprintk=serial,ttyS0,115200n8 nokaslr quiet panic=1 dodtest=/mnt/share/vm/test_m0.sh" \
  -virtfs local,path=$REPO,mount_tag=share,security_model=none \
  -serial file:$LOG -device virtio-rng-pci
```

The recommended form is `runvm.sh`, which also enforces a timeout and reads
the result file:

```sh
BZIMAGE=$KDIR/arch/x86/boot/bzImage INITRD=$REPO/vm/initramfs.cpio.gz \
  bash $REPO/vm/runvm.sh --test /mnt/share/vm/test_m0.sh --share $REPO \
       --timeout 90 --log $REPO/.dodresult/m0-serial.log
```

Exit codes from `runvm.sh`: the guest's `result.exitcode`; 124 on timeout;
125 for stale build or missing initramfs; 1 if QEMU exited without a result.

The gate verdict is in `.dodresult/test_m0.json` (`"status": "PASS"` and
`"exit_code": 42`). `result.exitcode` is **0** on a pass, because `init.c`
overwrites it with the script's exit status after `test_m0.sh` writes 42. This
is by design in `check_m0.sh`. The expected serial lines are in
`FIRST-BOOT-CHECKLIST.md`.

`guest-bootstrap.sh` calls `runvm.sh` directly and applies the same verdict
as `check_m0.sh`, without the initramfs rebuild that `check_m0.sh` performs.

## 12. Automation

`vm/guest-bootstrap.sh` runs steps 0 to 11 and stops at the first failure:

```sh
KERNEL_SHA256=<value from step 1> bash ~/repos/ntpers/vm/guest-bootstrap.sh
```

It never deletes a tree. An existing `~/ntpers/linux-6.18` makes it stop.
Set `SKIP_APT=1` to skip package installation, `RUN_REG=1` to also run the REG
probe, and `VM_TIMEOUT` to change the guest timeout (default 90 s).

## Known gaps

Each gap lists the minimal decision needed. G1 stops the bootstrap at step 1
until a checksum is supplied. G3 stops step 4 by design (the enter.c todo).
G4 stops step 4 only when a bit the edits need is already in use.

**G1. SHA256 of the tarball is not in the repository.** Decision: approve the
value from the gpg-verified `sha256sums.asc` and pass it as `KERNEL_SHA256`, or
commit it as a pinned file that the bootstrap reads.

**G2. Patch 0001 does not apply. Closed** by `tools/ntpers_kpatch.py`
(commits 5b5fb47 and 3e619a3). The patch files stay as a record, marked deprecated in
`src/kernel/patches/README.md` (commit 95d2f96). `guest-bootstrap.sh` no longer
calls `git apply` (commit ce79967). A regenerated diff would need the 6.18 tree,
which the repository does not contain.

**G3. Patch 0002 does not apply, and its enter hooks are not wired. Open.** The
audit and seccomp arch change is wired (5j, commit 5b5fb47). The `enter.c` hooks
are a todo (5l) and stop step 4. `enter.c` calls `syscall_trace_enter()`, which
the repository does not declare as a kernel API, so the hooks cannot be derived
with certainty. Until they exist, an NT syscall never reaches seccomp, audit or
ptrace, so a seccomp filter cannot see or stop it. Decision: must the NT path run
seccomp and audit for M0? If yes, derive `enter.c` from the 6.18 headers and add
an `arch/x86/entry/Makefile` line for it. If no, say so in the M0 claim. The
self-test named in patch 0002 (`tools/testing/selftests/ntpers/seccomp_arch.c`)
does not exist in the repository.

**G4. Bit assignments in `thread_info.h` are unverified. Open, checked at run
time.** The script uses 29 for `TIF_NTABI` and 30 for `TIF_NTSUSPEND`, and it
blocks if either bit is already defined in the tree. The repository does not
hold 6.18's `thread_info.h`, so whether 29 and 30 are free is unknown. Upstream
numbering may put `TIF_ADDR32` at 29; that would block the run, as intended.
Decision: choose two free bits from `arch/x86/include/asm/thread_info.h` in 6.18,
then change `NTABI_BIT` and `NTSUSPEND_BIT` in `tools/ntpers_kpatch.py`.
`proc_thread.c` falls back to 30 for `TIF_NTSUSPEND` and would need the same
change if the bit moves.

**G5. The wiring existed only as inline code. Closed** by the script's section 5
(commits 5b5fb47 and 3e619a3). The Kconfig, Kbuild, fs, `sched.h`, `fork.c` and `thread_info.h`
edits are anchored edits, and the source line goes after `endif # HYPERVISOR_GUEST`,
as `rebuild_kdir.sh` intended. The `fork.c` edit is placed after the task copy (5h).
Not changed here: `kbuild.sh` still calls scripts that do not exist
(`create_kconfig_nt.py`, `add_suspend.py`, `fix_syscall.py`, `fix_syscall_orig.py`,
`scratch/add_fs_*.py`), and `vm/rebuild_kdir.sh` still holds the old inline copy.
Their failures are silenced by `|| true` or `2>/dev/null`.

**G6. Two `syscall_64.c` dispatch forms. Closed** (commits 5b5fb47 and 3e619a3). One form is
used (5i): the NT branch takes `nr` as `rebuild_kdir.sh` did, and it brackets the
dispatch with `enter_from_user_mode()` and `syscall_exit_to_user_mode()`, as the
Linux path does. Patch 0001 used `regs->ax` and omitted the entry call. The change
is unbooted (G22).

**G7. `vm/kernel-config-ntpers` is missing.** `kbuild.sh` and `rebuild_kdir.sh`
both copy it. Decision: commit a config generated once by
`make-kernel-config.sh` under that name, or make `kbuild.sh` call the script.

**G8. The object count disagrees across the repository.** `arch/x86/nt/Makefile`
lists 20 objects. `kbuild.sh` checks those 20 names and then prints
"Makefile OK (18 objects)". `check_kernel_complete.sh` expects at least 17
objects in `built-in.a` and names 17. `OVERVIEW.md` says 17. Decision: the
number, and the single list the checks use.

**G9. `registry.c` is linked only by `rebuild_kdir.sh`. Closed: not built**
(commits 5b5fb47 and 3e619a3). The tracked Makefile does not list it, and `registry.h` is
included only by `registry.c` and `registry_test.c`, so no built object needs it.
The script does not copy `registry.c`. A link error for a registry symbol at the
first build (G22) would reopen this.

**G10. `nt_banner_init` had no caller. Closed** (commits 5b5fb47 and 3e619a3). The script
appends a `late_initcall` to `kernel_stubs.c` that calls it (5k). The banner
therefore prints once the initcalls run, which is after the early boot messages.

**G11. `runvm.sh` passes `quiet`.** With `quiet` the console loglevel drops
below KERN_INFO. `binfmt_pe.c` logs most of its lines with `pr_info`, so they
reach dmesg but not the serial log. Its `pr_err` lines do reach the serial log.
Decision: remove `quiet` for diagnostic runs, or read dmesg through
`vm/diag_m0.sh`, which writes `m0_diag_dmesg.log`.

**G12. PE binaries are untracked.** Without step 8 the initramfs has no
`/tests_kernel/minimal.exe` and the M0 gate fails. Decision: accept step 8 as
part of the build (the bootstrap does), or commit the binaries (not
recommended).

**G13. The gate order in `kbuild.sh` cannot pass on a fresh tree.** `kbuild.sh`
runs `check_m0.sh`, which calls `runvm.sh`, before `freshness.sh write`.
`runvm.sh` refuses without a manifest (OVERVIEW known problem 14). The bootstrap
stamps first. Decision: change the order in `kbuild.sh` to match, or keep the
two paths different.

**G14. The freshness manifest omits patches and `vm/`.** `compute_manifest`
does not hash `src/kernel/patches/*.patch`, `tools/ntpers_kpatch.py` or the
kernel-tree edits it makes. Editing either does not stale the build. Decision: add the patch
files and the script to `compute_manifest`.

**G15. Initramfs bytes are not reproducible.** `mkinitramfs.sh` embeds a
timestamp in `ntpers.identity.json`, and `vm/initramfs.identity.json` is
tracked. Decision: drop the timestamp from the archive, or accept that the
initramfs hash changes on every build.

**G16. Nested KVM is needed for the default timeouts.** `runvm.sh` falls back to
TCG when `/dev/kvm` is unusable, with 4 GB and 4 vCPUs. The 90 s M0 timeout was
chosen for KVM. Decision: the timeout under TCG, or run the gate on the host.

**G17. Checkout modes and `vm/` paths.** `git ls-files -s vm/*.sh` shows mode
100755 for every script in the index, so the mode problem in `OVERVIEW.md`
(known problem 1, "100644") is out of date for the index. A checkout that drops
the exec bit still makes `runvm.sh` skip its freshness check and `kbuild.sh`
skip the initramfs rebuild, because both test `-x`. Decision: whether the
guest checkout keeps modes. The bootstrap calls every script through `bash`, so
it does not depend on them. `OVERVIEW.md` should be corrected in the same
change.

**G18. Default guest script is missing.** `runvm.sh` and `init.c` default to
`/mnt/share/vmtests/smoke.sh`, which is not in the repository. M0 passes its
own script explicitly and is not affected. Decision: none for M0; decide later.

**G19. The option list is not yet proven.** `make-kernel-config.sh` adds
RD_GZIP, EARLY_PRINTK, MAGIC_SYSRQ, BINFMT_ELF and PCI, which `kbuild.sh` does
not check. They are inferred from the scripts and have not been built. A
dependency that olddefconfig drops fails the script, which is the intent.
Decision: keep the list after the first build, or shrink it.

**G20. Patch 0001's "Verified against linux-6.18" claim. Closed: withdrawn.**
`src/kernel/patches/README.md` says the claim is not supported. Nothing in the
repository records a verification.

**G21. Correction to the task description.** `build/Makefile` does not define
`NTPERS_LINUX`. The default `~/ntpers/linux-6.18` is in `build/fix_gsbase.py`
and `build/fix_iosb.py`, which read the variable from the environment. The
recipe uses `KDIR` throughout.

**G22. First boot is untested.** Nothing here was built or booted. The checklist
lists what should appear, and the first run may contradict it.

**G23. CRLF in the working tree. Partly closed.** The kernel-tree copies are
normalised to LF by `tools/ntpers_kpatch.py` (commits 5b5fb47 and 3e619a3), so the kernel
edits no longer depend on the checkout's line endings. The shell scripts are not
normalised: a CRLF `vm/guest-bootstrap.sh` fails under bash. Decision: use a clone
with `core.autocrlf=false` for the guest, or normalize the share.

**G24. Two NT sources exist in both trees. Open.** `arch/x86/nt/mm.c`, `mm.h`
and `nt_service_list.h` also exist in `src/kernel/nt/`. The script copies the
`src/` version over the arch copy, as `rebuild_kdir.sh` does for names outside its
skip list. The two `mm.c` files differ in size (67 KB and 146 KB), and which one is
canonical is not decided. Each overwrite is reported as `WARN G24`. Decision: keep
one copy in the repository and remove the other.
