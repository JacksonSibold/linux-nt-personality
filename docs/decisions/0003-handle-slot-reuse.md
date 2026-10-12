# 0003: Handle slot reuse and index exhaustion (H-2)

Status: Proposed. Changes handle-table internals and needs an aliasing policy. Not implemented.
Finding: `docs/review/kernel-nt.md` H-2 (Medium).
Date: 2026-10-12

## Context

`ht_alloc_index` (`src/kernel/nt/handle.c:143-161`) has its free-list path disabled with `if (false && table->first_free != HANDLE_INVALID_FREE)`. Every allocation takes `high_water`, which only increases. `ObCloseHandle` (`handle.c:355-380`) sets `e->free = true` and does not put the slot on the free list. The comment at `:359-375` gives the reason: reuse can alias a just-closed handle value.

Effects:

- A process that opens and closes about 2^24 handles gets `STATUS_INSUFFICIENT_RESOURCES` from the `idx > HANDLE_MAX_INDEX` check (`handle.c:150-152`). Its L3 pages (256 entries, about 6 KiB each) stay allocated until `ObDestroyHandleTable`.
- Memory held by closed handles grows without bound for a long-lived process that churns handles.

The aliasing concern is real, but it can be narrowed without disabling reuse.

## Options

1. **FIFO quarantine (recommended now).** Restore the free list as a FIFO. Closed slots are appended at the tail; a slot is popped from the head only when `free_count > HANDLE_QUARANTINE`. A closed value is then not reused for at least `HANDLE_QUARANTINE` allocations. Tradeoff: narrows the aliasing window rather than closing it. A stale value can still alias after enough churn.
2. **Generation counter encoded in the handle value.** Store a generation in each entry and place it in the upper bits of the handle. Tradeoff: changes the handle value format that user space sees (`handle_to_index`, `handle.c:95-98`, truncates to `u32`). This is ABI-visible and is out of scope for this batch. It is the proper end state.
3. **Free empty L3 pages.** Release an L3 page when all of its entries are free. Tradeoff: fixes memory growth only when free slots cluster; does not fix index exhaustion.
4. **Document and keep current behaviour.** Tradeoff: the exhaustion path stays.

## Recommendation

Option 1 now, with `HANDLE_QUARANTINE` as a constant. Option 2 belongs with the ABI review.

## Patch sketch (option 1)

- `src/kernel/nt/handle.h`: add `u32 last_free;` and `u32 free_count;` to `struct nt_handle_table`, and `#define HANDLE_QUARANTINE 4096`.
- `src/kernel/nt/handle.c:143-161` (`ht_alloc_index`): change the guard to `if (table->free_count > HANDLE_QUARANTINE && table->first_free != HANDLE_INVALID_FREE)`. On pop, advance `first_free` (`e->next_free`), and if the list becomes empty set `last_free = HANDLE_INVALID_FREE`. Decrement `free_count`.
- `ObCloseHandle` (`handle.c:372-380` area, after `e->free = true`): append `idx` at the tail (`ht_entry(table, table->last_free)->next_free = idx` when the list is non-empty, else set `first_free = idx`), set `e->next_free = HANDLE_INVALID_FREE`, update `last_free`, increment `free_count`.
- `ObDestroyHandleTable`: no change.

## Test plan

- `src/kernel/nt/object_test.c`: loop 10,000 times on open and close. Check that `high_water - free_count` stays bounded and that L3 page count does not grow past one page.
- Aliasing: close a handle, make fewer than `HANDLE_QUARANTINE` new handles, and check the old value still fails with `STATUS_INVALID_HANDLE`.
- `tests/kernel-host` is not used.

## Open questions

- Whether `HANDLE_QUARANTINE` should scale with table size. A fixed value is the recommendation for now.
