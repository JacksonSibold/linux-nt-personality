# Console milestone (v0.4.0): console exe runs end to end

Goal: a console PE that prints text and writes a file runs on the booted Linux 6.18.56 NT personality.
Each step is one commit, with the exact serial output from the boot that passed it. Boots use host QEMU
(TCG, `-kernel`, no network) with the initramfs from `tools/linux-env/step-initramfs.sh`. Serial logs are
kept in `C:\Users\Jackson\repos\linux-env\kernel-s1\s1-serial.log` (host, not committed).

| Step | Scope | Status |
|---|---|---|
| S1 | nostdlib exe: raw `NtWriteFile` to StandardOutput, `NtTerminateProcess` | PASS |
| S2 | exe imports the real `ntdll.dll` (LdrLoadDll) | PASS |
| S3 | kernel32 subset: `GetStdHandle`/`WriteFile`/`ExitProcess`, then `CreateFileA`/`WriteFile`/`ReadFile` in tmpfs | PASS |
| S4 | `printf`/`puts` through the msvcrt shim | PASS |

## S1: nostdlib exe on the NT syscall interface

Fixture: `tests/kernel/src/console_s1.c` (built as `tests/kernel/bin/console_s1.exe`). It reads
`StandardOutput` from `PEB->ProcessParameters` (PEB arrives in RCX), calls `NtWriteFile` (service 0x08)
with `"hello from NT\n"`, then `NtTerminateProcess(-1, status)` (service 0x2c). Exit status is the
`NtWriteFile` status.

Kernel change: `binfmt_pe` installs the exec-ing task's fd 1 as a File handle in the NT handle table
(`ntio_install_std_file`, `arch/x86/nt/nt_io.c`) and writes it to `RTL_USER_PROCESS_PARAMETERS.StandardOutput`
(+0x28). Before this, the parameter block was an empty page (process-creation.md, section 1, step 9).

Limits: only StandardOutput is wired. StandardInput and StandardError stay 0. The handle table is global
with 960 usable slots, so each exec uses one slot and never frees it. A per-process table is needed
before any long test run.

Serial (boot of 2026-10-13, `expected 0`):

```
binfmt_pe: StandardOutput handle=0x100
binfmt_pe: ntdll not mapped, falling back to exe entry
binfmt_pe: entry 0x140001000 sp 0x7ffff7fff000 teb 0x7ffff7dfb000 peb 0x7ffff7dfd000 delta 0x0
binfmt_pe: loading PE /tests_kernel/console_s1.exe pid=25
binfmt_pe: after nt_enter TIF_NTABI=1
hello from NT
STEP exit status: 0 (expected 0)
STEP PASS
```

Result: S1 PASS.

## S2: exe imports the real ntdll.dll

Fixture: `tests/kernel/s2/console_s2.c`, built by `make -C tests/kernel s2` into `bin/console_s2.exe`. It
imports `NtWriteFile` and `NtTerminateProcess` from `ntdll.dll`. The import library comes from
`src/ntdll/out/ntdll.def` through `dlltool`. The exe reads StandardOutput from `gs:0x60` (PEB), then
calls the import.

ntdll: `make -C src/ntdll` failed to link on an undefined `__mingw_vfprintf`, which the MinGW inline
`fprintf` in `pe_parse.c` calls. `ntdll_runtime.c` now has a no-op stub for it. Built ntdll: 479 `T`
symbols, including `LdrInitializeThunk`, `LdrLoadDll`, `NtWriteFile`.

Why M0 printed "ntdll not mapped": the root cause was the initramfs, not the kernel. Neither
`/ntdll.dll` nor `/windows/system32/ntdll.dll` was in it, so `pe_map_ntdll` returned -ENOENT. The K-5
check (root-owned, not group or world writable) also applies to the file, so `step-initramfs.sh` packs
every entry with `cpio --owner 0:0`. That check was not exercised with a non-root owner in this step.
No kernel change was needed.

The initramfs puts ntdll at `/windows/system32/ntdll.dll`. `LdrInitializeThunk` only looks there
(MAIN-5), so both the kernel and the thunk find the same file.

Serial (boot of 2026-10-13, `expected 0`). Lines beginning `nt console[-11]:` are ntdll's own debug output,
which the kernel routes to the pseudo handle -11 (printk, `nt_io.c`):

```
binfmt_pe: StandardOutput handle=0x100
binfmt_pe: ntdll mapped section 0 vaddr=0x1000 vs=0xb0f0 prot=0x5 at 0x2ca201000
binfmt_pe: mapped ntdll at 0x2ca200000 LdrInitializeThunk 0x2ca202530
binfmt_pe: entry 0x2ca202530 sp 0x7ffff7fff000 teb 0x7ffff7dfb000 peb 0x7ffff7dfd000 delta 0x0
binfmt_pe: loading PE /tests_kernel/console_s2.exe pid=25
nt console[-11]: Ldr: import dll_name= ntdll.dll
nt console[-11]: Ldr: patched IAT for ntdll.dll
nt console[-11]: Ldr: calling TLS and jumping to entry
hello from NT via ntdll
STEP exit status: 0 (expected 0)
STEP PASS
```

