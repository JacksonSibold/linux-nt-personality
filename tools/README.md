# Developer tools

Small, dependency-free Python 3 scripts (standard library only) for working
with this repository. Run them from the repository root or from anywhere by
path; none of them modify tracked files.

## check_repo.py

Repository hygiene checks, aligned with `PUBLICATION_CHECKS.md`. Checks the
files reported by `git ls-files` (or walks the tree outside a git work tree):

- `binary-size`: no binary file over 1 MiB.
- `py-compile`: every `.py` file compiles (no bytecode is written).
- `header-guard`: a `.c` file with a same-named `.h` beside it needs an
  include guard (`#ifndef`/`#define` pair or `#pragma once`) in that `.h`.
- `crlf-mix`: no text file mixes CRLF and bare-LF line endings.
- `conflict-marker`: no text file has a line starting with `<<<<<<< ` or
  `>>>>>>> `, or a lone `=======` line when the same file also has a
  `<<<<<<< ` line. These are leftover merge conflict markers. A Markdown
  setext underline on its own is not reported.

```
python3 tools/check_repo.py              # full report
python3 tools/check_repo.py --quiet      # summary line only
python3 tools/check_repo.py --root PATH  # check another tree
```

Exit status: `0` all checks pass, `1` at least one failure (each printed as
`FAIL [check] path: detail`), `2` usage error. It does not perform the
credential pattern scan described in `PUBLICATION_CHECKS.md`.

## check_doc_links.py

Checks relative links, images and reference links in every tracked `*.md`
file, and `#anchor` fragments against the headings (GitHub-style slugs) or
HTML `id`/`name` attributes of the target page. The target file must exist with
the same letter case, so the result is the same on Windows and Linux. External
URLs and links inside code are not checked.

```
python3 tools/check_doc_links.py              # full report
python3 tools/check_doc_links.py --quiet      # summary line only
python3 tools/check_doc_links.py --selftest   # built-in fixture
```

Exit status: `0` all links resolve, `1` at least one broken link (each printed
as `FAIL [doc-link] path:line: (target) detail`), `2` usage error.

## check_syscall_tables.py

Checks that every copy of the NT system-call table agrees. The copies are
`src/ntdll/syscall_table.def` (the source for the generators), the two kernel
service lists (`src/kernel/nt/nt_service_list.h` and
`arch/x86/nt/nt_service_list.h`), `build/generated_stubs.S`,
`src/ntdll/ntdll.def` (names only; it carries no SSNs) and `NT_NR_SERVICES` in
both kernel trees. It reports:

- `missing` and `extra`: a name that one copy lacks or adds.
- `ssn-mismatch`: a name whose SSN differs from the table.
- `duplicate`: a name or an SSN that appears twice in one copy.
- `argc-mismatch`: a kernel entry whose argument count differs from `Arity`.
- `alias`: a `Zw` alias whose target is not the matching `Nt` name.
- `nr-services`: `NT_NR_SERVICES` that is not the highest SSN plus one.

Unused SSNs are expected, so they are printed as `INFO [ssn-gap]` and do not
fail the check. The `ArgTypes` count in the table is printed as `INFO
[argc-types]` as well, because no generator reads that column.

```
python3 tools/check_syscall_tables.py              # full report
python3 tools/check_syscall_tables.py --quiet      # summary line only
python3 tools/check_syscall_tables.py --root PATH  # check another tree
python3 tools/check_syscall_tables.py --selftest   # built-in fixture
```

Exit status: `0` all copies agree, `1` at least one mismatch (each printed as
`FAIL [kind] path: detail`), `2` usage error. Standard library only.

## ntstatus_lookup.py

Looks up NTSTATUS values by symbolic name, hex or decimal. It reads
`src/ntdll/ntstatus.h` for `STATUS_*` definitions and the
`NtStatusToDosErrorTable` in `src/ntdll/ntstatus_to_dos.c`. That table is the
source that `src/kernel/nt/gen_ntstatus_table.py` turns into the kernel copy
used by `ntstatus_kernel.c`. The tool reads the userspace source directly, so it
does not depend on the tracked, generated `ntstatus_table.inc`.

```
python3 tools/ntstatus_lookup.py STATUS_BUFFER_OVERFLOW 0xC0000005
python3 tools/ntstatus_lookup.py --search TOO_SMALL
```

Each result shows all names for the value, severity, the customer-code bit and
the DOS error. The DOS error comes from the table when it is listed, and from
the same fallback rule the table code uses otherwise (marked `fallback rule`).
Use `--repo PATH` to point at another checkout.

Exit status: `0` all queries found, `1` a query was not found or the source
files could not be read, `2` usage error.

## pe_info.py

Prints a summary of a PE file: machine, PE32 or PE32+, timestamp, subsystem,
image base, entry point, sections with permissions, ASLR/NX/CFG flags, and an
import summary (DLL and function counts with per-DLL totals). Header reads are
bounds-checked, so a truncated or non-PE file gives an error message.

```
python3 tools/pe_info.py path/to/file.exe
python3 tools/pe_info.py --selftest
```

