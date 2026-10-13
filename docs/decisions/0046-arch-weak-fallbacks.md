# 0046: Weak fallbacks hide missing strong symbols (arch B-4)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` B-4 (Low, partly fixed in d730c42: the stray global is removed).
Date: 2026-10-12

## Context

`arch/x86/nt/weak.c:7-15` defines `nt_handle_table_destroy` and `nt_abandon_owned_mutants` as weak no-ops. If the strong definitions are not linked, handle tables and owned mutants are never cleaned up, and nothing warns.

## Options

1. Make a missing strong symbol a link error for the kernel build. Tradeoff: the build fails until every symbol is provided.
2. Log at init when the weak version is used. Tradeoff: the build still succeeds, and the log can be missed.
3. Keep silent no-ops. Tradeoff: leaks and missing cleanup with no signal.

## Recommendation (proposal)

Option 1 for the release build, and option 2 for a debug build.

## Patch sketch

- `weak.c`: remove the weak attribute for the two symbols, or keep it and call a `pr_warn` at init when the weak body runs.

## Test plan

- Link the kernel without the strong definition: the build fails (option 1), or the init log appears (option 2). Needs a kernel build (VM).

## Not verified

Not built or run.

## Owner question

Should a missing handle-table or owned-mutant cleanup routine fail the kernel link, or only log at init?
