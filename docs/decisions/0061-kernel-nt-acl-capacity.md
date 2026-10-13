# 0061: ACL capacity table silently drops registrations (kernel-nt SEC-1)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/kernel-nt.md` SEC-1 (Low, needs owner in the lows pass).
Date: 2026-10-12

## Context

`src/kernel/nt/security.c:40-70`: `g_acl_caps` has 256 slots. `acl_capacity_register` (`:57-63`) does nothing when the table is full. `acl_capacity_lookup` (`:47-55`) then falls back to `acl->AclSize`, which is the used size for ACLs built here, so `acl_has_space` (`:489-493`) reports no room and additions fail. The file has no call sites outside its tests (`docs/modules/security.md`), so the defect is latent.

## Options

1. Store the capacity in the ACL allocation. Tradeoff: a layout change.
2. Grow the table, and return `STATUS_INSUFFICIENT_RESOURCES` on overflow. Tradeoff: a bounded table can still fill.

## Recommendation (proposal)

Option 2 for now, because the layout is not needed while there are no call sites.

## Patch sketch

- `security.c:57-63`: return an error when the table is full, and have the caller check it.
- `security.c:47-55`: do not fall back to the used size silently.

## Test plan

- Register 257 ACLs: the 257th returns `STATUS_INSUFFICIENT_RESOURCES`, not success.
- `security_test` still passes (41 of 41 at the last run).

## Not verified

Not built or run.

## Owner question

Should the ACL capacity live in the ACL allocation (a layout change), or should the table fail visibly when it is full?
