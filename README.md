# linux-nt-personality (ntpers): native Windows NT personality research

[![CI](https://github.com/JacksonSibold/linux-nt-personality/actions/workflows/ci.yml/badge.svg)](https://github.com/JacksonSibold/linux-nt-personality/actions/workflows/ci.yml)

An experimental personal systems project investigating a native Windows PE execution personality alongside Linux's ABI. The source explores PE loading, NT syscall dispatch, object/handle management, virtual memory, synchronization and compatibility probing.

**Status: incomplete research prototype.** This is not a finished Windows compatibility layer, replacement for Windows, or production operating system. Development used AI-assisted implementation and independent verification.

## End goal

The project has two goals, stated in full in [docs/GOAL.md](docs/GOAL.md):

1. Play Rainbow Six Siege offline on Linux, using the owner's own local install, on this native personality.
2. Run the vendor's NVIDIA Windows kernel-mode display driver on the personality, as the GPU path for games.

The personality is a native NT personality inside the Linux kernel. It is early stage: it does not run Siege today, no third-party driver runs, and there is no GPU path yet. The measures in the next section are what shows progress, and none of them is a game or driver result.

## Current state

Each statement links to the file it comes from. These are recorded results; they were not re-run for this section.

- **Conformance.** The original development handoff records **3 of 423** criteria passing (historical, not reproduced in this publication). [docs/status/CRITERIA.md](docs/status/CRITERIA.md) lists all 423 criteria as `unknown`, because no per-criterion results are recorded in the repository.
- **Architecture.** [docs/phase2/ARCHITECTURE-STATUS.md](docs/phase2/ARCHITECTURE-STATUS.md) rates 21 components named in the architecture document: 0 implemented, 14 partial, 1 stub, 6 missing.
- **Static review.** [docs/review/INDEX.md](docs/review/INDEX.md) (read-only, 2026-10-12, nothing compiled or run) lists **<!-- counts:review_findings:begin -->221<!-- counts:review_findings:end --> findings** in <!-- counts:review_reports:begin -->5<!-- counts:review_reports:end --> reports: <!-- counts:review_high:begin -->50<!-- counts:review_high:end --> high, <!-- counts:review_medium:begin -->73<!-- counts:review_medium:end --> medium, <!-- counts:review_low:begin -->84<!-- counts:review_low:end --> low, <!-- counts:review_info:begin -->14<!-- counts:review_info:end --> info. Fix status is recorded for <!-- counts:fix_findings:begin -->221<!-- counts:fix_findings:end --> of them, in <!-- counts:fix_reports:begin -->5<!-- counts:fix_reports:end --> reports: <!-- counts:fix_fixed:begin -->114<!-- counts:fix_fixed:end --> fixed, <!-- counts:fix_partly:begin -->11<!-- counts:fix_partly:end --> partly fixed, <!-- counts:fix_blocked:begin -->41<!-- counts:fix_blocked:end --> blocked, <!-- counts:fix_no_change:begin -->7<!-- counts:fix_no_change:end --> need no change and <!-- counts:fix_open:begin -->48<!-- counts:fix_open:end --> open ([fix status](docs/review/INDEX.md#fix-status)). [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) summarises the highest-priority findings. Decision records are indexed in [docs/decisions/INDEX.md](docs/decisions/INDEX.md): <!-- counts:adr_total:begin -->76<!-- counts:adr_total:end --> records, <!-- counts:adr_fixed:begin -->18<!-- counts:adr_fixed:end --> fixed, <!-- counts:adr_open:begin -->16<!-- counts:adr_open:end --> open, <!-- counts:adr_blocked:begin -->41<!-- counts:adr_blocked:end --> blocked and <!-- counts:adr_no_change:begin -->1<!-- counts:adr_no_change:end --> no-change.
- **Modules.** Fifteen module reference pages cover `src/kernel/nt/` ([docs/modules/INDEX.md](docs/modules/INDEX.md)). Known gaps per module are listed in [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).
- **Syntax check.** A re-run on 2026-10-12 with the msys64 MinGW gcc (`CC=/c/msys64/ucrt64/bin/gcc.exe sh tests/syntax/check.sh`) gives 36 of 41 kernel-facing files passing `gcc -fsyntax-only` against the userspace shims (`passed=36 failed=5 warnings=482`). The five failures are `src/kernel/nt/dispatch.c`, `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`, `src/kernel/fs/binfmt_pe_test.c` and `arch/x86/nt/dispatch.c`. The per-file table in [tests/syntax/README.md](tests/syntax/README.md) predates the last four files and is not updated here.
- **Build and boot.** No kernel build, VM boot or conformance run is recorded in this repository.
- **Driver boundary.** No third-party NT `.sys` executes in the Personality. The tracked tree has no driver-load path. The basis and the gated driver criteria are in [docs/policy/CLAIM-BOUNDARY.md](docs/policy/CLAIM-BOUNDARY.md).

## Repository map

The snapshot tracks <!-- counts:tracked_files:begin -->888<!-- counts:tracked_files:end --> files (`git ls-files`).

- `src/`: kernel-personality components, loader, ntdll and Win32 experiments.
- `arch/`: architecture-specific integration.
- `build/`: top-level Makefile and generator scripts.
- `docs/`: architecture, research notes and definition of done.
- `tests/`: conformance harness, differential probes and regression source.
- `tools/`: standalone Python tools (repository checks, doc counts, NTSTATUS lookup, PE summary, import gap against the personality, criteria report).
- `vm/`: experimental build/run and artifact-freshness tooling.

The original development handoff records a minimal PE reaching exit code 42 and only **3 of 423** full conformance criteria passing (no signed bundle is linked, so this count is unverified). These are historical project records, not results reproduced during this publication. Large generated evidence bundles, Windows binaries, VM images, build products and local Git history are excluded from this source snapshot. Some evidence-dependent commands will therefore lack their inputs.

## Running the tests

Run these from the repository root. The Python scripts use only the standard library. CI uses Python 3.12.

```
python -m unittest discover tests/unit
python tools/check_repo.py
python tools/check_doc_links.py
python tools/update_doc_counts.py --check
sh tests/syntax/check.sh
```

- **`tests/unit`** holds <!-- counts:unit_modules:begin -->42<!-- counts:unit_modules:end --> test modules with <!-- counts:unit_tests:begin -->878<!-- counts:unit_tests:end --> `test_` functions, counted from the source. The suite passed on 2026-10-12. They cover the scripts in `build/`, `src/kernel/nt/gen_ntstatus_table.py` and `tools/`. The tests use temporary fixtures and do not modify the tree. CI runs them.
- **`tools/check_repo.py`** checks the files listed by `git ls-files`: no binary over 1 MiB, every `.py` compiles, every `.c` with a same-named `.h` has an include guard, and no text file mixes CRLF and bare LF. It exits 1 on any failure. `--quiet` prints only the summary. It does not run the credential scan in `PUBLICATION_CHECKS.md`. CI runs it.
- **`tests/syntax/check.sh`** runs `gcc -fsyntax-only -Wall -Wextra -std=c11` over every `src/kernel/nt/*.c`, `src/loader/*.c`, `src/kernel/fs/*.c` and `arch/x86/nt/*.c` file, with the shims in `tests/syntax/shim/`. It prints PASS or FAIL per file and exits with the number of failing files. Set `CC=` to choose the compiler. It is not run by CI.

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
| [docs/decisions/INDEX.md](docs/decisions/INDEX.md) | Decision records for the review findings, with status. |
| [docs/GOAL.md](docs/GOAL.md) | End goal (offline Siege on Linux and the NVIDIA display driver), layers, safety approach and current state. |
| [docs/status/NTOSKRNL-SURFACE.md](docs/status/NTOSKRNL-SURFACE.md) | Kernel-side names defined in `src/kernel/nt`, the surface kernel-mode driver imports are measured against. |
| [docs/status/CRITERIA.md](docs/status/CRITERIA.md) | Generated conformance status table for the 423 criteria. |
| [docs/status/CRITERIA-ROADMAP.md](docs/status/CRITERIA-ROADMAP.md) | Plan for the 423 criteria: what each needs, its blockers, effort, dependency order and milestones M-A to M-F, and the criteria that need out-of-scope or gated components. |
| [docs/status/PROGRESS.md](docs/status/PROGRESS.md) | Chronological summary of the commit history, grouped by theme. |
| [docs/phase0/DEFINITION-OF-DONE.md](docs/phase0/DEFINITION-OF-DONE.md) | The ratified definition of done. |
| [docs/phase1/](docs/phase1/) | Research reports 1 to 4. |
| [docs/phase2/ARCHITECTURE.md](docs/phase2/ARCHITECTURE.md) | Master architecture. |
| [docs/phase2/ARCHITECTURE-STATUS.md](docs/phase2/ARCHITECTURE-STATUS.md) | Each architecture component mapped to its implementation file(s) and status. |
| [docs/architecture/overview.md](docs/architecture/overview.md) | Mermaid diagrams: components, and how a PE executable starts. |
| [docs/architecture/syscall-dispatch.md](docs/architecture/syscall-dispatch.md) | From the `syscall` stub to the NT handler, with the status return mapping and table coverage. |
| [docs/architecture/object-model.md](docs/architecture/object-model.md) | Object and handle lifecycle, handle table layout, and the process and thread registry. |
| [docs/architecture/memory.md](docs/architecture/memory.md) | VAD structures, section objects, and the allocate, free, protect and map flows. |
| [docs/architecture/loading.md](docs/architecture/loading.md) | The userspace loader in ntdll: imports, relocations, TLS and `DllMain`, with stub status. |
| [docs/architecture/build-and-test.md](docs/architecture/build-and-test.md) | How the build and test layers relate, and which ones CI runs. |
| [tools/README.md](tools/README.md) | Options and exit codes for the standalone tools. |

## Engineering lesson

Independent verification found tests measured against stale kernel images. Source-to-image freshness checks and artifact identity stamping were added before accepting results. Other defects remain, including loader and virtual-memory behavior; the design documentation describes goals, not completed capabilities.

## Build and validation

Kernel/VM work requires a separately configured Linux environment. Read the scripts before running them: some rebuild a local kernel tree. Publication checks cover Python syntax and repository hygiene only; a clean kernel build, VM boot and full conformance run were not performed here.

## Source and licensing

This snapshot retains source notices. Linux-derived integration material remains subject to its upstream license; this publication does not relicense third-party code or include proprietary Windows binaries. A complete file-by-file provenance/license review has not been established. No new blanket license grant is made for the project's original code.
