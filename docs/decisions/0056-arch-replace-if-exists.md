# 0056: ReplaceIfExists is ignored on rename (arch IO-13)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-13 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:2259-2300` (`ntio_set_rename`) never reads the `ReplaceIfExists` field. A rename onto an existing file always replaces it. NT requires `STATUS_OBJECT_NAME_COLLISION` when the flag is FALSE and the target exists.

## Options

1. Check the target first and honour the flag, returning the collision status when the flag is FALSE. Tradeoff: an existence check the shim lacks today, and a status choice.
2. Keep replacing and document it. Tradeoff: differs from NT.

## Recommendation (proposal)

Option 1, which is the NT behaviour.

## Patch sketch

- `ntio_set_rename`: look up the target; if it exists and the flag is FALSE, return `STATUS_OBJECT_NAME_COLLISION`.

## Test plan

- Rename onto an existing file with the flag FALSE: collision status, and the target is unchanged.
- Rename with the flag TRUE: the target is replaced.

## Not verified

Not built or run.

## Owner question

Should a rename onto an existing file fail with `STATUS_OBJECT_NAME_COLLISION` when `ReplaceIfExists` is FALSE?
