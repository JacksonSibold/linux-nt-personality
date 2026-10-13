# 0060: timeout_to_jiffies dereferences a pointer that may be a user address (kernel-nt SYNC-2)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/kernel-nt.md` SYNC-2 (Low, needs owner in the lows pass).
Date: 2026-10-12

## Context

`src/kernel/nt/sync.c:1421-1433`: when `access_ok(to)` fails, `t = *to` reads the pointer directly. That is correct for a kernel copy and wrong for a user address. The callers at `:1701`, `:1751`, `:1839` and `:1967` must pass a kernel copy. `nt_svc_NtWaitForMultipleObjects` does (`:2214-2218`). The provenance of the other callers was only partly traced.

## Options

1. Make the helper accept only kernel pointers: `WARN_ON` and return `-1` otherwise. Tradeoff: a caller that passes a user address gets an error, not the timeout.
2. Copy the timeout in each service wrapper with `copy_from_user`, then pass the kernel copy. Tradeoff: one copy per caller.

## Recommendation (proposal)

Option 2, the same pattern as `NtWaitForMultipleObjects`, so the helper never dereferences a user value.

## Patch sketch

- Each of the four callers: `copy_from_user` the timeout into a local, pass its address.
- Optionally keep the `access_ok` branch as a `WARN_ON` for debug builds.

## Test plan

- A user timeout pointer to each wait service: copied and honoured.
- A kernel-only build check (VM) because `sync.c` kernel branch does not compile on the host.

## Not verified

Not built or run. The callers outside those four were not traced.

## Owner question

Should the helper trust that every caller passes a kernel copy, or should each wrapper copy the timeout itself?
