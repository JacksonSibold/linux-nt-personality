# 0035: ACL capacity table silently drops registrations (SEC-1)

Status: Blocked. Needs an owner decision between a layout change and an error-return change. Not implemented.
Finding: `docs/review/kernel-nt.md` SEC-1 (Low).
Date: 2026-10-12

## Context

`src/kernel/nt/security.c` keeps ACL capacities in `g_acl_caps[ACL_CAP_MAX]` (`ACL_CAP_MAX` 256, around `:40-70`). `acl_capacity_register` returns nothing and silently does nothing when the table is full. `acl_capacity_lookup` then falls back to `acl->AclSize`, which for an ACL built here is the used size. `acl_has_space` (around `:489`) then reports no room and additions fail with no error at the source.

Register call sites in the file (grep): `:459`, `:868`, `:892`, `:978`, `:1035`. The review said six; grep finds five. The owner should confirm which one is missing before choosing.

## Options

1. Store capacity in the ACL allocation (a header or trailing field next to the ACL). Removes the table and its 256-entry limit. Tradeoff: changes the allocation layout, and every allocate and free site must agree.
2. Make `acl_capacity_register` return `NTSTATUS`, return `STATUS_INSUFFICIENT_RESOURCES` on overflow, and check it at each call site. Tradeoff: the 256-ACL cap stays, but the failure is reported where it happens.
3. Grow the table dynamically. Tradeoff: still a hidden global, and a growth failure needs the same error path as option 2.

## Recommendation

Option 2 now. It makes the failure visible without a layout change. Option 1 is the end state if the 256 cap is itself a problem. Decide before any change to the allocation layout.

## Patch sketch

- `static NTSTATUS acl_capacity_register(PACL acl, USHORT cap)`: return `STATUS_INSUFFICIENT_RESOURCES` when `g_acl_cap_used >= ACL_CAP_MAX`.
- At each of the five call sites, check the status and free the duplicate ACL on failure (`:459` allocation path first).
- The fallback in `acl_capacity_lookup` stays, now unreachable for registered ACLs.

## Test plan

- Host (`security_test`, which runs in `tests/kernel-host`): register 256 live ACLs, then create one more through the public path. Expect `STATUS_INSUFFICIENT_RESOURCES`, not a silent success followed by a failed add.
- Host: after freeing one ACL, a new ACL is accepted again.
- Host: an ACL created before the table filled still accepts ACEs up to its capacity.

## Not verified

Not built or run. The five-versus-six call-site count needs the owner to confirm.
