# Review: src/kernel/nt (object, handle, registry, sync, security, mm, io, iocp, proc_thread, dispatch, nt_obj)

Scope: `src/kernel/nt/object.{c,h}`, `handle.{c,h}`, `registry.{c,h}`, `sync.{c,h}`, `security.{c,h}`, `mm.{c,h}`, `io.{c,h}`, `iocp.{c,h}`, `proc_thread.{c,h}`, `dispatch.c`, `nt_obj.c`.
Method: read-only static review. Every finding below was checked against the source text at the cited line. Nothing was compiled, sanitized, fuzzed, or run. No source file was modified.
Branch: `haiku/unitc` (worktree `ntpers-unitc`).

Severity: High / Medium / Low / Info. "Latent" means the defect is real in the code but no current caller reaches it from user space (noted per finding).

## Summary

| Severity | Count |
|---|---|
| High | 7 |
| Medium | 9 |
| Low | 18 |
| Info | 1 |

The code is careful about `copy_from_user` at most service entry points, and the object, handle, and dispatch layers are structured clearly. The problems cluster in four places: (1) kernel-heap memory returned to user space without being initialised, (2) user-controlled values used as kernel pointers or table indices without range checks, (3) kernel-build registries that keep pointers to freed objects, and (4) waits and I/O that do not check object type or access rights.

## Findings

### H-1 [High] User handle index is not range-checked before the handle table (fixed 68fe25b)
- `src/kernel/nt/handle.c:95-98` (`handle_to_index` truncates the handle to `u32`), `:105-112` (`ht_entry` asserts `i1 < HANDLE_L1_SIZE` and then indexes `table->l1[i1]`).
- Callers pass the raw user handle with only `idx == 0` rejected: `ObCloseHandle` (`handle.c:347-348`), `ObReferenceObjectByHandle` (`:429-430`), `NtDuplicateObject` (`:472-473`), `ObHandleIsValid` (`:563-566`, no index check at all). The raw value reaches `nt_obj.c:1094` (`nt_svc_NtClose` kernel branch) and `nt_obj.c:674`, `:755`, `:793`, and others.
- `HANDLE_MAX_INDEX` is `2^24 - 1` (`handle.h:48`), so any index at or above `2^24` gives `i1 >= 256`. In a kernel build `KASSERT` is `BUG_ON` (`kshim.h:63`), so `NtClose(0x40000000)` panics the kernel. In a release build without the assert the read goes past `l1[256]`. Truncation to `u32` also means a handle value with high bits set aliases a valid slot.
- Fix: in `handle_to_index` reject values with bits above 32 or indices above `HANDLE_MAX_INDEX`, and return NULL from `ht_entry` instead of asserting for out-of-range indices. Apply the check in all four callers.

### O-1 [High] Destroyed process and thread objects stay in the kernel registries (use after free) (fixed 30b200a)
- `src/kernel/nt/object.c:1174-1189` (`ObDestroyProcessObject`): `ob_unregister_proc(proc)` is inside `#ifndef __KERNEL__` (`:1177-1178`). The kernel branch (`:1183-1185`) frees the object without removing it from `g_ob_proc_list`.
- `src/kernel/nt/object.c:1191-1207` (`ObDestroyThreadObject`): `ObUnregisterThread(thr)` is also inside `#ifndef __KERNEL__` (`:1194-1195`). The kernel build never removes the node from `g_ob_thr_list`.
- After `kfree`, `ObFindProcessByPid` (`:897-906`), `ObFindThreadByTid` (`:938-947`), `ObForEachThreadOfProcess`, `ObSuspendProcessThreads`, `ObResumeProcessThreads`, `ObTerminateProcessThreads` (`:948-1018`), and `nt_current_ethread` (`:1035-1051`, which calls `ObFindThreadByTid` first) all walk freed nodes. The lookups by pid or tid can return a dangling pointer, which is then dereferenced, for example in `nt_svc_NtOpenProcess` (`proc_thread.c:1589-1592`).
- Fix: move `ob_unregister_proc` and `ObUnregisterThread` out of the `#ifndef` block so both builds unregister before `kfree`. Also make the lookup functions take a reference under the registry lock (see P-1).

