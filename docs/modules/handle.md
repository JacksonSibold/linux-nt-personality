# Module: handle (Per-process handle table)

Sources: `src/kernel/nt/handle.c`, `src/kernel/nt/handle.h`
Related: `object.md` (object references and handle counts), `nt_obj.c` (NtClose, NtDuplicateObject wrappers, not covered here).

## Purpose

Provides a per-process NT handle space that is independent of Linux file descriptors. Each entry maps a HANDLE value to an object header, a granted `ACCESS_MASK`, and attribute flags. Handles are resolved to object references with a type and access check (`ObReferenceObjectByHandle`), support pseudo-handles for the current process and thread, and support duplication and inheritance between processes. The header comment in `handle.c` lists the design differences from fds (typed objects, opt-in inheritance, cross-process duplication, no RLIMIT-style quota).

## Public API (handle.h)

- `NTSTATUS ObCreateHandleTable(struct nt_eprocess *proc, struct nt_handle_table **out_table)` - allocate an empty table owned by `proc`.
- `void ObDestroyHandleTable(struct nt_handle_table *table)` - release all table pages.
- `NTSTATUS ObCreateHandle(struct nt_eprocess *proc, struct nt_object_header *obj, ACCESS_MASK granted_access, ULONG attributes, PHANDLE out_handle)` - allocate an entry and return its handle value.
- `NTSTATUS ObCloseHandle(struct nt_eprocess *proc, HANDLE handle)` - remove the entry and drop the handle count. The `okay_to_close` callback can refuse.
- `NTSTATUS ObReferenceObjectByHandle(struct nt_eprocess *proc, HANDLE handle, struct nt_object_type *type, KPROCESSOR_MODE mode, ACCESS_MASK desired_access, struct nt_object_header **out_obj, ACCESS_MASK *granted_access)` - resolve a handle, check type and desired access, return a referenced object. Pseudo-handles are resolved here and never stored.
- `NTSTATUS NtDuplicateObject(struct nt_eprocess *src_proc, HANDLE src_handle, struct nt_eprocess *dst_proc, PHANDLE dst_handle, ACCESS_MASK desired_access, ULONG attributes, ULONG options)` - copy or move a handle between processes. Options: `DUPLICATE_CLOSE_SOURCE` (0x1), `DUPLICATE_SAME_ACCESS` (0x2).
- `NTSTATUS ObInheritHandles(struct nt_eprocess *src, struct nt_eprocess *dst)` - copy handles marked inheritable into a child, keeping the same numeric value.
- `NTSTATUS ObpCreateHandleAtIndex(struct nt_handle_table *table, struct nt_object_header *obj, ACCESS_MASK granted_access, ULONG attributes, ULONG index, PHANDLE out_handle)` - create an entry at a specific index.
- `ULONG ObHandleTableCount(struct nt_handle_table *table)` and `bool ObHandleIsValid(struct nt_eprocess *proc, HANDLE h)` - debug helpers for tests.

Macros: `NtCurrentProcess()` = (HANDLE)-1, `NtCurrentThread()` = (HANDLE)-2; `HANDLE_FLAG_PROTECT_FROM_CLOSE` (0x1) and `HANDLE_FLAG_INHERIT` (0x2) occupy the low two bits of a handle value.

## Key data structures

- `struct nt_handle_entry` - `obj`, `granted_access`, `attr`, `free`, `next_free`.
- `struct nt_handle_l3` - 256 entries. `struct nt_handle_l2` - 256 pointers to L3 pages. `struct nt_handle_table` - `lock` (spinlock_t), `owner`, `l1[256]` pointers to L2 directories, `first_free`, `high_water`, `num_handles`.
- Index layout: 8 + 8 + 8 bits, so `HANDLE_MAX_INDEX` = 2^24 - 1. Pages are allocated on demand.
- Handle value = index shifted left by 2, with the low bits holding flags. The header states that values are multiples of four and that the free list is LIFO.

## Invariants and locking (as implemented)

- The table is guarded by a per-table `spinlock_t` (about 8 `spin_lock` and 24 `spin_unlock` sites in `handle.c`).
- Free indices are kept in a LIFO free list via `next_free`. `first_free` is the head; `HANDLE_INVALID_FREE` (0xFFFFFFFF) marks the end.
- Handle values have no generation counter. A stale handle can refer to a reused slot, as the header comment notes.
- Pseudo-handle values (-1 and -2) are interpreted in `ObReferenceObjectByHandle` and never written into the table.
- `handle.c` (around lines 305-307) comments on how gaps left by out-of-order index creation are initialised as free entries.

## Known gaps (evident in code)

- `handle.h` line 35 defines `NT_CURRENT_THREAD_HANDLE` with unbalanced parentheses: `((HANDLE)((ULONG_PTR)-2)`. It is not used anywhere in `src/`, so it does not break the build, but it is wrong. `NT_CURRENT_PROCESS_HANDLE` on line 34 is balanced.
- No dedicated handle test file exists. Handle behaviour is exercised indirectly through `object_test.c` and `i2_test.c`.
- The header comment says growth is memory-limited rather than fixed (`handle.c:74-77`). I did not find a limit check in `handle.c` that enforces either a fixed or a memory-based cap, so the table is bounded only by its 2^24 index space.
- `ObpIncrementHandleCount` and `ObpDecrementHandleCount` are called from this module, but their correctness depends on `object.c`. This doc does not verify them.
