# 0019: aligned_highest u32 wrap is not reachable (L-14)

Status: Accepted. No code change. The finding is not reachable, so it is downgraded from Info to no finding.
Finding: `docs/review/loader.md` L-14 (Info).
Date: 2026-10-12

## Context

The review says `aligned_highest` in `src/loader/pe_parse.c` (computed in u32, `(highest + SectionAlignment - 1) & ~(SectionAlignment - 1)`) can wrap when `highest` is within one alignment unit of 4 GiB.

The wrap cannot happen, because of two earlier checks in `pe_validate`:

- `SizeOfImage % SectionAlignment != 0` is rejected (`pe_parse.c:451-452`). So `SizeOfImage` is a multiple of `A = SectionAlignment` and at most `2^32 - A`.
- `highest <= SizeOfImage` is required (the check just above the computation).

So `highest <= 2^32 - A`, and `highest + A - 1 <= 2^32 - 1`. The u32 sum never wraps for any alignment value that passes the check.

## Options

1. No change. The arithmetic is correct for every input that reaches it.
2. Compute in u64, as the review suggests. This is a defensive cleanup that protects against a future edit that removes the alignment check.
3. Add a comment that states the precondition.

## Recommendation

Option 1 now. Take option 2 when the function is next edited, together with the comment from option 3.

Option 2 is not done in this batch: no input can show the wrap, so a test cannot fail before the change. The batch rule requires a failing test first. A test added with option 2 would pass before and after and only guard the code.

## Patch sketch

None for now. For option 2: `u64 aligned_highest = ((u64)highest + A - 1) & ~((u64)A - 1);` with the comparisons done in u64.

## Test plan

- Existing: the `SizeOfImage` alignment rejection at `pe_parse.c:451-452` covers the precondition. Add a dedicated host test only if the function is changed.
- Host: `make -C tests/host test` must pass with no change.

## Not verified

The wrap was checked by reading the arithmetic and the two preceding checks. A host attempt to build a failing image was tried, and it was rejected at the alignment check before the computation was reached.