### S-1 [High] NtWaitForMultipleObjects waits on any object type (type confusion, writes into foreign objects) (fixed 6f58d04)
- `src/kernel/nt/sync.c:2229` calls `ObReferenceObjectByHandle(proc, k_handles[i], NULL, ...)` with a NULL type, so any handle (file, directory, registry key, process, thread) is accepted.
- `sync.c:2235` then treats the body as a dispatcher header: `hdrs[i] = (struct nt_dispatcher_header *)nt_object_body(hdr);`. The wait path reads `h->type` (`:263-265`) and links a wait block into `h->waiters` (`insert_blocks`, used from the wait engine). The layout of, for example, `struct nt_directory` begins with an `rwlock_t`, so the list head and `signal_state` are written into unrelated memory.
- Fix: pass the expected dispatcher types, or check `hdr->type` against the set of types whose body begins with `struct nt_dispatcher_header`, and return `STATUS_OBJECT_TYPE_MISMATCH` otherwise.

### D-1 [High] Uninitialised kernel heap returned to user space by NtQueryVirtualMemory (already correct on haiku/ntfix: dispatch.c:258 uses kzalloc; no change)
- `src/kernel/nt/dispatch.c:258` allocates `kbuf` with `kmalloc(MemoryInformationLength)`, not zeroed.
- `src/kernel/nt/mm.c:1334-1378`: the kernel writes only `sizeof(MEMORY_BASIC_INFORMATION)` (48 bytes) and sets `*ReturnLength = 48`.
- `dispatch.c:264` then calls `copy_to_user(MemoryInformation, kbuf, MemoryInformationLength)`, copying the caller-chosen length. Every byte beyond 48 is uninitialised kernel heap.
- Fix: `kzalloc`, or copy only the bytes the call reported (`kret`).

### R-1 [High] Uninitialised kernel heap returned to user space by NtQueryValueKey (fixed 0254b09)
- `src/kernel/nt/registry.c:1621` (kernel branch of `nt_svc_NtQueryValueKey`; the shim at `:1448` is the same) allocates `kout = kmalloc(len)`.
- `NtQueryValueKey` (`registry.c:1079-1140`) fills only `needed` bytes when `Length >= needed`, then the wrapper runs `copy_to_user(info, kout, len)` at `:1625` for the full `len`. The tail is uninitialised. Fix: `kzalloc`, or copy only `min(needed, len)`.

### M-1 [High] Kernel pointer returned as section handle and used unvalidated
- `src/kernel/nt/mm.c:1498`: `NtCreateSection` returns `*SectionHandle = (HANDLE)sec;`, a kernel address, to the caller.
- `src/kernel/nt/mm.c:1507-1515`: `NtMapViewOfSection` casts `SectionHandle` to `struct nt_section *` and reads `sec->attributes`, `sec->size`, `sec->page_protection` and `sec->backing` (`:1524-1552`) without any handle-table lookup or type check. `NtMapViewOfSection` is registered as a service (`nt_service_list.h:34`).
- Impact: address disclosure through the returned handle (KASLR), and a forged handle makes the kernel dereference an arbitrary address and feed `sec->backing` into `kshim_section_map` (`kshim.c:117-137`).
- Fix: return a real handle from the object table, look it up with type checking in `NtMapViewOfSection`, and reject unknown values with `STATUS_INVALID_HANDLE`.
- Cross-reference: arch-x86-nt.md M-2 reports the same defect on the other tree.

