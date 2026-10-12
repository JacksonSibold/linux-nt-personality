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

```
python3 tools/check_repo.py              # full report
python3 tools/check_repo.py --quiet      # summary line only
python3 tools/check_repo.py --root PATH  # check another tree
```

Exit status: `0` all checks pass, `1` at least one failure (each printed as
`FAIL [check] path: detail`), `2` usage error. It does not perform the
credential pattern scan described in `PUBLICATION_CHECKS.md`.

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
