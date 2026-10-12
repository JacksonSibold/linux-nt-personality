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
python tools/check_doc_links.py
python tools/update_doc_counts.py --check
```

- `tests/unit` holds <!-- counts:unit_modules:begin -->25<!-- counts:unit_modules:end --> test modules (<!-- counts:unit_tests:begin -->489<!-- counts:unit_tests:end --> `test_` functions). They cover the scripts in
  `build/` (`fix_gsbase.py`, `fix_iosb.py`, `gen_ntdll_def.py`, `gen_stubs.py`,
  `merge_dod.py`, `validate_criteria.py`), `tools/` (`check_doc_links.py`, `check_repo.py`,
  `coverage_report.py`, `criteria_report.py`, `ntstatus_lookup.py`, `pe_info.py`,
  `provenance_scan.py`) and `src/kernel/nt/gen_ntstatus_table.py`. The tests run scripts as subprocesses
  or import them, and use temporary fixtures, so they do not modify the tree.
- `tools/check_doc_links.py` checks every tracked `*.md` file. It verifies
  relative links, images and reference links (the target file must exist,
  with the same letter case), and `#anchor` fragments against the headings or
  HTML ids of the target page. It skips external URLs and code. It prints
  `FAIL [doc-link] path:line` lines and exits 1 on a broken link.
  `--selftest` checks its built-in fixture.
- `tools/check_repo.py` checks the files reported by `git ls-files`: no binary
  over 1 MiB, every `.py` file compiles, every `.c` with a same-named `.h` has an
  include guard in that header, no text file mixes CRLF and bare LF, and no
  text file has leftover merge conflict markers (`<<<<<<< `, `>>>>>>> `, or a
  lone `=======` in a file that also has `<<<<<<< `). It
  prints `FAIL [check] path: detail` lines and exits 1 on any failure. Use
  `--quiet` for the summary only. It does not run the credential pattern scan
  described in `PUBLICATION_CHECKS.md`.

- `tools/update_doc_counts.py --check` compares the counts quoted in `README.md`, `CONTRIBUTING.md`, `docs/DEVELOPMENT.md`, `docs/KNOWN-ISSUES.md` and `docs/status/PROGRESS.md` with the repository. Each count sits between `<!-- counts:NAME:begin -->` and `<!-- counts:NAME:end -->` markers. It exits 1 when a block is stale, and CI runs it. `--write` rewrites stale blocks. The counts come from reading the files, not from running the tests.

## Running every local check at once

`tools/run_all_checks.sh` runs the CI checks above plus the local C and shell
suites, in this order: unit tests, `check_repo.py`, `check_doc_links.py`,
`update_doc_counts.py --check`,
`provenance_scan.py --check`, `vm/lint.sh`, `tests/syntax/check.sh`,
`make -C tests/host test`, and `make -C tests/kernel-host check`. It ends with a
PASS / FAIL / KNOWN-FAIL / SKIP matrix.

- Python is taken from `$PYTHON` if set and working, then from the WinPython
  path in the script, then from `python3` or `python`. A candidate counts only
  if it runs, so the Windows Store stub is skipped.
- The MSYS2 toolchain directories (`/c/msys64/ucrt64/bin`,
  `/c/msys64/usr/bin`) are added to `PATH` only when they exist. A section whose
  tool is missing prints `SKIP` and does not fail the run.
- Each section has a 10-minute limit. The kernel-host limit applies per module
  (`TIMEOUT=600`).
- Kernel-host modules listed on the `known-fail:` line of
  `tests/kernel-host/README.md` are reported as `KNOWN-FAIL`.
- The exit status is 1 only when a section reports `FAIL`.

The latest recorded run is in [status/TEST-MATRIX.md](status/TEST-MATRIX.md).

## Other tools

- `tools/ntstatus_lookup.py` looks up NTSTATUS values by name or number, using
  `src/ntdll/ntstatus.h` and `src/ntdll/ntstatus_to_dos.c`. Example:
  `python tools/ntstatus_lookup.py STATUS_BUFFER_OVERFLOW 0xC0000005`.
