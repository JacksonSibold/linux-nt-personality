# 0034: timeout_to_jiffies dereferences non-user pointers (SYNC-2)

Status: Blocked. Needs an owner decision and a full caller audit. Not implemented.
Finding: `docs/review/kernel-nt.md` SYNC-2 (Low).
Date: 2026-10-12

## Context

`src/kernel/nt/sync.c` `timeout_to_jiffies` (around `:1421-1433`) copies the timeout with `copy_from_user` when `access_ok` passes. When `access_ok` fails it does `t = *to`, a direct read. That read is correct for a kernel copy and wrong for a user address that failed `access_ok`, which is a fault or an attacker-chosen kernel address. Callers at `:1701`, `:1751`, `:1839` and `:1967` pass `due_time` or `timeout`. The provenance of each was only partly traced in the review. `nt_svc_NtWaitForMultipleObjects` (`:2214-2218`) copies first.

## Options

1. Make the helper accept only kernel pointers. Keep the direct read, add `WARN_ON`, and return `-1` for anything that fails `access_ok`. Tradeoff: a caller that passes a user pointer gets an error, which is correct, but a kernel copy that happens to fail `access_ok` is also refused, so the helper's contract has to be stated exactly.
2. Copy in each service wrapper that passes a timeout, and have the helper take a kernel pointer only. Tradeoff: four call sites change, and each must return the copy-fault status. Matches the pattern already used by `NtWaitForMultipleObjects`.
3. Use `get_user`-style copies inside the helper for every case. Tradeoff: the helper cannot tell a kernel copy from a user address, which is the original problem.

## Recommendation

Option 2. The helper then has one job, and the provenance of each caller is decided at its wrapper. Option 1 is an acceptable stopgap if the audit is not done.

## Patch sketch

- In each of the four callers (or their service wrappers): `LARGE_INTEGER kt; if (timeout && copy_from_user(&kt, timeout, sizeof kt)) return STATUS_ACCESS_VIOLATION;` then pass `timeout ? &kt : NULL`.
- In `timeout_to_jiffies`: delete the `access_ok` branch and the direct read. Keep the NULL check.
- Audit `:1701` first: the provenance of `due_time` was not traced.

## Test plan

- Kernel build only: the kernel branch of `sync.c` does not compile on the host. Verify by reading each caller and by `grep` for `timeout_to_jiffies(`.
- Host: `sync_test` covers the userspace copy path only. Add a case for each service wrapper once a user-pointer test hook exists.
- Negative: a timeout pointer that faults returns the copy-fault status, not a kernel read.

## Not verified

Not built for the kernel. The caller audit is incomplete (`:1701` not traced).
