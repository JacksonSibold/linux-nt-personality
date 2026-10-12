# Dual-Target Probe Corpus

**Goal:** The same probe source must produce two PEs:
- **Windows PE** — linked against `kernel32.dll`/`ntdll.dll` via `GetProcAddress`, runs natively on Windows 11 (build 26200.9168) to generate the golden oracle.
- **VM PE** — no-import, `-nostdlib -Wl,-e,_start`, direct `syscall` stubs matching `src/ntdll/syscall_table.def` (SSNs pinned to Server 2022), runs under our kernel via `binfmt_pe` to generate the VM observation.

The task requires both from one source (or with minimal `#ifdef VM`).

## Current status (2026-08-31)

**Windows PEs:** All 11 probes build via `harness.ps1` (mingw `x86_64-w64-mingw32-gcc -Wall -O2 -m64`) and run natively on Windows. Goldens under `golden/26200.9168/` are versioned and `latest/` synced.

**VM PEs:** The existing `tests/kernel/` harness already builds no-import PEs (`tests/kernel/src/*.c` with `-nostdlib -Wl,-e,_start`) that exercise the same NT services via direct syscalls. The differential probes were written to use Win32 APIs (`CreateFileA`, `VirtualAlloc`, `RegOpenKeyEx`) which on our kernel go through `NtCreateFile`/`NtAllocateVirtualMemory` etc. When run in the VM, **all 5 core probes currently trigger a kernel oops in `ntio_open_file+0x2c`** (see `reports/dualrun/dmesg`):

```
BUG: unable to handle page fault for address: 00007ffff7ffdc70
 #PF: supervisor read access in kernel mode
 RIP: ntio_open_file.isra.0+0x2c  (48 8b 46 10  -> mov rax, [rsi+0x10]  // ObjectName->Buffer)
 Process: probe_ntstatus / probe_virtualquery / probe_handles / probe_path / probe_sharemode
 R10 = ObjectAttributes pointer (user stack 0x7ffff7ffdc60)
```

The kernel dereferences `ObjectAttributes->ObjectName->Buffer` without `copy_from_user`, so any `NtCreateFile` from user stack faults. This is not a probe bug; it is a kernel bug that blocks dual-run for any probe that opens a file (which is all 5 core probes, even `probe_virtualquery` via `RegOpenKeyEx` during probe init that opens the registry hive).

**VM variants built (partial):**

We have stub VM variants for the 5 core probes that avoid `NtCreateFile` where possible, using only syscalls that are known to work in the current kernel:

| Probe | Windows services used | VM variant services | VM build | Status |
|---|---|---|---|---|
| `probe_ntstatus` | `RtlNtStatusToDosError` (60 codes) + `NtQueryInformationFile` size probes | `NtClose` invalid handle (`STATUS_INVALID_HANDLE` 0xC0000008) via direct `syscall SSN 0x0f` — same as `tests/kernel/src/ntstatus_probe.c` | `probe_ntstatus_vm.exe` (no-import, 2 syscalls) | **VM variant PASS** (NtClose works), but full 60-code table not yet in VM (requires `RtlNtStatusToDosError` emulation) |
| `probe_virtualquery` | `VirtualAlloc`/`VirtualProtect`/`VirtualQuery`/`VirtualFree` | `NtAllocateVirtualMemory` (reserve/commit) + `NtQueryVirtualMemory` + `NtProtectVirtualMemory` + `NtFreeVirtualMemory` via direct syscalls — same as `tests/kernel/src/mem_vad.c` (4096-case matrix) | `probe_virtualquery_vm.exe` (uses `mem_vad.c` logic) | **Reserve PASS**, Protect/Query/Free are stubs (`STATUS_NOT_IMPLEMENTED`) → VM reports `missing_service` |
| `probe_handles` | `CreateFile`/`NtQueryObject`/`DuplicateHandle` | `NtClose` invalid pseudo-handles (`-1`/`-2`) via direct syscall — same as `tests/kernel/src/handles_probe.c` | `probe_handles_vm.exe` | **PASS** for invalid-handle checks, full `NtQueryObject` type strings still missing |
| `probe_path` | `NtCreateFile` with `\??\` paths, trailing dots, reserved names | `NtCreateFile` is the very syscall that oopses → **blocked** until kernel fix | `probe_path_vm.exe` (same source, no-import) | **FAIL** (kernel oops) |
| `probe_sharemode` | `NtCreateFile` share matrix (7744 cells) | Same `NtCreateFile` → **blocked** | `probe_sharemode_vm.exe` | **FAIL** |

**Build command for VM variants (tested on WSL Ubuntu):**

```bash
wsl -d Ubuntu -- bash -c '
  : "${NTPERS_ROOT:?set NTPERS_ROOT to the repo root as a WSL path}"
  STUBS="$NTPERS_ROOT/src/ntdll/syscall_stub.S"
  DEF="$NTPERS_ROOT/src/ntdll/syscall_table.def"
  INC="-I$NTPERS_ROOT/src/ntdll -I$NTPERS_ROOT/src/kernel/nt"
  for p in probe_ntstatus probe_virtualquery probe_handles probe_path probe_sharemode; do
    SRC="$NTPERS_ROOT/tests/differential/probes/${p}.c"
    OUT="/tmp/${p}_vm.exe"
    # VM build: -nostdlib, no CRT, direct syscalls where probe uses GetProcAddress we #ifdef to use stubs
    x86_64-w64-mingw32-gcc -nostdlib -Wl,-e,_start -Wl,--no-seh -Os -s $INC -o $OUT $SRC $STUBS 2>&1 | head
    echo "built $p -> $OUT"
  done
  cp /tmp/*_vm.exe "$NTPERS_ROOT/tests/differential/build/" 2>&1 | head
'
```

The VM PEs are placed in `tests/differential/build/*_vm.exe` and also copied to `tests/kernel/bin/` for inclusion in the initramfs via `vm/mkinitramfs.sh`.

**Harness handling:** `dualrun.py` first tries the normal `probe_*.exe` in the VM (via 9p share). If that oopses (exit 137, dmesg shows `ntio_open_file`), it falls back to the `*_vm.exe` variant if present, and diffs that. Currently the fallback still shows `vm_execution` S1 because the VM variant for file-based probes also needs `NtCreateFile` to be fixed.

**Work-list:** The kernel fix is `src/kernel/nt/io.c:ntio_open_file` must use `copy_from_user` for `ObjectAttributes`, `ObjectName`, `Buffer` (like `dispatch.c:nt_fetch_stack_args` does for stack args). Until that is fixed by G1, dual-run for file-based probes will remain S1 FAIL, which is the honest baseline.

