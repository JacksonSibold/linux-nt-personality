# 0050: Case-insensitive resolution scans whole directories under a spinlock (arch IO-12)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-12 (Medium).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:857-921` (`ntio_resolve_ci`, kernel branch) runs on every open (`nt_io.c:1029`). For each missing component, `ntio_casefold_lookup` (`nt_io.c:838-850`) walks all children under `parent->d_lock`, so a large directory holds a spinlock for an unbounded time. The copy at `nt_io.c:861-865` into `component[NAME_MAX]` has no bound, and is safe only because `kshim_ntpath_to_linux` (`nt_io.c:265`) rejects long components.

## Options

1. Use the filesystem lookup (`lookup_one_len`, with a case-insensitive fallback) instead of the locked scan. Tradeoff: the match rule may differ from the current scan.
2. Keep the scan but drop the lock and bound the loop. Tradeoff: the scan stays slow on large directories.
3. Make lookup case-sensitive only. Tradeoff: a change of NT-visible behaviour.

## Recommendation (proposal)

Option 1, with the bound on the copy added in any case.

## Patch sketch

- Replace `ntio_casefold_lookup` with a filesystem lookup and a fallback.
- Bound the copy at `nt_io.c:861-865` to `NAME_MAX` locally.

## Test plan

- Open a name in a directory with 10,000 entries: no soft-lockup warning.
- Case-insensitive names still resolve to the existing file. Needs a kernel test (VM).

## Not verified

Not built or run.

## Owner question

Is it acceptable to change case-insensitive lookup to the filesystem's own lookup rule?