### P-1 [High] Process and thread can be opened by ID with no access check, and the lookup result is used unreferenced
- `src/kernel/nt/proc_thread.c:1589` (`p = ObFindProcessByPid(pid)`) returns a pointer that is not referenced. The registry lock is released inside `ObFindProcessByPid` (`object.c:897-906`), so the object can be freed before `ObCreateHandle(proc, p->obj, ...)` at `proc_thread.c:1592` runs. The same applies to `NtOpenThread` at `:1610-1616` (`ObFindThreadByTid`).
- No caller check is made: `ObjectAttributes` is discarded (`:1579`), and `DesiredAccess == 0` becomes full access `0x1FFFFF` (`:1592`). Any process can open, and then suspend or terminate, any other process by PID.
- Fix: take an object reference inside the registry lock, add an owner or privilege check, and mask the granted access to what the caller asked for.

### H-2 [Medium] Closed handle slots are never reused, so a process runs out of indices
- **Status:** partly fixed in d7adc94 (decision 0003, option 1, FIFO quarantine). Aliasing is narrowed, not removed. The generation counter (option 2) is still open for the ABI review.
- `src/kernel/nt/handle.c:143-161`: the free-list path is disabled with `if (false && table->first_free != ...)`. Every allocation uses `high_water`, and `high_water` only increases.
- Each L3 page (256 entries, about 6 KiB) stays allocated until `ObDestroyHandleTable`. A process that opens and closes about 16.7 million handles exhausts the 2^24 index space and gets `STATUS_INSUFFICIENT_RESOURCES`, with the memory still held.
- The comment at `handle.c:359-375` justifies this as avoiding handle aliasing. Aliasing can instead be prevented with a generation counter in the entry.
- Fix: add a generation field and restore the free list, or recycle slots with a generation check.

### M-2 [Medium] Section view size and offset checks can be bypassed by integer wrap (already correct on haiku/ntfix: offset checked before arithmetic at mm.c:1516-1527; no change)
- `src/kernel/nt/mm.c:1517` (`offset = SectionOffset->QuadPart`, unchecked against `sec->size`), `:1524-1526`: `actual_size = sec->size - offset` underflows when `offset > sec->size` and `*ViewSize == 0`. The check `offset + actual_size > sec->size` then cannot fail, because the sum wraps back to `sec->size`.
- With a nonzero `*ViewSize`, `offset + actual_size` can wrap to a small value for a large aligned offset, passing the check.
- Result: a VAD and a `kshim_section_map` covering memory outside the section's backing. Fix: check `offset <= sec->size` and `actual_size <= sec->size - offset` before the arithmetic.

### M-3 [Medium] Section objects are never freed
- `src/kernel/nt/mm.c:1450-1500`: `NtCreateSection` allocates `sec` and its backing (`kshim_section_create`). `kfree(sec)` appears only on error paths. No close or destroy routine exists, and `vad->section` keeps a pointer to the section after unmap.
- Each successful `NtCreateSection` leaks `sec` and the backing store (the `size` bytes of the section) for the life of the system. This is an unprivileged memory exhaustion path.
- Fix: make the section an object-manager object with a delete callback that releases `sec->backing`, and drop the reference when the last handle and last view are gone.

### M-4 [Medium] Failed commit or decommit leaves the VAD table and the real mapping out of step
- **Status:** blocked. Decision 0005 needs an owner; see the note in that record.
- `src/kernel/nt/mm.c:1061-1075`: the rollback after a failed `kshim_mmap_commit` is a no-op (the comment at `:1071` says "approximate; tests don't hit rollback path"). The VAD stays `MEM_COMMIT` while no pages are mapped. `NtQueryVirtualMemory` then reports committed memory that faults on access.
- `mm.c:1205-1237` (`NtFreeVirtualMemory`, MEM_DECOMMIT): the loop splits and changes VADs to `MEM_RESERVE` while walking. The early return at `:1227` (`STATUS_INVALID_PARAMETER`) and the `nt_vad_split` failure at `:1219` leave earlier pieces already changed, with no undo.
- Cross-reference: arch-x86-nt.md M-4 and M-5.
- Fix: record the original state of each piece before mutating, and restore it on any error before returning.

