# Contributing

Thanks for looking at ntpers. It is an experimental research prototype, so
please read the [README](README.md) first for its status and limits.

## Before you open a change

1. Run the checks that CI runs, from the repository root:

   ```
   python -m unittest discover tests/unit
   python tools/check_repo.py
   python tools/check_doc_links.py
   python tools/update_doc_counts.py --check
   ```

2. Keep commits small and focused, one logical change per commit.
3. Do not commit build products, VM images, evidence bundles, or credentials.
   `.gitignore` covers common ones, but check `git status` before committing.

The full list of tools and test suites, and how to run them, is in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Guidelines

- Use the standard library only for Python tools and scripts.
- Keep line endings consistent within each file. `check_repo.py` rejects files
  that mix CRLF and bare LF.
- Give each `.c` header an include guard or `#pragma once`.
- If you change `src/ntdll/ntstatus_to_dos.c`, regenerate the kernel table with
  `make -f build/Makefile ntstatus-table` and commit the updated
  `src/kernel/nt/ntstatus_table.inc` in the same commit. See
  [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#generated-file-ntstatus_tableinc).
- Do not add a new known gap silently. Record it in the relevant
  `docs/modules/` page and in [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).

## Reporting problems

Open an issue or pull request describing the problem, the file and line, and
how you checked it. Security-relevant findings should describe the input that
triggers the problem and whether it was run or only read.
