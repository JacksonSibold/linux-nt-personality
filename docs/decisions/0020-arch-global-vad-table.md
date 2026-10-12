# 0020: One global VAD table for every process (arch M-1)

Status: Proposed. Arch source (`arch/x86/nt`). Recorded only in this batch, not implemented.
Finding: `docs/review/arch-x86-nt.md` M-1 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/mm.c:167` defines one `struct nt_mm g_nt_mm`, which the comment at 165 calls a singleton for tests. Six services ignore their process handle with `(void)ProcessHandle;`: lines 836, 1133, 1271, 1353, 1553 and 1610. Operations act on the caller's address space while being recorded in the shared table. `ntmm_reset` (`mm.c:774`) and `ntmm_destroy` are not called from exec or process exit. This also makes kernel-fs K-10 (0014) more likely to conflict.

## Options

1. Per-process `struct nt_mm` in `nt_eprocess`. Resolve `ProcessHandle` through the object manager. Reset on exec and destroy on exit. Tradeoff: touches every service and the exec and exit paths. Large change. End state.
2. Interim guard: each of the six services returns `STATUS_NOT_SUPPORTED` when `ProcessHandle` names a process other than the caller. Tradeoff: no cross-process operations; the table stays shared but cannot be used across processes. Small change.
3. Keep the singleton and document it as single-process only. Tradeoff: accepts the table leak and the wrong-address-space behavior.

## Recommendation

Option 2 now, as a guard. Option 1 as the fix.

## Patch sketch

- Add `ntmm_check_process(HANDLE h)` that resolves the handle and rejects any process other than the caller.
- Replace `(void)ProcessHandle;` at each of the six sites with a call to it.
- Option 1 later: move `g_nt_mm` into the process object and reset it on exec.

## Test plan

- Kernel test: from process A, `NtAllocateVirtualMemory` with B's process handle returns an error; A's VAD table does not list B's regions.
- Kernel test: after exec, the VAD table for the new process is empty before the image is mapped.
- Host: not applicable.

## Not verified

Not built or run. `src/kernel/nt` has its own mm (0001 covers its section handles) and was not compared here.
