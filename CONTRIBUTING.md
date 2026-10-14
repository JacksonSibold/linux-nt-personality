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

## How to pick an issue

The open work is tracked as GitHub issues, grouped by milestone (M-A to M-F
follow the criteria roadmap, and the release rungs v0.5.0 to v1.0.0 count
criteria that pass). The overview is [docs/ROADMAP.md](docs/ROADMAP.md).

- Start with a `good first issue`. Each one names its source document, its
  acceptance criteria and the exact command that verifies it.
- Do not start an issue labelled `blocked-on-owner`. It needs a decision that
  the project owner records in [docs/OWNER-DECISIONS.md](docs/OWNER-DECISIONS.md)
  first. You can comment with a proposed option, but do not change code on it.
- An issue labelled `needs-guest` needs a booted personality kernel or the
  Linux guest. Say in the pull request which of those checks you ran.
- Every issue must stay traceable to a repository file. If you find that a
  finding is wrong or already fixed, say so in the issue and cite the commit.

## How to run the tests

Run these from the repository root.

- Unit suite (standard library only):
  `python -m unittest discover tests/unit`
- Repository checks that CI runs:
  `python tools/check_repo.py`, `python tools/check_doc_links.py`,
  `python tools/update_doc_counts.py --check`
- Host C tests, which need a gcc toolchain (for example on Linux or WSL):
  `make -C tests/ntdll-host test` (ntdll and the Win32 shim, pure logic) and
  `make -C tests/kernel-host check` (kernel sources built against a host shim).
- Linux guest and kernel build: see [docs/status/LINUX-ENV.md](docs/status/LINUX-ENV.md)
  for the environment and [docs/vm/BUILD-RECIPE.md](docs/vm/BUILD-RECIPE.md) for
  the kernel build steps. Those steps need Linux or WSL, a kernel tree and, for
  the golden comparison, a Windows reference host.
- Criteria harness: `python tests/harness/dodrun.py` runs the registered
  criteria. It refuses to run while the integrity lint fails; see the open
  harness issue before you run it.

## The honesty rule for criteria

A criterion is PASS only when `python tests/harness/dodrun.py` records PASS for
it in a run record. Evidence that is not a dodrun record does not change a
criterion's status:

- a direct check (`python tools/docs_lint.py --policy --criterion <ID>`) is
  evidence to attach, not a PASS;
- a host unit test is evidence, and it does not stand in for a golden or a
  guest run;
- a registry entry that always passes is a false PASS and is not accepted.

Do not edit the generated status files by hand. [docs/status/CRITERIA.md](docs/status/CRITERIA.md),
[docs/status/CRITERIA-ROADMAP.md](docs/status/CRITERIA-ROADMAP.md) and
[docs/status/SERVICES.md](docs/status/SERVICES.md) come from
`tools/criteria_report.py`, `tools/criteria_roadmap.py` and
`tools/service_inventory.py`. Change the source and regenerate. In the pull
request, say which results you ran and which you only read.

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
