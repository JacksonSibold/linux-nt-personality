# 0053: EndOfFile is not range-checked (arch IO-18)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-18 (Low, partly fixed in bdae5f7: negative sizes are rejected).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:2234-2245` (`ntio_set_eof`) rejects negative sizes since bdae5f7. A very large value is still accepted, and the file grows as a sparse file with no limit.

## Options

1. Keep the negative check and cap the size at a maximum the owner chooses. Tradeoff: the cap is a new policy value.
2. Cap only, with no maximum beyond the filesystem limit. Tradeoff: the largest sizes are still reachable.

## Recommendation (proposal)

Option 1, with the cap as an owner-chosen constant.

## Patch sketch

- `ntio_set_eof`: return `STATUS_INVALID_PARAMETER` above the cap, next to the existing negative check.

## Test plan

- A size above the cap: `STATUS_INVALID_PARAMETER`, file unchanged.
- A size at the cap: accepted.

## Not verified

Not built or run.

## Owner question

What maximum file size should `SetInformationFile(EndOfFile)` accept?