### IO-1 [Medium] NtReadFile and NtWriteFile do not check FILE_READ_DATA or FILE_WRITE_DATA (read half fixed 1d160e8; write half fixed 80b6fea)
- `src/kernel/nt/io.c:1890` (`nt_svc_NtReadFile`) and `io.c:1957` (`nt_svc_NtWriteFile`) call `ntio_handle_lookup(FileHandle, 0)`. A `required` value of 0 skips the granted-access test in `ntio_handle_lookup` (`io.c:402-428`).
- A handle opened with only `SYNCHRONIZE` or `FILE_READ_ATTRIBUTES` can therefore be read or written.
- Fix: pass `FILE_READ_DATA` and `FILE_WRITE_DATA` as `required`, and keep `FILE_APPEND_DATA` handling consistent.

### IO-2 [Medium] File handle table is global, and close has no owner check
- `src/kernel/nt/io.c:352-367` (`ntio_handle_create`) and `:402-505` store file handles in one table shared by all processes. `ntio_handle_close` (`:488-505`) accepts any index in range and closes it, regardless of which process created it.
- Any caller can close another caller's file handle, or read it via `ntio_handle_peek` (`:453-465`), which takes no reference.
- Fix: move file handles into the per-process handle table (`handle.c`), or record an owner and check it in `ntio_handle_close`.

### PT-1 [Medium] Kernel heap address of the TEB returned by NtQueryInformationThread
- `src/kernel/nt/proc_thread.c:1007-1010`: the TEB is `kzalloc`ed, so `teb_base` is a kernel heap address.
- `proc_thread.c:1817` writes `tbi.TebBaseAddress = target->teb_base` to the caller. The target can be any thread (see P-1). This discloses a kernel heap address, which weakens KASLR.
- Fix: return the user-space TEB address, or refuse the query unless the caller owns the thread.

### IOCP-1 [Medium] Concurrency count is never decremented, so a port stops after N removals
- `src/kernel/nt/iocp.c:117-123` (`ntio_iocp_dequeue_one`) increments `active` when it hands out a packet. The only decrement is in `ntio_iocp_release_concurrency` (`:130-137`), which is marked `__attribute__((unused))` and has no caller.
- With a port created for N threads, after N packets have been removed `iocp->active >= iocp->concurrency` holds forever, and every later `NtRemoveIoCompletion` returns a timeout. This is a functional denial of service on any port that is used with a concurrency limit.
- Fix: call `ntio_iocp_release_concurrency` when a thread finishes handling a packet, or decrement `active` when the packet is returned to the caller and the caller is done.

### IOCP-2 [Medium] Completion queue has no quota
- `src/kernel/nt/iocp.c:80-108` (`ntio_iocp_post`) allocates one packet per post with `kzalloc` and never checks `iocp->queued`.
- Any caller that can open a port can post in a loop until memory runs out.
- Fix: cap `queued` (for example 64K) and return `STATUS_INSUFFICIENT_RESOURCES` above the cap.

### H-3 [Low] NtDuplicateObject ignores the result of closing the source handle
- `src/kernel/nt/handle.c:512-515`: after the duplicate succeeds, `ObCloseHandle(src_proc, src_handle)` is called and its status is discarded. The source can refuse to close (`PROTECT_FROM_CLOSE` at `:353`, or `okay_to_close`), so the call reports success while the source stays open. The comment at `:513` acknowledges this.
- Fix: check the status before creating the duplicate, or undo the duplicate on failure.

### H-4 [Low] Handle-table page pointers are read outside the lock
- `src/kernel/nt/handle.c:252-253`: `ObCreateHandle` calls `ht_entry(table, idx, false)` after `ht_alloc_index` has released `table->lock`. `ht_entry` reads `table->l1[i1]` and `l2->l3[i2]`, which another thread can write under the lock during its own allocation (`handle.c:113-134`). The pointer read is unsynchronised.
- Fix: keep the lock across the `ht_entry` call, or return the entry pointer from `ht_alloc_index`.

