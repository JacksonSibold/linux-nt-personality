# 0029: MinGW build compiles only loader stubs (LDR-1)

Status: Proposed. Recorded only, not implemented. Needs VM: the fix changes what the real ntdll build links, and nothing in this repository can build or run that target.
Finding: `docs/review/ntdll.md` LDR-1 (High).
Date: 2026-10-12

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
