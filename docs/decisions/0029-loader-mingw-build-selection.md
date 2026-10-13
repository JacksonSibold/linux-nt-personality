# 0029: MinGW build compiles only loader stubs (LDR-1)

Status: Implemented in 10f42c8 (LDR-1). The MinGW build compiles the real loader: `LDR_USE_POSIX` (ldr.c) selects the platform layer, and the stub block is gone. Host-verified: `make -C src/ntdll check` links `ntdll.dll` with `--no-undefined`, 372 names match the `.def`, and `tests/ntdll-host/check_ldr_stub.sh` reports the real `LdrLoadDll` (115 instructions). Not verified: the DLL has not been run. Win32 `LoadLibrary` still fails, because `kernel32.c` passes a NULL context (K32-4, still open and a prerequisite of the end-to-end path).
Finding: `docs/review/ntdll.md` LDR-1 (High).
Date: 2026-10-12

Build recipe added: `src/ntdll/Makefile` (commit `94b2a7c`) links `ntdll.dll` from the MinGW sources, with the `.def` from `build/gen_ntdll_def.py` and `build/generated_stubs.S`. `tests/ntdll-host/check_exports.sh` (commit `cd3de58`) compares the exports with the `.def`. Verified by `objdump` only; the DLL was not run.

- Stubs still linked: `objdump -d` on `ldr.o` shows `LdrLoadDll` as `mov $0xc00000bb,%eax; ret` (STATUS_NOT_SUPPORTED), and `LdrGetDllHandle` as STATUS_DLL_NOT_FOUND. The `__MINGW32__` guard at `ldr.c:83` is unchanged, so this decision is still open.
- The generator could not be run on this machine: no Python interpreter is installed (`python3` is the Windows Store stub). The export check was run against the checked-in `src/ntdll/ntdll.def`, copied into `out/`. Re-run `make -C src/ntdll check` with Python 3 to check the generated file.

## Context

`src/ntdll/ldr.c` selects its implementation with `__MINGW32__`:

- Line 83 (`#ifndef __MINGW32__`) opens the POSIX implementation. Its matching `#else` is at line 1802. Lines 1802-1827 are the stubs: `LdrLoadDll` returns `STATUS_NOT_SUPPORTED`, `LdrGetDllHandle` returns `STATUS_DLL_NOT_FOUND`, `winldr_ctx_create` returns NULL.
- The `__MINGW32__` NT-syscall helpers (`ldr_read_file` at line 625, `ldr_alloc_image` at line 756, and the `__MINGW32__` branches at lines 1559-1571) sit inside the excluded block. They are dead for the target.
- Line numbers differ from the review (base `7e75069`) because the bounds commits added lines. The cited structure is unchanged.

The repository has no recipe that links `ldr.c` or `ntdll.dll`. `build/Makefile` builds only the NTSTATUS table and the test drivers. `src/ntdll/ldr.c` is not in `tests/ntdll-host/Makefile` either. The MinGW ntdll link therefore lives outside this tree, so the file selection cannot be checked here. `ldr_test.c` needs `x86_64-w64-mingw32-gcc` for its fixtures, and that compiler is not installed on this machine.

Effect: every Win32 `LoadLibrary`, `GetModuleHandle` and `GetProcAddress` fails in the MinGW build. `kernel32.c` (lines near 225-271) also passes a NULL context (K32-4), so the stubs are not the only problem.

## Options

1. **Guard flip in place.** Change line 83 so the MinGW build also compiles the core, and route the platform calls (file read, image allocation, locks) through the `__MINGW32__` helpers. Tradeoff: one file gets two platform layers behind `#ifdef`, and the host build still needs POSIX headers.
2. **Split the platform layer.** Move the POSIX file, map and lock calls into `ldr_host.c`, and the NT calls into `ldr_nt.c`. Keep the parser, list, hash and resolve logic in `ldr_core.c`, shared by both. The host tests link `ldr_host.c`; the ntdll link uses `ldr_nt.c`. Tradeoff: the largest change, and every build recipe that names `ldr.c` must change.
3. **Keep the stubs, and move the loader to `LdrInitializeThunk`.** Route `Ldr*` calls to `ntdll_main.c`'s startup loader (the separate loader in `ntdll-startup.md`). Tradeoff: two loaders share no state, and `ntdll-startup.md` already names this as a gap.