- `tools/pe_info.py FILE` prints a PE summary. `python tools/pe_info.py --selftest`
  runs its built-in checks.

See [tools/README.md](../tools/README.md) for the full options and exit codes.

## Building ntdll.dll (MinGW, build only)

```
make -C src/ntdll          # builds src/ntdll/out/ntdll.dll and out/ntdll.def
make -C src/ntdll check    # also compares the DLL's exports with the .def
```

- Needs Python 3 (for `build/gen_ntdll_def.py`) and the MSYS2 UCRT64 gcc
  (`x86_64-w64-mingw32-gcc`). The flags are listed in the header of
  `src/ntdll/Makefile`.
- The `check` target runs `tests/ntdll-host/check_exports.sh`. It uses
  `objdump` and does not load the DLL. It is not run by CI.
- Do not run the DLL or any exe built here. Windows Application Control blocks
  freshly built binaries on the development machine.
- `make` does not import `TMP` from the MSYS2 shell here. If the link fails with
  "Cannot create temporary file", pass `TMP=<dir>` on the command line.
- The loader's MinGW build still links stubs (`LDR-1`, see
  `docs/decisions/0029-loader-mingw-build-selection.md`).

## Paths and environment

Scripts find the repository from their own location, so a checkout can live
anywhere. There is no absolute path to edit. `build/*.sh`, `build/Makefile`
and `tools/` resolve the root from `$0` or `__file__`. `vm/` scripts resolve it
the same way and read `BZIMAGE` and `INITRD` from the environment when set.

| Variable | Default | Used by |
|---|---|---|
| `NTPERS_ROOT` | the parent of `build/` (the repository root) | `build/validate_criteria.py` (reads `tests/conformance/criteria.json`), `build/merge_dod.py` (reads `docs/phase0/dod-agent-*.md` and writes `tests/conformance/criteria.json`), `build/test_ntdll_pe.c` (opens `src/ntdll/syscall_table.def` when the working directory lacks it), `tests/conformance/test_pe_parse.c` (joins the probe list paths; unset means they resolve from the working directory), and the harness and differential entry points listed under "Harness root" below |
| `NTPERS_LINUX` | `~/ntpers/linux-6.18` | `build/fix_gsbase.py` and `build/fix_iosb.py` (the Linux 6.18 tree in WSL that they patch) |

Set `NTPERS_ROOT` or `NTPERS_LINUX` to an absolute path only to run a script
against a different tree, for example a copy or a fixture directory. Leave them
unset otherwise.

### Harness root

The conformance harness and the differential tools run WSL commands (freshness
checks, VM runs, copy-back of results) that need the repository root as a WSL
path. That root is resolved as follows:

* `NTPERS_ROOT` when it is set and non-empty, used as given;
* otherwise the checkout that contains the script.

A Windows drive path is converted to its WSL mount path (`C:\x\y` becomes
`/mnt/c/x/y`). A POSIX path is used unchanged, so `NTPERS_ROOT=/path/to/ntpers-research`
behaves exactly as the old hardcoded value did.

Before this change about 30 sites hardcoded the placeholder
`/path/to/ntpers-research`. None remain. The Python entry points import the
helpers in `tests/harness/_paths.py`: `repo_root()` returns the root as a
native path and `wsl_repo_root()` returns it as a WSL path. `windows_to_wsl_path()`
is the shared conversion, and `tests/harness/freshness.py` reuses it.
`tests/kernel/harness/*.py`, `tests/harness/selftest.py`,
`tests/harness/vmtest.py`, `tests/harness/dualrun_wrapper.py` and
`tests/differential/dualrun.py` all use `wsl_repo_root()`. The PowerShell scripts apply the
same rule inline: `tests/differential/harness.ps1` sets `$ProjectRoot` to the
native root and `$WslRoot` to its WSL form, and `tests/differential/compare.ps1`
sets `$ProjectRoot` only.
Unit tests for the helper are in `tests/unit/test_harness_paths.py`.