`--selftest` builds PE32 and PE32+ fixtures in memory (each importing two
functions from KERNEL32.dll), checks the parsed fields, and checks the error
paths (bad magic, truncation, bad signatures, unreadable import directory).

Exit status: `0` success, `1` the file cannot be read or is not a valid PE,
`2` usage error.

## coverage_report.py

Line coverage for `build/*.py`, `tools/*.py` and
`src/kernel/nt/gen_ntstatus_table.py`. It runs `tests/unit` in-process under
`sys.settrace` and prints one row per file (executable statement lines, lines
covered, percentage). Docstrings are not counted. Code that a test runs only in
a subprocess is not seen, so those files show a lower bound. Standard library
only.

```
python3 tools/coverage_report.py              # run the unit tests and report
python3 tools/coverage_report.py --selftest   # check the measurement on a fixture
```

Exit status: `0` report printed and all unit tests passed, `1` a unit test
failed, `2` usage error.

## c_coverage.sh

gcov line coverage for the C host suites. It builds and runs
`tests/kernel-host` (`make check`), `tests/host` and `tests/ntdll-host` with
`COVERAGE=1` (instrumented, `-O0`, objects in `build-cov/` and `out-cov/`),
then prints per-file line coverage for `src/kernel/nt`, `src/loader` and
`src/ntdll` (test drivers `*_test.c` are skipped; a file no suite compiles is
shown as "not built") and the 15 largest uncovered functions. Needs the MSYS2
UCRT64 gcc (`gcov` ships with it). The kernel-host suite takes minutes.

```
sh tools/c_coverage.sh             # run all suites, then report
sh tools/c_coverage.sh --no-run    # report from the existing counters
```

Exit status: `0` all suites passed, `1` a suite or test failed (the report is
still printed), `2` gcov missing or usage error.

## update_doc_counts.py

Keeps the counts quoted in the docs equal to the repository. A count sits
between `<!-- counts:NAME:begin -->` and `<!-- counts:NAME:end -->` markers in
`README.md`, `CONTRIBUTING.md`, `docs/DEVELOPMENT.md`, `docs/KNOWN-ISSUES.md`
and `docs/status/PROGRESS.md`. Values are counted from the files, never by
running anything:

- `unit_modules`, `unit_tests`: the `tests/unit/test_*.py` modules and the
  functions named `test_*` in them (found by parsing, so strings and comments
  are not counted).
- `review_*`, `fix_*`: totals in `docs/review/INDEX.md`, checked against the
  Total rows of its tables.
- `adr_*`: the status column of `docs/decisions/INDEX.md`.
- `tracked_files`: the entries of `git ls-files` (a walk outside a git work tree).

```
python3 tools/update_doc_counts.py --check   # report stale blocks, change nothing
python3 tools/update_doc_counts.py --write   # rewrite stale blocks in place
```

Exit status: `0` all blocks current, `1` a block is stale, a block has no
matching end marker, a source table does not add up, or a target is missing,
`2` usage error. CI runs `--check`.

## criteria_trace.py

Traceability matrix from the Definition-of-Done criteria in
`tests/conformance/criteria.json` (423 entries) to the evidence under `tests/`.
It finds the criterion IDs that each file under `tests/` names (text match, so
no PyYAML is needed) and the active entries of `tests/harness/registry.yaml`.
Each criterion gets one class, first match wins:

- `HAS-HARNESS-ENTRY`: the registry registers a runnable command for it.
- `HAS-HOST-TEST`: a code file under `tests/` (outside `tests/unit`) names it.
- `REFERENCED-IN-DOCS`: named elsewhere under `tests/` (documents, data,
  comments, unimplemented registry entries, `tests/unit` self-tests).
- `NO-EVIDENCE`: nothing under `tests/` names it.

This is traceability, not results. No class is a pass, and the result column
stays `unknown` unless a repository record states a status. Detection is
textual, so a probe that covers a criterion without naming its ID is not
counted. `DOD-PE-A010/B010` counts both IDs.

```
python3 tools/criteria_trace.py                  # class counts
python3 tools/criteria_trace.py --json           # every criterion, JSON
python3 tools/criteria_trace.py --markdown --out docs/status/TRACEABILITY.md
python3 tools/criteria_trace.py --selftest       # built-in checks on synthetic data
```

Exit status: `0` success, `1` an input cannot be read or the selftest fails,
`2` usage error. CI regenerates `docs/status/TRACEABILITY.md` and fails when
the committed file differs.

## static_analyze.sh

POSIX sh, not Python. Runs `gcc -fsyntax-only` with the strict warning set and
`gcc -fanalyzer -c -o /dev/null` on each buildable unit in `src/loader`,
`src/kernel/nt`, `src/ntdll` and `arch/x86/nt`, using the host-shim profiles
from `tests/syntax/check.sh`, `tests/kernel-host` and `tests/ntdll-host`. It
writes `build/static-analysis/` (`report.txt`, `summary.txt`, `units.txt`).
Nothing is linked or run. Triage is in `docs/status/STATIC-ANALYSIS.md`.

```
sh tools/static_analyze.sh               # all in-scope units
sh tools/static_analyze.sh FILE...       # specific files
```

Always exits `0`. CI runs it as an informational step.