### H-5 [Low] okay_to_close runs with the table spinlock held
- `src/kernel/nt/handle.c:350-358`: `obj->type->ops.okay_to_close` is called inside `spin_lock(&table->lock)`. No implementation exists in the tree, but a future one that calls `ObReferenceObjectByHandle` on the same process would deadlock.
- Latent. Fix: document the contract in `object.h:80-83`, or call it after releasing the lock with a re-check.

### O-2 [Low] Exclusive-open check is not atomic with handle creation
- `src/kernel/nt/object.c:638-653` (`ObOpenObjectByName`): the test `is_exclusive && handle_count > 0` and the later `ObCreateHandle` are not under one lock. Two opens can both pass.
- Fix: perform the check and the handle-count increment under a single lock, or use `atomic_cmpxchg`.

### O-3 [Low] Symbolic link resolution has no depth limit
- `src/kernel/nt/object.c:312-329` (`ObpResolveSymlink`) calls `ObReferenceObjectByName(&link->target, ...)`, which can reach `ObpResolveSymlink` again for a symlink in the final component (`:400-403`). A link whose target names itself recurses until the kernel stack overflows.
- Latent: no code in this tree creates symlink objects (`NtCreateSymbolicLinkObject` is absent), so only internal code could create one. Fix: pass a depth counter (for example 32) and return `STATUS_NAME_TOO_LONG` or `STATUS_OBJECT_PATH_NOT_FOUND` beyond it.

### O-4 [Low] nt_dup_unicodestring: USHORT wrap for lengths 0xFFFE
- `src/kernel/nt/object.c:62-68`: `max_len = (USHORT)(len + sizeof(WCHAR))`. For `len == 0xFFFE` the sum is `0x10000`, which truncates to 0. `kmalloc(0)` then gives a zero-size allocation, and `memcpy(dst->Buffer, src->Buffer, len)` at `:79` writes 65534 bytes into it.
- Latent: the only caller is `ObpInsertDirectoryEntry`, and its names come from `ObInsertObject` (`:517`), which cannot exceed `0xFFFC`. The helper is still wrong for its own contract. Fix: reject `len > 0xFFFC` with `STATUS_NAME_TOO_LONG`, or compute the size in `size_t`.
- The same defect is in `registry.c:182` (see R-2).

### O-5 [Low] ObMakeTemporaryObject may write to an object it has just freed
- `src/kernel/nt/object.c:656-664`: `ObpRemoveFromDirectory(obj)` drops the directory's reference (`:305`). If that was the last reference, the object is freed, and then `obj->flags &= ~OBJ_PERMANENT` (`:661`) writes to freed memory.
- Latent: the only caller, `nt_svc_NtMakeTemporaryObject` (`nt_obj.c:755-761`), holds a reference from `ObReferenceObjectByHandle`. Fix: clear the permanent flag before the removal, or take a local reference across the call.

### O-6 [Low] Registry insertion failures are ignored
- `src/kernel/nt/object.c:1119` (`ob_register_proc(p)`) and `:1170` (`ObRegisterThread(t)`): the return values are dropped. On allocation failure the process or thread is created but absent from the registry, so `ObFindProcessByPid` and `ObFindThreadByTid` cannot find it and `NtOpen*` by ID fails.
- Fix: check the return values and fail the creation with `STATUS_NO_MEMORY`.

### M-5 [Low] Round-up of a huge RegionSize wraps to zero
- `src/kernel/nt/mm.c:860-873` (`NtAllocateVirtualMemory`): `ntmm_round_up(orig_size, PAGE)` at `:863` and `:873` (and `orig_size + delta` at `:860`, `:870`) wraps for sizes above `2^64 - 4096`. `req_size` becomes 0, and a zero-length VAD is inserted.
- The later `kshim_mmap_reserve` with length 0 fails, so the caller gets `STATUS_NO_MEMORY` after a VAD insert and remove. The outcome is wrong, not unsafe. Fix: check `orig_size > SIZE_MAX - delta - PAGE_SIZE` before rounding.

