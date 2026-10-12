# ntpers — Windows execution personality research

[![CI](https://github.com/JacksonSibold/ntpers-research/actions/workflows/ci.yml/badge.svg)](https://github.com/JacksonSibold/ntpers-research/actions/workflows/ci.yml)

An experimental personal systems project investigating a native Windows PE execution personality alongside Linux's ABI. The source explores PE loading, NT syscall dispatch, object/handle management, virtual memory, synchronization and compatibility probing.

**Status: incomplete research prototype.** This is not a finished Windows compatibility layer, replacement for Windows, or production operating system. Development used AI-assisted implementation and independent verification.

## Current state

Each statement links to the file it comes from. These are recorded results; they were not re-run for this section.

- **Conformance.** The original development handoff records **3 of 423** criteria passing (historical, not reproduced in this publication). [docs/status/CRITERIA.md](docs/status/CRITERIA.md) lists all 423 criteria as `unknown`, because no per-criterion results are recorded in the repository.
- **Architecture.** [docs/phase2/ARCHITECTURE-STATUS.md](docs/phase2/ARCHITECTURE-STATUS.md) rates 21 components named in the architecture document: 0 implemented, 14 partial, 1 stub, 6 missing.
- **Static review.** [docs/review/INDEX.md](docs/review/INDEX.md) (read-only, 2026-10-12, nothing compiled or run) lists **137 findings** in four reports: 28 high, 43 medium, 60 low, 6 info. [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) quotes older totals (21 / 34 / 42); the review index is the source used here.
- **Modules.** Eleven module reference pages cover `src/kernel/nt/` ([docs/modules/INDEX.md](docs/modules/INDEX.md)). Known gaps per module are listed in [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).
- **Syntax check.** [tests/syntax/README.md](tests/syntax/README.md) records 25 of 30 kernel-facing files passing `gcc -fsyntax-only` against the userspace shims with MinGW gcc 16.2. The five failures need Linux kernel headers.
- **Build and boot.** No kernel build, VM boot or conformance run is recorded in this repository.

## Repository map

- `src/`: kernel-personality components, loader, ntdll and Win32 experiments.
- `arch/`: architecture-specific integration.
- `build/`: top-level Makefile and generator scripts.
- `docs/`: architecture, research notes and definition of done.
- `tests/`: conformance harness, differential probes and regression source.
- `tools/`: standalone Python tools (repository checks, NTSTATUS lookup, PE summary, criteria report).
- `vm/`: experimental build/run and artifact-freshness tooling.

The original development handoff records a minimal PE reaching exit code 42 and only **3 of 423** full conformance criteria passing. These are historical project records, not results reproduced during this publication. Large generated evidence bundles, Windows binaries, VM images, build products and local Git history are excluded from this source snapshot. Some evidence-dependent commands will therefore lack their inputs.

## Running the tests

Run these from the repository root. The Python scripts use only the standard library. CI uses Python 3.12.

```
python -m unittest discover tests/unit
python tools/check_repo.py
sh tests/syntax/check.sh
```

- **`tests/unit`** holds 8 test modules. They cover the scripts in `build/`, `src/kernel/nt/gen_ntstatus_table.py` and `tools/criteria_report.py`. The tests use temporary fixtures and do not modify the tree. CI runs them.
- **`tools/check_repo.py`** checks the files listed by `git ls-files`: no binary over 1 MiB, every `.py` compiles, every `.c` with a same-named `.h` has an include guard, and no text file mixes CRLF and bare LF. It exits 1 on any failure. `--quiet` prints only the summary. It does not run the credential scan in `PUBLICATION_CHECKS.md`. CI runs it.
- **`tests/syntax/check.sh`** runs `gcc -fsyntax-only -Wall -Wextra -std=c11` over every `src/kernel/nt/*.c`, `src/loader/*.c` and `src/kernel/fs/*.c` file, with the shims in `tests/syntax/shim/`. It prints PASS or FAIL per file and exits with the number of failing files. Set `CC=` to choose the compiler. It is not run by CI.

Other suites (`tests/harness`, `tests/kernel`, `tests/conformance`, `tests/differential`, `build/Makefile`, `vm/`) need Linux or WSL, a cross compiler or a configured kernel tree. They are not covered by CI. The table of requirements is in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Documentation index

| Path | Contents |
|---|---|
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Running the tests and tools, paths and environment variables, the generated `ntstatus_table.inc`. |
| [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) | Known gaps per module and the top findings from the static review. |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Checks to run before a change, and coding guidelines. |
| [docs/modules/](docs/modules/INDEX.md) | Module reference for `src/kernel/nt/`: purpose, API, data structures, invariants and known gaps. |
| [docs/review/](docs/review/INDEX.md) | Static review reports: loader, kernel fs, `arch/x86/nt`, `src/kernel/nt`. |
| [docs/analysis/](docs/analysis/tree-divergence.md) | Analysis of the divergence between `arch/x86/nt` and `src/kernel/nt`. |
| [docs/status/CRITERIA.md](docs/status/CRITERIA.md) | Generated conformance status table for the 423 criteria. |
| [docs/status/PROGRESS.md](docs/status/PROGRESS.md) | Chronological summary of the commit history, grouped by theme. |
| [docs/phase0/DEFINITION-OF-DONE.md](docs/phase0/DEFINITION-OF-DONE.md) | The ratified definition of done. |
| [docs/phase1/](docs/phase1/) | Research reports 1 to 4. |
| [docs/phase2/ARCHITECTURE.md](docs/phase2/ARCHITECTURE.md) | Master architecture. |
| [docs/phase2/ARCHITECTURE-STATUS.md](docs/phase2/ARCHITECTURE-STATUS.md) | Each architecture component mapped to its implementation file(s) and status. |
| [tools/README.md](tools/README.md) | Options and exit codes for the standalone tools. |

## Engineering lesson

Independent verification found tests measured against stale kernel images. Source-to-image freshness checks and artifact identity stamping were added before accepting results. Other defects remain, including loader and virtual-memory behavior; the design documentation describes goals, not completed capabilities.

## Build and validation

Kernel/VM work requires a separately configured Linux environment. Read the scripts before running them: some rebuild a local kernel tree. Publication checks cover Python syntax and repository hygiene only; a clean kernel build, VM boot and full conformance run were not performed here.

## Source and licensing

This snapshot retains source notices. Linux-derived integration material remains subject to its upstream license; this publication does not relicense third-party code or include proprietary Windows binaries. A complete file-by-file provenance/license review has not been established. No new blanket license grant is made for the project's original code.
