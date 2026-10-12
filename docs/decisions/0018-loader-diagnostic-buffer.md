# 0018: Shared diagnostic buffer and kernel log flooding (L-10)

Status: Proposed. Changes `pe_validate` callers in `src/ntdll`. Recorded only in this batch, not implemented.
Finding: `docs/review/loader.md` L-10 (Low).
Date: 2026-10-12

## Context

`pe_diag_buf` (`src/loader/pe_parse.c:22`) is one file-scope buffer. Concurrent `pe_validate()` calls race on it, and `pe_validate_diag()` returns a pointer into it. The kernel build also calls `pr_info` on every failure (`pe_parse.c:41`), so a caller can flood the log by loading crafted files. `pe_validate` is called from `src/ntdll/ldr.c:927`.

## Options

1. Caller-owned buffer: `pe_validate(ctx, char *diag, size_t cap)`. Tradeoff: signature change at every caller, including `ldr.c:927` and the host tests.
2. Per-context buffer: a `char diag[512]` field in `pe_context_t`, read with `pe_validate_diag(ctx)`. Tradeoff: signature change to `pe_validate_diag`; the struct grows by 512 bytes. No race, since each context has its own buffer.
3. Serialize `pe_validate` with a lock in the kernel. Tradeoff: adds a lock to a parser; the global buffer and the log issue remain.
4. Rate-limit the kernel log only (`pr_info_ratelimited`). Tradeoff: fixes flooding, not the race.

## Recommendation

Option 2 for the race, together with option 4 in the kernel branch. The kernel branch cannot be exercised by the host harness, so the rate limit is checked by review only.

## Patch sketch

- Add `char diag[512]` to `pe_context_t`. Make the diagnostic helpers take `ctx` (`PE_FAIL` already has `ctx` in most walkers; thread it through the others).
- `pe_validate_diag(const pe_context_t *ctx)` returns `ctx->diag`, or `""` after success.
- Kernel branch of `pe_diag_set`: `pr_info_ratelimited`.
- Update `ldr.c` and the host tests to pass `ctx`.

## Test plan

- Host: validate context A with a bad image, then context B with a different bad image. Each diagnostic must match its own context.
- Host: after a successful validation, `pe_validate_diag(ctx)` returns `""`.
- Host: the full suite passes (`make -C tests/host test`).

## Not verified

Race is shown by reading the code, not by running two threads. The kernel log change is not compiled on the host.
