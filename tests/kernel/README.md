# tests/kernel - In-VM Conformance Harness

This harness runs PE binaries against the **real kernel NT personality** via `binfmt_pe`, not a userspace shim.

## Design

- PE test programs are compiled as real PE32+ binaries (`x86_64-w64-mingw32-gcc -nostdlib` or hand-built no-import PEs) and placed in the initramfs (`/pe_*.exe` and `/tests_kernel/*.exe`).
- The guest kernel executes them via `binfmt_pe` (registered in `fs/binfmt_pe.c`). The NT dispatch (`arch/x86/nt/dispatch.c`) handles syscalls through `TIF_NTABI`.
- Results are collected over the 9p virtfs share (`/mnt/share/.dodresult/kernel_*.json`) and evaluated by the host harness.
- This makes the *test subject* the actual kernel personality, satisfying `required_environment: kernel` for DoD criteria.

## Layout

- `src/` - C sources for PE tests (no-import, direct NT syscalls)
- `bin/` - built PE binaries (copied into initramfs)
- `guest/` - shell scripts executed *inside* the VM (via `dodtest=`). They launch PEs, capture ftrace, check for helper processes, CPUID, etc., and write JSON to the share.
- `harness/` - host-side Python that invokes `vmtest.py` and parses results.

## Environment

- Host: `wsl -d Ubuntu -- bash -c "..."` with QEMU/KVM, `~/ntpers/linux-6.18` bzImage.
- Guest: minimal initramfs (`vm/init.c` + `vm/mkinitramfs.sh`) with busybox, no Wine/QEMU/Box64 lineage.
- Share: QEMU `virtfs` `local,path=<project_root>,mount_tag=share`

## Suites

- **ARCH** (`arch_probe`): counterfeit detection - ftrace NT vs host syscall ratio, helper process check, CPUID hypervisor bit, X11/Wayland fd check.
- **MEM** (`mem_vad`): VirtualAlloc reserve + VirtualQuery coalescing (reports missing `NtFree/Protect/Query` if stub).
- **SYNC** (`sync_probe`): Event/Mutant/Semaphore create/set/wait (reports missing WaitMultiple).
- **HANDLES** (`handles_probe`): handle table, DuplicateHandle, pseudo-handles.
- **NTSTATUS** (`ntstatus_probe`): RtlNtStatusToDosError table via real syscalls (reports missing `RtlNtStatusToDosError` if not linked).

Where a service is `STATUS_NOT_IMPLEMENTED`, the test reports `missing_service: "NtXxx"` rather than silently skipping, per governance.

## Integration

- `tests/harness/vmtest.py` is the glue to `vm/runvm.sh`.
- `tests/harness/dodrun.py` registry entries point to `tests/kernel/harness/*.py` which exit 0 on PASS, 1 on FAIL.
- Only criteria with `required_environment: kernel` or `vm` and whose thresholds are met in-VM are mapped in `registry.yaml`.

## Usage

```bash
# Build PEs
make -C tests/kernel

# Run one suite in VM (host, via WSL)
python3 tests/harness/vmtest.py --test /mnt/share/tests/kernel/guest/run_arch.sh --timeout 120

# Full conformance
python3 tests/harness/dodrun.py --registry tests/harness/registry.yaml
```
