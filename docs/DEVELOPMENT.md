# Development guide

This guide covers running the tests and tools in this snapshot, the repository
layout, and one known gap in the kernel NTSTATUS table. For the project status
and the list of known problems, see [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Requirements

- Python 3.12. CI uses 3.12 on `ubuntu-latest`. The Python scripts use only the
  standard library, so there is nothing to install.
- Run commands from the repository root. The unit tests and `check_repo.py`
  assume that the current directory is the root.
- The kernel, VM and conformance parts need Linux or WSL, a GCC toolchain, and
  for some suites a MinGW-w64 cross compiler and a configured Linux 6.18 tree.
  They are not covered by CI and are described below only for reference.

## Running the checks that CI runs

```
python -m unittest discover tests/unit
python tools/check_repo.py
```

- `tests/unit` holds 7 test modules (91 tests). They cover the scripts in
  `build/` (`fix_gsbase.py`, `fix_iosb.py`, `gen_ntdll_def.py`, `gen_stubs.py`,
  `merge_dod.py`, `validate_criteria.py`) and
  `src/kernel/nt/gen_ntstatus_table.py`. The tests run scripts as subprocesses
  or import them, and use temporary fixtures, so they do not modify the tree.
- `tools/check_repo.py` checks the files reported by `git ls-files`: no binary
  over 1 MiB, every `.py` file compiles, every `.c` with a same-named `.h` has an
  include guard in that header, and no text file mixes CRLF and bare LF. It
  prints `FAIL [check] path: detail` lines and exits 1 on any failure. Use
  `--quiet` for the summary only. It does not run the credential pattern scan
  described in `PUBLICATION_CHECKS.md`.

## Other tools

- `tools/ntstatus_lookup.py` looks up NTSTATUS values by name or number, using
  `src/ntdll/ntstatus.h` and `src/ntdll/ntstatus_to_dos.c`. Example:
  `python tools/ntstatus_lookup.py STATUS_BUFFER_OVERFLOW 0xC0000005`.
- `tools/pe_info.py FILE` prints a PE summary. `python tools/pe_info.py --selftest`
  runs its built-in checks.

See [tools/README.md](../tools/README.md) for the full options and exit codes.

## Other test suites (not run by CI)

| Path | What it does | What it needs |
|---|---|---|
| `tests/harness/` | Definition-of-Done conformance runner (`dodrun.py`) and its self-tests (`selftest.py`), driven by `tests/conformance/criteria.json` and `registry.yaml` | Python 3, WSL/Linux, and the VM and kernel evidence for many criteria. Unregistered criteria are reported as UNIMPLEMENTED, not PASS |
| `tests/kernel/` | PE test programs run inside a guest kernel through `binfmt_pe` (`make -C tests/kernel`) | `x86_64-w64-mingw32-gcc` and the VM setup in `vm/` |
| `tests/conformance/` | `make -C tests/conformance test` builds and runs `test_nt_types.c` with `gcc -Werror` | gcc on Linux |
| `tests/differential/` | Probes and checkers compared against golden output (`make -C tests/differential check`) | gcc; the `golden/` inputs the Makefile reads are not in this snapshot |
| `build/Makefile` | Top-level targets `test`, `conformance` and `validate-criteria` | `PROJ` is a placeholder (`/path/to/ntpers-research`) and must be set or overridden |
| `vm/` | Kernel build, initramfs, boot and freshness scripts | WSL Ubuntu and a Linux 6.18 tree at `~/ntpers/linux-6.18`. Read the scripts before running them, because some rebuild that tree |
| `arch/x86/nt/Makefile` | Kernel object list for the NT personality | Only meaningful inside a kernel tree. See KNOWN-ISSUES.md |

## Repository layout

| Path | Contents |
|---|---|
| `src/loader/` | PE parser and protection mapping (`pe_parse.c`, `pe_prot.c`) |
| `src/kernel/fs/` | `binfmt_pe.c` and the kernel shim (`kshim.c`) |
| `src/kernel/nt/` | NT personality modules: object, handle, mm, io, sync, registry, security, dispatch, and their `*_test.c` drivers |
| `src/kernel/patches/` | Kernel patches that add the NT personality to a Linux tree (dispatch and seccomp audit arch) |
| `src/ntdll/` | Userspace ntdll and Win32 experiments, plus the NTSTATUS sources (`ntstatus.h`, `ntstatus_to_dos.c`) |
| `arch/x86/nt/` | Architecture-specific NT integration (`dispatch`, `mm`, `nt_io`, `proc_thread`, and related files) |
| `tests/unit/` | Unit tests run by CI |
| `tests/harness/` | Definition-of-Done conformance runner |
| `tests/conformance/`, `tests/differential/`, `tests/kernel/`, `tests/regression/` | Other test suites (see above) |
| `tools/` | Standalone Python tools (see above) |
| `build/` | Top-level Makefile and build and generator helpers |
| `vm/` | VM and kernel build scripts |
| `docs/` | Architecture, research notes, module reference (`docs/modules/`), review reports (`docs/review/`) |

## Known gap: `ntstatus_table.inc` is not tracked

`src/kernel/nt/ntstatus_kernel.c` includes `ntstatus_table.inc` at line 22. That
file is not in the repository, and nothing in the tree creates it except the
generator. Until the generator runs, the kernel NTSTATUS-to-DOS table cannot be
compiled from this snapshot.

How the generator works, from `src/kernel/nt/gen_ntstatus_table.py`:

1. It reads the userspace source of truth, `src/ntdll/ntstatus_to_dos.c`, and
   finds the array `static const NTSTATUS_TO_DOS NtStatusToDosErrorTable[] = { ... };`.
   If the array is not found, it exits with status 1.
2. It collects entries of the form `{ NAME, 123L }` with a regular expression.
   It prints the count and warns if the count is not 527, but it continues.
3. It writes `src/kernel/nt/ntstatus_table.inc` (or the path given). The file
   starts with an `AUTO-GENERATED - DO NOT EDIT` comment and a line
   `Entries: N`. The body is the array lines that contain `STATUS_`, `0x`, or
   start with `{`, with comments and blank lines removed. Other lines are
   dropped.

It does not sort the entries and does not check their order, although
`ntstatus_kernel.c` binary-searches the table, so it must stay sorted. It also does not evaluate
the `STATUS_` names, so the entry count is a check on the syntax only.

Generate the file from the repository root:

```
python src/kernel/nt/gen_ntstatus_table.py \
  --input src/ntdll/ntstatus_to_dos.c \
  --output src/kernel/nt/ntstatus_table.inc
```

When this was written, the run printed `Found 519 entries` and the
`expected 527` warning, and exited with status 0. The 527 figure is in the
script and in `tests/unit/test_gen_ntstatus_table.py`, so the current source
and the expected count disagree. This was not resolved.

Notes:

- No Makefile runs the generator. Run it by hand after editing
  `ntstatus_to_dos.c`.
- The generated `.inc` is not in `.gitignore`. Do not commit it unless the
  project decides to track it, because it would show as an untracked file.
- Regenerate the file whenever `ntstatus_to_dos.c` changes, or the kernel and
  userspace tables can drift apart.
