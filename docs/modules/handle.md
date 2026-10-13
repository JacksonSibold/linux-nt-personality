# Module: handle (Per-process handle table)

Sources: `src/kernel/nt/handle.c`, `src/kernel/nt/handle.h`
Related: `object.md` (object references and handle counts), `nt_obj.c` (the `NtClose`, `NtDuplicateObject` and handle-flag wrappers, summarised in [object](object.md)), `proc_thread.md` (the shim calls inheritance at process creation), `iocp.md` and `io.md` (handles to their objects).

## Purpose

Provides a per-process NT handle space that is independent of Linux file descriptors. Each entry maps a handle value to an object header, a granted `ACCESS_MASK`, and attribute flags. Handles are resolved to object references with a type and access check (`ObReferenceObjectByHandle`), support pseudo-handles for the current process and thread, and support duplication between processes and inheritance into a new table.

The header comment in `handle.c` (lines 60 to 80) compares the design with Linux file descriptors: typed objects, opt-in inheritance, cross-process duplication, and no `RLIMIT_NOFILE`-style quota.

## Public API (handle.h)

- `NTSTATUS ObCreateHandleTable(struct nt_eprocess *proc, struct nt_handle_table **out_table)` - an empty table owned by `proc`.
- `void ObDestroyHandleTable(struct nt_handle_table *table)` - releases all table pages.
- `NTSTATUS ObCreateHandle(struct nt_eprocess *proc, struct nt_object_header *obj, ACCESS_MASK granted_access, ULONG attributes, PHANDLE out_handle)` (`handle.c:251`) - allocates an entry and returns its handle value.
- `NTSTATUS ObpCreateHandleAtIndex(struct nt_handle_table *table, ..., ULONG index, PHANDLE out_handle)` (`handle.c:278`) - creates an entry at a given index. Gap slots it passes over are put on the free list.
- `NTSTATUS ObCloseHandle(struct nt_eprocess *proc, HANDLE handle)` (`handle.c:351`) - removes the entry, drops the handle count and runs the type's `ops.close`. The `okay_to_close` callback can refuse.
- `NTSTATUS ObGetHandleFlags` (`handle.c:410`) and `ObSetHandleFlags` (`handle.c:434`) - read and set the per-handle flags (`HANDLE_FLAG_PROTECT_FROM_CLOSE`, `HANDLE_FLAG_INHERIT`). `NtQueryObject` class `ObjectHandleFlagInformation` reads them.
- `NTSTATUS ObReferenceObjectByHandle(struct nt_eprocess *proc, HANDLE handle, struct nt_object_type *type, KPROCESSOR_MODE mode, ACCESS_MASK desired_access, struct nt_object_header **out_obj, ACCESS_MASK *granted_access)` (`handle.c:459`) - resolves a handle, checks type and desired access, and returns a referenced object. Pseudo-handles are resolved here and never stored.
- `NTSTATUS NtDuplicateObject(...)` (`handle.c:520`) - copies or moves a handle between processes. Options: `DUPLICATE_CLOSE_SOURCE` (0x1) and `DUPLICATE_SAME_ACCESS` (0x2). The service wrapper is in `nt_obj.c`.
- `NTSTATUS ObInheritHandleTable(struct nt_handle_table *src, struct nt_handle_table *dst, u32 *copied)` (`handle.c:646`) - copies the `OBJ_INHERIT` entries of `src` into `dst` at the same indices with the same granted access. All or nothing: a failed copy undoes the copies made so far without running type callbacks.
- `NTSTATUS ObInheritHandles(struct nt_eprocess *src, struct nt_eprocess *dst)` (`handle.c:682`) - a wrapper over `ObInheritHandleTable` with no count.
- `ULONG ObHandleTableCount(...)` and `bool ObHandleIsValid(...)` - debug helpers for tests.

Macros: `NtCurrentProcess()` is `(HANDLE)-1` and `NtCurrentThread()` is `(HANDLE)-2` (`handle.h:31`, `:32`). `HANDLE_FLAG_PROTECT_FROM_CLOSE` (0x1) and `HANDLE_FLAG_INHERIT` (0x2) occupy the low two bits of a handle value.

## Key data structures

- `struct nt_handle_entry` - `obj`, `granted_access`, `attr`, `free`, `next_free`.
- `struct nt_handle_l3` - 256 entries. `struct nt_handle_l2` - 256 pointers to L3 pages. `struct nt_handle_table` - a spinlock, `owner`, `l1[256]` pointers to L2 directories, `first_free`, `last_free`, `free_count`, `high_water` and `num_handles`.
- Index width: 8 + 8 + 8 bits (`HANDLE_INDEX_BITS`), so `HANDLE_MAX_INDEX` is 2^24 - 1 (`handle.h:47`, `:48`). Pages are allocated on demand.
- Handle value = index shifted left by 2, with the low bits holding flags.

## Invariants and locking (as implemented)

- The table is guarded by a per-table spinlock.
- Closed indices go on a FIFO free list linked by `next_free`. A closed index goes to the tail (`last_free`, `handle.c:387`-`:388`). A slot is reused only when `free_count > HANDLE_QUARANTINE` (4096, `handle.h:80`), in `ht_alloc_index` (`handle.c:145`, `:152`). `HANDLE_INVALID_FREE` (0xFFFFFFFF) marks the end of the list.
- Indices above `HANDLE_MAX_INDEX` are rejected.
- Handle values have no generation counter. A stale handle can refer to a reused slot after quarantine (ADR 0003, option 2, open).
- Pseudo-handle values (-1 and -2) are interpreted in `ObReferenceObjectByHandle` and never written into the table.
- Inheritance copies only entries with `HANDLE_FLAG_INHERIT` (`handle.c:658`). `PROTECT_FROM_CLOSE` is not carried over. Each copy takes one reference and one handle count.

## Known gaps (evident in code)

- `handle.h:35` defines `NT_CURRENT_THREAD_HANDLE` as `((HANDLE)((ULONG_PTR)-2)`, with unbalanced parentheses. It is unused in `src/`, so the build is not affected. `NT_CURRENT_PROCESS_HANDLE` on the line above is balanced.
- The `handle.c` header comment (line 22 and line 60) still describes a LIFO free list. The code is FIFO with quarantine.
- The header comment (lines 74 to 80) says growth is memory-limited. No limit check in `handle.c` enforces a memory or fixed cap, so the table is bounded only by its 2^24 index space.
- No dedicated handle test file exists. Coverage comes from `object_test.c`, `i2_test.c`, and `handle_inherit_test.c`, which covers the all-or-nothing inheritance (rollback on a colliding slot, reference and handle counts, `PROTECT_FROM_CLOSE` not inherited, and NULL tables).
- The shim calls `ObInheritHandles` at process creation and ignores its status (`proc_thread.c:426`). The kernel build calls no inheritance path (process-creation milestone M4).
- `ObpIncrementHandleCount` and `ObpDecrementHandleCount` are defined in `object.c`, and their correctness is not verified here.
