# 0041: Protection mapping has no W^X policy (loader L-12)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/loader.md` L-12 (Low).
Date: 2026-10-12

## Context

`src/loader/pe_prot.c:44-46` returns READ|WRITE|EXEC unchanged when a section asks for all three. The comment at `pe_prot.c:5` mentions the write+execute case, but nothing enforces or flags it. `pe_prot.c:25-32` derives protections from the content class only when no MEM bits are set, so a section with only `IMAGE_SCN_MEM_WRITE` gets `PROT_WRITE` without `PROT_READ`.

## Options

1. Reject sections that are both writable and executable. Tradeoff: some legacy images fail to load.
2. Accept them but flag each one (log or a returned flag). Tradeoff: the protection stays weak.
3. Keep the current mapping. Tradeoff: no policy.

Independent of the above: set READ whenever WRITE is set.

## Recommendation (proposal)

Option 2 first, so legacy images still load and the cases are visible, then option 1 once the owner accepts the break. Always set READ with WRITE.

## Patch sketch

- `pe_prot.c:44-46`: count or reject the W+X case according to the chosen option.
- `pe_prot.c:25-32`: add `PROT_READ` whenever `PROT_WRITE` is set.

## Test plan

- Host test in the loader tests: a WRITE-only section maps to READ|WRITE.
- Host test: a W+X section is rejected (option 1) or flagged (option 2).

## Not verified

Not built or run.

## Owner question

Should the loader reject writable and executable sections, or only flag them, and should WRITE always imply READ?