### M-6 [Low] MEM_DECOMMIT and NtProtectVirtualMemory compute end = base + size without an overflow check
- `src/kernel/nt/mm.c:1205` (`NtFreeVirtualMemory`) and `:1268` (`NtProtectVirtualMemory`): `end = base + size` wraps when the range crosses the top of the address space. The walk loop then does nothing, but `kshim_mmap_commit` (`:1237`) still runs with the unmodified `size`.
- Fix: reject `size > ULONG_PTR_MAX - base` before the sum.

### M-7 [Low] File-backed sections ignore the file
- `src/kernel/nt/mm.c:1475`, `:1485`: `NtCreateSection` passes the `FileHandle` value, cast to `const char *`, to `kshim_section_create_file`. That function discards the path and creates an anonymous backing of the requested size (`kshim.c:111-115`). The handle is never looked up, so a mapped "file" section is zeroed memory, not file contents.
- Fix: look up the file object by handle and back the section with its page cache, or return `STATUS_NOT_IMPLEMENTED` until it does.

### SYNC-1 [Low] nt_wait_on_address dereferences caller pointers with no access check
- `src/kernel/nt/sync.c:1828-1836`: `*(volatile uint32_t *)addr` and `*(uint32_t *)cmp` read the addresses directly.
- Latent: no non-test caller exists (`sync_test.c:290` and `:308` only). Fix: `copy_from_user` (or `get_user`) both values, and reject non-user addresses before the load.

### SYNC-2 [Low] timeout_to_jiffies dereferences non-user pointers
- `src/kernel/nt/sync.c:1421-1433`: when `access_ok(to)` fails, `t = *to` reads the pointer directly. This is correct for kernel copies and wrong for user-controlled addresses. The three callers at `:1701`, `:1751`, `:1839`, `:1967` must always pass a kernel copy. `nt_svc_NtWaitForMultipleObjects` does (`:2214-2218`). The other callers were not traced.
- Fix: make the helper accept only kernel pointers (`WARN_ON`, return `-1` otherwise), or copy in every caller.

### SEC-1 [Low] ACL capacity table silently drops registrations
- `src/kernel/nt/security.c:40-70`: `g_acl_caps` has 256 slots. `acl_capacity_register` (`:57-63`) silently does nothing when full. `acl_capacity_lookup` (`:47-55`) then falls back to `acl->AclSize`, which is the used size for ACLs built here, so `acl_has_space` (`:489-493`) reports no room and additions fail.
- Latent: `security.c` has no call sites outside its tests (see `docs/modules/security.md`). Fix: store the capacity in the ACL allocation, or grow the table and report `STATUS_INSUFFICIENT_RESOURCES` on overflow.

### SEC-2 [Low] Self-relative SD import reads ACL header before the bounds check
- `src/kernel/nt/security.c:864` and `:874`: `a->AclSize` is read from `sr_buffer + sr->Sacl` (or `Dacl`) before checking that the 8-byte ACL header lies inside `sr_length`. The check that follows in the same condition compares only `offset + AclSize`.
- Latent: `sd_from_self_relative` has no non-test caller. Fix: check `offset + sizeof(ACL) <= sr_length` first.

### R-2 [Low] dup_us: USHORT wrap for lengths 0xFFFE
- `src/kernel/nt/registry.c:179-188`: `USHORT max = src->Length + sizeof(WCHAR)` has the same wrap as O-4. `kmalloc(0)` then `memcpy(b, src->Buffer, src->Length)` at `:185`.
- Latent: the only user-reachable caller, `NtSetValueKey`'s `ValueName` (`registry.c:1030`), arrives through `k_copy_us_us`, which caps it at 512 bytes (`registry.c:1339`). Other callers (`:534`, `:796`, `:803`, `:845-866`) take names built from path components. Fix: as O-4.

