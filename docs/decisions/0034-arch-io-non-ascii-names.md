# 0034: Non-ASCII NT names collapse to '?' (arch IO-15)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-15 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:189` writes `?` for every UTF-16 code unit at or above `0x80`. Distinct names such as `é.txt` and `?.txt` map to the same Linux path, so one file can be opened under another's name.

## Options

1. Reject non-ASCII names with an NT status chosen by the owner. Tradeoff: names that Windows software expects to work fail.
2. Encode non-ASCII names (for example UTF-8) consistently in both directions. Tradeoff: more code, and the encoding becomes a stable policy.
3. Keep the current mapping. Tradeoff: name collisions remain.

## Recommendation (proposal)

Option 1 now, because it removes the collision with the smallest change. Option 2 when a caller needs Unicode names.

## Patch sketch

- `nt_io.c:189`: return the chosen status when `wc >= 0x80` instead of writing `?`.
- Add a check in the path conversion so the rejection happens before any open.

## Test plan

- Path `é.txt` is rejected; path `?.txt` is still accepted and maps to its own file.
- No two distinct inputs produce the same output path (table-driven test on the conversion).

## Not verified

Not built or run.

## Owner question

Should non-ASCII NT file names be rejected now, or encoded to UTF-8 as the policy?