Paths under `/mnt/share`, `/tmp` and `/mnt/c/Windows` are fixed guest or WSL
locations, not host checkout paths, and are not configurable.

## Other test suites (not run by CI)

| Path | What it does | What it needs |
|---|---|---|
| `tests/harness/` | Definition-of-Done conformance runner (`dodrun.py`) and its self-tests (`selftest.py`), driven by `tests/conformance/criteria.json` and `registry.yaml` | Python 3, WSL/Linux, and the VM and kernel evidence for many criteria. Unregistered criteria are reported as UNIMPLEMENTED, not PASS |
| `tests/kernel/` | PE test programs run inside a guest kernel through `binfmt_pe` (`make -C tests/kernel`) | `x86_64-w64-mingw32-gcc` and the VM setup in `vm/` |
| `tests/conformance/` | `make -C tests/conformance test` builds and runs `test_nt_types.c` with `gcc -Werror` | gcc on Linux |
| `tests/differential/` | Probes and checkers compared against golden output (`make -C tests/differential check`) | gcc; the `golden/` inputs the Makefile reads are not in this snapshot |
| `build/Makefile` | Top-level targets `test`, `conformance` and `validate-criteria` | `PROJ` defaults to the repository root derived from the Makefile's location. Override it only to target another tree |
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
| `docs/architecture/` | Mermaid diagrams of the components, the startup path, syscall dispatch, objects, memory, the loader, and the test layers. [build-and-test.md](architecture/build-and-test.md) shows how the test layers relate. |

## Generated file: `ntstatus_table.inc`

`src/kernel/nt/ntstatus_kernel.c` includes `src/kernel/nt/ntstatus_table.inc`
at line 22. The `.inc` is generated from the userspace table and is tracked in
the repository, so the kernel source compiles from a checkout. Do not edit the
`.inc` by hand. Edit `src/ntdll/ntstatus_to_dos.c` and regenerate:

```
make -f build/Makefile ntstatus-table
```

`make` runs `python3` by default. Pass `PYTHON=` to use another interpreter.
The rule rebuilds the `.inc` when either `ntstatus_to_dos.c` or the generator
changes. It runs the generator from the repository root, so the header's source
path is stable.

How the generator works, from `src/kernel/nt/gen_ntstatus_table.py`:

1. It reads the userspace source of truth, `src/ntdll/ntstatus_to_dos.c`, and
   finds the array `static const NTSTATUS_TO_DOS NtStatusToDosErrorTable[] = { ... };`.
   If the array is not found, it exits with status 1.
2. It parses every line of the array body that starts with `{`. An entry is
   `{ NAME, VALUE }`, where NAME is a `STATUS_` identifier or a cast hex literal
   such as `(NTSTATUS)0xc0000225L`, and VALUE may carry U/L suffixes such as
   `1168LL`. A line that starts with `{` but does not parse, or a line that
   mentions `STATUS_` or `0x` without the leading brace, stops the run with
   status 1 and writes nothing.
3. It writes the file with LF line endings. The header starts with an
   `AUTO-GENERATED - DO NOT EDIT` comment and a line `Entries: N`. The body is
   the entries, verbatim and in source order.

The generator does not sort the entries and does not evaluate the `STATUS_`
names. `ntstatus_kernel.c` binary-searches the table, so it must stay sorted
by signed value. The generator does not check that. `--expect N` makes the run
fail unless exactly N entries are found; the Makefile rule does not pass it.

The table has 520 entries. An earlier run reported 519 and warned against a
hard-coded 527. The count was wrong because the regex dropped `1168LL`, and
527 had no basis in the source. The unit test
`test_committed_inc_matches_generator_output` fails if the tracked `.inc` no
longer matches the generator's output, so commit the regenerated `.inc` together
with any change to `ntstatus_to_dos.c`.
