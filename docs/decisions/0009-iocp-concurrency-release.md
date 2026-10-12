# 0009: Concurrency slot is never released (IOCP-1)

Status: Proposed. Needs a decision on when a thread stops counting as active. Not implemented.
Finding: `docs/review/kernel-nt.md` IOCP-1 (Medium).
Date: 2026-10-12

## Context

`ntio_iocp_dequeue_one` (`src/kernel/nt/iocp.c:117-123`) increments `iocp->active` each time it hands out a packet. The only decrement is `ntio_iocp_release_concurrency` (`iocp.c:130-137`), which is marked `__attribute__((unused))` and has no caller. Once `active >= concurrency`, the checks at `iocp.c:117`, `:266` and `:482` all fail, so removal returns a timeout forever. A port created with `NumberOfConcurrentThreads = N` stops after N packets have been removed.

## Options

1. **Release when the removing thread next removes or exits (closest to Windows).** Record the thread that took each packet (a `struct nt_ethread *` on the packet), and in `ntio_iocp_dequeue_one` or the removal entry point decrement `active` for packets previously taken by the calling thread. Also release on thread exit. Tradeoff: needs a per-thread association and a thread-exit hook. Touches the thread object.
2. **Release when the packet is handed out.** Decrement `active` in `ntio_iocp_dequeue_one` immediately. Tradeoff: the limit then never blocks anything; the concurrency value becomes advisory. Simple and safe, but it is a semantic change that removes the feature.
3. **Release on a timer or on the next removal by any thread.** Tradeoff: not a Windows model and hard to reason about.
4. **Wire up the unused function on a simple rule** (for example, decrement on each `NtRemoveIoCompletion` return). Tradeoff: counts calls, not threads, so one thread can hold several slots.

## Recommendation

Option 1, which needs a per-thread owner field and a thread-exit hook. The owner must choose between option 1 (correct, more code) and option 2 (simple, drops the limit). Neither is a small local change, so this record does not implement either.

## Patch sketch (option 1)

- `src/kernel/nt/iocp.h:25-40` (`struct nt_iocp_packet`): add `void *taken_by;`.
- `iocp.c:117-123` (`ntio_iocp_dequeue_one`): set `pkt->taken_by = nt_current_ethread()` when `active++`.
- The removal entry points (`iocp.c:266`, `:482` region): before the `active < concurrency` test, call `ntio_iocp_release_for_thread(iocp, nt_current_ethread())`, which walks a per-port list of taken-but-unfinished packets and decrements `active` for each packet owned by this thread.
- `iocp.c:130-137`: make `ntio_iocp_release_concurrency` take the thread and call it from `nt_svc_*` removal paths and from the thread-exit path in `proc_thread.c`.
- The packet is freed after its handoff; the taken list needs its own pointer, so the packet is kept until released.

## Test plan

- `src/kernel/nt/io_test.c` (completion port section near `:673-683`): create a port with concurrency 2, post 5 packets, remove them from one thread with the release rule; all 5 are removed. Without the fix, the third removal times out.
- Thread exit with an outstanding packet releases its slot.
- `tests/kernel-host` is not used.

## Open questions

- Whether option 2 is acceptable (the concurrency limit is dropped). If the owner accepts that, the change is a one-line decrement and can be done in a later batch.
