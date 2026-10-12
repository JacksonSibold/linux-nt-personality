# 0010: Completion queue quota (IOCP-2)

Status: Accepted. Implemented in the commit that follows this record.
Finding: `docs/review/kernel-nt.md` IOCP-2 (Medium).
Date: 2026-10-12

## Context

`ntio_iocp_post` (`src/kernel/nt/iocp.c:80-108`) allocates one packet per call with `kzalloc` and appends it to `iocp->queue`, incrementing `iocp->queued`. It never checks `queued`. Any caller that can open a port can post in a loop until kernel memory runs out. The same function is also the sink for file completions (`ntio_complete_to_iocp`, `io.c:1768-1773`), which ignores its return value, so a full queue silently drops file completions.

## Options

1. **Fixed cap on queued packets (chosen).** A constant `NTIO_IOCP_MAX_QUEUED` (65536). Check under the port lock and return `STATUS_INSUFFICIENT_RESOURCES` above it. Tradeoff: the cap is arbitrary, and file completions past the cap are dropped (already the case on allocation failure). No ABI change.
2. **Per-port quota from `NumberOfConcurrentThreads`.** Tradeoff: ties memory use to a thread count that is often 0 or small; surprising.
3. **Charge packets to the caller's process quota.** Tradeoff: needs process memory accounting, which does not exist in this tree; object-model change.
4. **Block the poster until space is free.** Tradeoff: `NtSetIoCompletion` must not sleep in the file completion path, so this does not work for file I/O.

## Recommendation

Option 1, with 65536 as the cap. Option 3 is the proper end state.

## Patch (implemented)

- `src/kernel/nt/iocp.h`: `#define NTIO_IOCP_MAX_QUEUED 65536` above `struct nt_iocp`.
- `src/kernel/nt/iocp.c` `ntio_iocp_post`: allocate the packet first (as now), then under `iocp->lock` check `iocp->queued >= NTIO_IOCP_MAX_QUEUED`. On overflow, unlock, `kfree(pkt)`, return `STATUS_INSUFFICIENT_RESOURCES`. Otherwise append and increment as now. The check and the append share one lock acquisition, so the cap is exact.

## Test plan

- `src/kernel/nt/io_test.c` (completion port section): post `NTIO_IOCP_MAX_QUEUED` packets and check success; the next post returns `STATUS_INSUFFICIENT_RESOURCES`; removing one packet lets the next post succeed.
- Check the packet count returns to zero after the port is closed (no leak).
- `tests/kernel-host` is not used.

## Not verified

No C compiler is installed on the machine used for this batch, so the change was not built or run. The test cases above must be run before merge.