Result: S2 PASS.

## S3: kernel32 subset with a file in tmpfs

Build: `make -C src/ntdll kernel32` links `out/kernel32.dll` from `kernel32.c` and the runtime helpers
(`ntdll_runtime.c` as `out/k32_runtime.o`: memcpy, strlen and friends are not exported by ntdll),
importing ntdll.dll. It exports 202 symbols and its only import is ntdll.dll. The first link attempt
failed on undefined `memcpy`, `strlen` and `memset`, so the runtime object was added.

Fixture: `tests/kernel/s3/console_s3.c`, built by `make -C tests/kernel s3`. It imports only from kernel32.dll.
Steps: `GetStdHandle(STD_OUTPUT)` and `WriteFile` a banner; `CreateFileA("C:\tmp\s3.txt", CREATE_ALWAYS)`
and `WriteFile` a payload; `CloseHandle`; reopen with `OPEN_EXISTING`; `ReadFile`; compare the bytes; write
them to stdout; `ExitProcess(0)`. Each failed step exits with a distinct code (10 to 17).

Initramfs: `/tmp` is a tmpfs mounted by the init (`step-initramfs.sh`). `kernel32.dll` and `ntdll.dll` go
under `/windows/system32`, where the thunk looks (MAIN-5). Path mapping `\??\C:\tmp\s3.txt` to `/tmp/s3.txt`
is in `arch/x86/nt/nt_io.c` (`kshim_ntpath_to_linux`); no kernel change was needed for S3.

Serial (boot of 2026-10-13, `expected 0`; `nt console[-11]:` lines are the DLL loader's debug output and are
omitted here):

```
binfmt_pe: StandardOutput handle=0x100
binfmt_pe: mapped ntdll at 0x2ca200000 LdrInitializeThunk 0x2ca202530
binfmt_pe: loading PE /tests_kernel/console_s3.exe pid=26
Ldr: mapped kernel32.dll
Ldr: patched IAT for kernel32.dll
Ldr: calling TLS and jumping to entry
hello from kernel32
file payload from S3
STEP exit status: 0 (expected 0)
STEP PASS
```

Result: S3 PASS.

## S4: printf and puts through msvcrt.dll

Build: `make -C src/ntdll msvcrt` links `out/msvcrt.dll` from `msvcrt.c` (it has its own runtime helpers, so
`k32_runtime.o` is not linked in: the symbols clash). It imports from kernel32.dll and ntdll.dll.
Fixture: `tests/kernel/s4/console_s4.c`, built by `make -C tests/kernel s4`. It calls `printf` and `puts`
and returns; `LdrInitializeThunk` then terminates with 0.

Loader fixes needed for S4 (src/ntdll/ntdll_main.c). Each was a real first root cause, found in order:

1. The thunk patched the exe's IAT and one level below it. msvcrt.dll's own imports from kernel32.dll (and
   kernel32's from ntdll.dll) were left raw, so the first call jumped to ip 0x33d47a. Fix: `ldr_bind_imports`
   binds a module's imports and recurses into each dependency, mapping it if needed. Each module is bound once.
2. `get_ntdll_base` probed only the preferred base 0x180000000. binfmt_pe maps ntdll at an ASLR address, so the
   loaded-module table held a bogus base for ntdll (0x20000). Fix: find the base from `LdrInitializeThunk`'s own
   address, walking down to the MZ header.

Serial (boot of 2026-10-13, `expected 0`; the lines beginning `Ldr:` are the loader's debug output, shown
here with the `nt console[-11]:` prefix removed):

```
binfmt_pe: StandardOutput handle=0x100
binfmt_pe: mapped ntdll at 0x2ca200000 LdrInitializeThunk 0x2ca202530
binfmt_pe: loading PE /tests_kernel/console_s4.exe pid=26
Ldr: mapped msvcrt.dll
Ldr: patched IAT for msvcrt.dll
Ldr: calling TLS and jumping to entry
hello from msvcrt 4 printf
hello from msvcrt puts
STEP exit status: 0 (expected 0)
STEP PASS
```

Result: S4 PASS.

Regression on the final ntdll (same kernel): S2 `hello from NT via ntdll` and S3 `hello from kernel32` /
`file payload from S3` still pass, each with exit status 0.

## Resume

Guest (`~/hello` is a sync of this worktree, `tools/linux-env/sync-repo.sh` with `GUEST_DEST=hello`):

```sh
cd ~/hello && python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18.56
cd ~/ntpers/linux-6.18.56 && nice make -j2 ARCH=x86_64 bzImage
make -C ~/hello/tests/kernel bin/console_s1.exe
sh ~/hello/tools/linux-env/step-initramfs.sh ~/hello console_s1 0 ~/ntpers/s1-initramfs.cpio.gz
```
