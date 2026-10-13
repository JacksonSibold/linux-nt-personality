# vm - NT Personality VM and Build

This directory contains the VM and kernel build infrastructure for the NT personality.

Commands below use `$NTPERS_ROOT`, the repository root as a WSL path (for example `/mnt/c/<your-checkout>`). Export it before running them.

## Quick Start for Agents

**You must run these two scripts - no exceptions.**

1. **Before starting any work**, run the preflight check:

   ```bash
   wsl -d Ubuntu -- bash -c "~/ntpers/linux-6.18/vm/preflight.sh"
   # or from project root via WSL:
   wsl -d Ubuntu -- bash -c "'$NTPERS_ROOT/vm/preflight.sh'"
   ```

   `preflight.sh` verifies:
   - `.config` exists with all required options (`CONFIG_NT_PERSONALITY=y`, `CONFIG_BINFMT_PE=y`, `VIRTIO`, `9P_FS`, etc.)
   - `arch/x86/nt/Makefile` contains all 17 objects (not truncated)
   - No duplicate strong symbols across `arch/x86/nt/*.c` (e.g., the `g_kshim_root` bug that caused NT paths to resolve under `/tmp` instead of `/`)
   - The tree still builds

   If preflight fails, **do not proceed**. Fix the tree first. Each failure consumes most of a cycle and has broken the build in four consecutive waves.

2. **To build the kernel**, use ONLY `kbuild.sh`:

   ```bash
   wsl -d Ubuntu -- bash -c "'$NTPERS_ROOT/vm/kbuild.sh'"
   # or
   ~/ntpers/linux-6.18/vm/kbuild.sh  # if linked
   ```

   `kbuild.sh`:
   - Restores `.config` from `vm/kernel-config-ntpers` if missing (e.g., after `make clean`)
   - Verifies required `CONFIG_*` options
   - Verifies `arch/x86/nt/Makefile` has 17 objects
   - Runs `make -j$(nproc)` in `~/ntpers/linux-6.18`
   - **Refuses to report success** unless `tests/kernel/check_kernel_complete.sh` passes (checks for 17 objects, 369+ `nt_svc_*` symbols, and required symbols like `nt_banner_init`, `do_nt_syscall`, etc.)

   **Do not run `make` directly.** `kbuild.sh` is the only sanctioned build method. It enforces the completeness check that catches truncated Makefiles and missing weak stubs.

## Files

- `kernel-config-ntpers` - Known-good `.config` with all required options. If `make clean` deletes `.config`, `kbuild.sh` restores this file.
- `kbuild.sh` - Sanctioned build script (see above).
- `preflight.sh` - Pre-work sanity check (see above).
- `runvm.sh` - Boots the kernel in QEMU with 9p share (used by `tests/kernel` harness). `--transport initrd-serial` (or `VM_TRANSPORT=initrd-serial`) uses the serial console instead; see [docs/vm/TRANSPORTS.md](../docs/vm/TRANSPORTS.md).
- `mkinitramfs.sh` - Builds the initramfs from `init.c` and PE probes. With `VM_TRANSPORT=initrd-serial` it also packs the payload directories under `/mnt/share`.
- `init.c` - Guest init that runs `M0` and other tests. In `initrd-serial` mode it prints the results with `emit-results.sh`.
- `emit-results.sh` - Guest-side framed emitter for `initrd-serial` (busybox applets only). The host side is `tools/vm_serial_collect.py`.

## Why This Exists

The build has broken in four consecutive waves. Each time it consumed most of a cycle. Instructions alone did not prevent it. These scripts make the protection **durable and automatic**:

- `kernel-config-ntpers` prevents `make clean` from leaving the tree unconfigured.
- `kbuild.sh` prevents silent truncation of `arch/x86/nt/Makefile` (which turns services into `STATUS_NOT_IMPLEMENTED`) from being reported as success.
- `preflight.sh` catches duplicate strong symbols (e.g., `g_kshim_root` defined in both `kshim.c` and `io.c`) immediately via `nm`, and verifies the tree is sane before work begins.

**Leave the tree building.** That is the only measure of success.

## M0

`M0` is a minimal PE that exits 42. It is the first proof the NT personality correctly handles `NtTerminateProcess`.

Build and run:

```bash
vm/kbuild.sh
vm/runvm.sh  # or via tests: wsl -d Ubuntu -- bash -c "python3 tests/harness/vmtest.py ..."
```

Check `dmesg` for the NT banner (`nt_banner_init`) and verify M0 exits 42.

## Checker

`tests/kernel/check_kernel_complete.sh` is the host-side guard against truncated NT Makefiles. It:

- Runs `nm vmlinux` and checks for expected symbols (`do_nt_syscall`, `nt_service_table`, `nt_svc_Nt*`, etc.)
- Counts `nt_svc_*` symbols (expects >=350, currently 369)
- Checks `arch/x86/nt/built-in.a` contains 17 objects

Agent S1 fixed this checker (avoiding `SIGPIPE` via temp file, resolving `KDIR` correctly) and proved it catches a truncated Makefile.

Never disable this check.