## Recommendation

Option 2. It keeps one implementation of the import, export and relocation logic, and it gives the host tests a path that does not depend on the NT target. It also removes the dead-code branches (LDR-12) by construction.

This is not a small or local change, and the NT side cannot be verified without the real personality kernel. It is therefore deferred. Before starting it, first fix the process-wide context (K32-4), because the stubs fail regardless of the file layout.

## Patch sketch

- New `src/ntdll/ldr_core.c`: everything between the current `ldr.c:83` and `ldr.c:1801` that does not call POSIX or NT directly.
- New `src/ntdll/ldr_nt.c`: the `__MINGW32__` helpers (`ldr_read_file`, `ldr_alloc_image`, lock and TLS helpers) using `nt_syscalls.h`.
- `ldr_host.c`: the existing POSIX calls, under `#ifndef __MINGW32__`, used only by `ldr_test.c` and the host build.
- `ldr.h`: unchanged public API. Remove the `__MINGW32__` stubs from `ldr.c` once the core is linked.
- Build: add the new objects to the ntdll link (outside this repo) and to the Linux test build.

## Test plan

- Host (after the split): the core and `ldr_host.c` build with `gcc -fsyntax-only -std=c11 -Isrc/ntdll -Isrc/loader` and link without the stubs. `ldr_test.c` still passes (it needs the MinGW cross compiler for fixtures, so it is run on a machine that has it).
- Host: `nm` on the core object shows no `Ldr*` stub symbols.
- Needs VM: from a Win32 process on the built ntdll, `LoadLibraryA("kernel32.dll")` returns a non-NULL module and `GetProcAddress` returns a non-NULL address for a known export. The test uses the real personality kernel, so it cannot run on the host.

## Not verified

- The ntdll link recipe is not in this repository, so the claim that the MinGW build compiles only the stubs was checked by reading the `#ifndef` structure, not by a build.
- The NT syscall wrappers in `nt_syscalls.h` were not checked for the calls the `__MINGW32__` helpers need.

## Outcome (10f42c8)

- Option 2 was not taken as written. The platform layer is selected in one file by `LDR_USE_POSIX` rather than split into `ldr_core.c`, `ldr_nt.c` and `ldr_host.c`. The NT-syscall helpers (`ldr_read_file`, `ldr_alloc_image`, `ldr_file_exists`, `ldr_free_image`) that already existed under `__MINGW32__` are now the MinGW layer.
- The pure parts are in `src/ntdll/ldr_sched.h` with host tests in `tests/ntdll-host/test_ldr_sched.c`: name normalisation, search order, API-set line parsing, the ordinal parser, reference counts, the forwarder depth limit and the attach result rule.
- The MinGW build needed three things the POSIX build never did: the `PACCESS_MASK` and `GENERIC_MAPPING` types (355ad0c, the build was already failing), `src/loader/pe_parse.c` in the link (with `PE_FREESTANDING`, so its diagnostics do not use `stderr`), and no `munmap`, `fopen`/`fgets`, `strtoul` or `sscanf` in the loader body (the freestanding runtime has none of them).
- Still open: `ldr_alloc_image` commits anonymous memory and copies the image. It does not use `NtCreateSection` with `SEC_IMAGE` and `NtMapViewOfSection`, which the design also names. That needs the personality kernel to verify.

## Needs the Linux guest or a running personality (not verified in this repository)

- The POSIX layer (`LDR_USE_POSIX`=1) was checked only with `gcc -fsyntax-only` against stub `sys/mman.h`. It was not linked or run. `src/ntdll/ldr_test.c` (the POSIX conformance test with MinGW fixtures) has not been run.
- Running the MinGW `ntdll.dll`, the NT file and image helpers (`NtCreateFile`, `NtReadFile`, `NtAllocateVirtualMemory`), and `LdrLoadDll` against real DLLs.
- The Windows Application Control policy blocks freshly built exes on this host, so no built PE is run here.
