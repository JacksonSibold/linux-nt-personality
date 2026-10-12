# 0037: Exec continues without ntdll when the ntdll image fails (kernel-fs K-18)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/kernel-fs.md` K-18 (Low, partly fixed in 833e002: the `register_binfmt` return is now checked).
Date: 2026-10-12

## Context

`src/kernel/fs/binfmt_pe.c:946` and `:955` log an ntdll mapping failure and use the executable entry point without ntdll. The process then runs in a state the rest of the tree does not expect. Silent degradation is a policy decision and should be explicit.

## Options

1. Fail the exec when ntdll cannot be mapped. Tradeoff: a broken ntdll blocks every PE program.
2. Keep the fallback and document it as a supported mode. Tradeoff: programs run without the user-mode runtime.

## Recommendation (proposal)

Option 1. It matches the fail-closed choices in records 0012 and 0014, and the fallback path is not tested.

## Patch sketch

- `binfmt_pe.c:946`, `:955`: return a failure status to the caller and undo the exec through the existing post-commit path (record 0012 applies after the point of no return).

## Test plan

- Kernel test: exec with a missing or unmappable ntdll image returns failure and the process does not start.
- Needs a kernel build (VM).

## Not verified

Not built or run.

## Owner question

When the ntdll image cannot be mapped, should exec fail or continue without ntdll?