### R-3 [Low] build_full_path indexes buf[p-1] when the root key path is empty
- `src/kernel/nt/registry.c` (`build_full_path`, around lines 605-610): `if (buf[p-1] != '\\')` runs with `p == 0` when `rk->full_path.Length == 0`, reading `buf[-1]`.
- Latent: key paths are never empty in the current tree. Fix: test `p > 0 &&`.

### D-2 [Low] Debug log on a user-triggerable path
- `src/kernel/nt/dispatch.c:113-115`: `pr_info` runs for six specific SSNs on every call, from any caller. A loop can flood the kernel log.
- Fix: remove, or switch to `pr_debug` or a ratelimited call.

### I-1 [Info] Stale comment about the call that frees the mapping
- `src/kernel/nt/mm.c:1610-1613`: the comment says `vm_munmap` is called, but that line is commented out. The active call is `kshim_munmap` at `:1614`. Same behaviour, but the comment misleads the next reader.

## Fix status (branch haiku/ntfix)

Fixed in this branch: H-1 (68fe25b), R-1 (0254b09; all eight user-output buffers in registry.c), S-1 (6f58d04), O-1 (30b200a), IO-1 read half (1d160e8). Already correct here, no change: D-1, M-2.

Fixed on branch haiku/adr: IO-1 write half (80b6fea; any-of lookup, `ntio_handle_lookup_any_of`) and IOCP-2 (0b7921b; `NTIO_IOCP_MAX_QUEUED` = 65536). Neither commit has been compiled or run; no C compiler was available on the machine.

Not fixed. Each has a design record in `docs/decisions/`:

- M-1: NtCreateSection returns a kernel address as the handle. Needs a real object-table handle and a type-checked lookup in NtMapViewOfSection. Record 0001 (object-model change).
- P-1: NtOpenProcess and NtOpenThread have no access or owner check and use the registry result unreferenced. Needs a policy for who may open which process. Record 0002.
- H-2: closed handle slots are never reused. Recommended: FIFO quarantine now, generation counter later (ABI). Record 0003.
- M-3: section objects are never freed. Needs an object type with a delete callback; depends on M-1. Record 0004.
- M-4: VAD rollback on failed commit or decommit. Recommended: validate, preallocate, then mutate. Record 0005.
- IO-2: file handle table is global. Interim owner field, long-term per-process table. Record 0007 (owner rule for duplicates needs a decision).
- PT-1: TEB address returned to callers is a kernel heap address. Needs the TEB user mapping read first. Record 0008.
- IOCP-1: concurrency count is never released. Needs a decision on when a thread counts as finished. Record 0009.

Low findings are not addressed in this pass (M-5 and M-6 are still present, as are the others).

## Not verified / out of scope

- Nothing was compiled, linked, sanitized, fuzzed, or executed. Overflow and wrap claims come from the arithmetic at the cited lines.
- Read in full: `object.c`, `handle.c`, `dispatch.c` (except the header section).
- Read in part: `mm.c` (lines 802-1644, the allocation, free, protect, query, and section paths; the helper section at 1-800 was not read), `io.c` (handle table, path conversion, read, query and set-information copies; the create and open path at `:1228-1540` and the rename and disposition code at `:1080-1227` were not read), `proc_thread.c` (information-class handling and the kernel creation path around `:932-1200`; the shim branch `:83-931` was not read), `sync.c` (wait engine entry points and the address-wait code; the keyed-event and waiter lists were not read), `registry.c` (value and key write paths and the service wrappers), `security.c` (ACL and SID helpers; the token and access check code was not read), `iocp.c` (`:150-736`), `nt_obj.c` (file, event, and object query wrappers in part).
- Header files were spot-checked for the structures used above (`handle.h`, `object.h`, `sync.h`, `kshim.h`). `mm.h`, `io.h`, `iocp.h`, `proc_thread.h`, `registry.h`, `security.h` were not read in full.
- Finding IDs are local to this report. Cross-references to `arch-x86-nt.md` refer to the same defect in the other review tree.
