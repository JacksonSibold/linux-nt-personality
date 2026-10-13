# Review: src/kernel/nt (object, handle, registry, sync, security, mm, io, iocp, proc_thread, dispatch, nt_obj)

Scope: `src/kernel/nt/object.{c,h}`, `handle.{c,h}`, `registry.{c,h}`, `sync.{c,h}`, `security.{c,h}`, `mm.{c,h}`, `io.{c,h}`, `iocp.{c,h}`, `proc_thread.{c,h}`, `dispatch.c`, `nt_obj.c`.
Method: read-only static review. Every finding below was checked against the source text at the cited line. Nothing was compiled, sanitized, fuzzed, or run. No source file was modified.
Branch: `haiku/unitc` (worktree `ntpers-unitc`).

Severity: High / Medium / Low / Info. "Latent" means the defect is real in the code but no current caller reaches it from user space (noted per finding).

## Summary

| Severity | Count |
|---|---|
| High | 8 |
| Medium | 9 |
| Low | 19 |
| Info | 1 |

The C-* findings are the exception to the static method above. They were found by the concurrency stress programs in `tests/kernel-host` (`make stress`, branch `haiku/stress`, 2026-10-12, 16 workers, seeds 1 to 3). C-1 was reproduced and fixed. C-2 was not reproduced. The stress run also found no handle-table, sync, or allocation-leak defects.

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

### M-1 [High] Kernel pointer returned as section handle and used unvalidated (fixed c8f479e, record 0001)
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

### C-1 [High] Concurrent ObMakeTemporaryObject drops the directory reference more than once (fixed ecc7f83)
- `src/kernel/nt/object.c`, `ObpRemoveFromDirectory`: the function read `obj->parent_dir` and dropped the directory's reference without re-checking under the directory lock. Two threads calling `ObMakeTemporaryObject` on the same named object (for example, two handles with delete access) could both see the same non-NULL `parent_dir`. The second call found no entry but still called `ObDereferenceObject`, so the count fell below the number of live references.
- Found by `make stress` in `tests/kernel-host` (`stress_object_test`, its make-temporary round). Before the fix the count read 9 where 16 was expected, and then `KASSERT(c >= 0)` fired in `ObDereferenceObject`. The random phase hit the same assertion at seeds 1 to 3 with 8 threads.
- Fix `ecc7f83`: re-check `parent_dir` under the directory's write lock, and drop the reference only when this call unlinked the entry. Regression: `50dbf92` (round in `stress_object_test.c`, which failed before the fix). Free-time poisoning under `KSHIM_POISON=1` that supports the check: `a164b43`.
- Not the same as O-5: O-5 is a single-caller ordering question, and this is a concurrent double drop.

### C-2 [Low] Named insert publishes the directory entry before setting parent_dir (fixed d963217)
- `src/kernel/nt/object.c`, `ObpInsertDirectoryEntry`: the entry is linked and the write lock is released, then `ObReferenceObject(obj)` runs, and only after that `obj->parent_dir` and `obj->name` are set, without the lock. A thread that finds the name in that window (`ObReferenceObjectByName`) and calls `ObMakeTemporaryObject` sees `parent_dir == NULL` and returns. The delete is lost and the name stays in the directory until a later delete.
- Not reproduced by `stress_object_test`; the window is a few instructions wide. Fixed in d963217 as suggested: the reference, `parent_dir` and `name` are set before the entry is linked, under the directory lock. `object_extra_test` `insert_publishes_entry_after_parent_set` pauses the insert in that window and fails on the old order (it found `parent_dir == NULL`).

### I-1 [Info] Stale comment about the call that frees the mapping
- `src/kernel/nt/mm.c:1610-1613`: the comment says `vm_munmap` is called, but that line is commented out. The active call is `kshim_munmap` at `:1614`. Same behaviour, but the comment misleads the next reader.

### T-1 [Medium] Timer handles are rejected by the single-object wait (fixed e2cf675)
- `src/kernel/nt/nt_obj.c` (`nt_svc_NtWaitForSingleObject`, both branches) and `src/kernel/nt/sync.c` (`nt_svc_NtWaitForMultipleObjects`, kernel build) accept only event, mutant and semaphore types, so a timer returns `STATUS_OBJECT_TYPE_MISMATCH`. The wait engine already signals and consumes timers.
- Fix: accept `nt_timer_type`. Synchronization timers reset when a wait consumes them, and notification timers stay set. `sync_extra_test` covers the single-object path. The multiple-object path is compiled only in the kernel build and is reviewed, not run here.

### T-2 [Medium] Userspace timer fires after one second when the due time is two seconds or more, and cancel polls (fixed 3cbaa53)
- `src/kernel/nt/sync.c`, `timer_thread_func` (userspace branch): each sleep is capped at one second, and the chunk is treated as the whole sleep, so a due time of two seconds or more signals early. `nt_cancel_timer` slept `usleep(2000)` and returned before the thread had exited.
- Fix: the thread waits on `timer_cond` until the absolute NT expiry and re-reads the clock on each wake. Cancel wakes the thread and waits for it to exit. Periodic timers advance by the period without a burst. The kernel timer uses `mod_timer` with the full due time, so it is not affected.

### T-3 [High] NtSetTimer runs a user APC routine on the timer thread or in softirq context (fixed 8e7fd33)
- `src/kernel/nt/sync.c`: the userspace timer thread called `t->apc_routine` when no APC thread was set, and `timer_callback` called it in softirq context. `nt_svc_NtSetTimer` passed the user pointer through unchecked. No APC queue exists (`nt_thread_queue_apc` sets one flag), so the routine cannot be delivered to its thread.
- Fix: a non-NULL `ApcRoutine` returns `STATUS_NOT_IMPLEMENTED` before any state changes, in the wrapper and in both `nt_set_timer` functions. The timer callbacks never call a routine.

### K-1 [Medium] NtReleaseKeyedEvent with no waiter succeeds and drops the release (fixed ed89f0e for userspace; kernel open)
- `src/kernel/nt/sync.c`, `nt_release_keyed_event`: with no waiter for the key it returned `STATUS_SUCCESS`. NT blocks the releaser until a waiter takes the release, or returns `STATUS_TIMEOUT` when the timeout ends (at once for zero).
- Fix (userspace shim): a queued releaser list. A waiter that arrives first takes the release. Matching and waking happen under `g_keyed_lock`. The kernel keyed-event code (`nt_wait_for_keyed_event` and `nt_release_keyed_event` in the kernel branch, `:1756` and later) is a separate implementation with the same defect. It was not changed, because it cannot be built or run on this host.

### T-4 [Low] NtWaitForMultipleObjects and the nt_internal.h prototypes take unsigned long arguments (not changed)
- `src/kernel/nt/sync.c` (`nt_svc_NtWaitForMultipleObjects`) and `src/kernel/nt/nt_internal.h:73-74` take twelve `unsigned long` arguments. On an LLP64 host (Win64) a pointer argument is truncated to 32 bits. The kernel build is LP64, so the defect is latent there. The `nt_internal.h` prototype of `nt_svc_NtWaitForSingleObject` does not match its definition in `nt_obj.c` (`HANDLE`, `BOOLEAN`, `PLARGE_INTEGER`). Nothing in the host tests calls either through `nt_internal.h`.

## Timer and keyed-event fixes (branch haiku/timerfix)

The timer and keyed-event services were not on this branch. They were on `haiku/svcsync` and were picked up here first: `b891048`, `10ddb43`, `8ea698f`, `0258238`, `b9661c2`, `f8a0644`, and the build fix `5c5b570`. They are now `c4e9a6f`, `e388c15`, `67b98b2`, `a559148`, `16b16ed`, `8ddfe67`, and `3c73654`. Then, one commit per gap:

- `e2cf675` T-1, timers are waitable (`sync_extra_test`: `wait_single_sync_timer_auto_reset`, `wait_single_notification_timer_stays_set`).
- `3cbaa53` T-2, due time and period, cancel without polling (`set_timer_long_due_time_honoured`; guards `set_timer_period_repeats`, `cancel_timer_is_prompt`).
- `8e7fd33` T-3, APC refusal (`set_timer_rejects_user_apc`).
- `ed89f0e` K-1, keyed release blocks (`keyed_release_without_waiter_times_out`, `keyed_release_blocks_until_waiter`).

The new tests were built against the sources before the fixes (`3c73654`, with `sync.c`, `sync.h` and `nt_obj.c` taken from that commit). Each of the four gaps failed for its stated reason (`0xc0000024` for T-1, a signal after the first 1 s chunk, before the 2 s check, for T-2, `STATUS_SUCCESS` for T-3, `STATUS_SUCCESS` for K-1). With the fixes, all 27 `sync_extra_test` cases pass.

Not done or not verified on this host:
- The kernel keyed-event release (K-1) and the kernel timer APC removal (T-3) are compiled only in the kernel build, which this host does not run. Only the APC refusal and the callback change were made there.
- `tools/check_syscall_tables.py`, `tools/update_doc_counts.py --write`, and the Python unit suite were not run. `python` on this machine is the Windows Store alias stub. The counts in the Summary table and `docs/status/SERVICES.md` were not regenerated.
- `stress_sync_test` was run once, seed 1 with 8 threads and `KSHIM_POISON=1`: 0 violations. Seeds 2 and 3 and the other stress programs were not run.
- `mm_test.exe` returned 126 (Windows Application Control) on the first run and on one retry. It links `mm` and `kshim` only, so this change does not affect it. The other 11 `make check` programs pass.

## Fix status (branch haiku/ntfix)

Fixed in this branch: H-1 (68fe25b), R-1 (0254b09; all eight user-output buffers in registry.c), S-1 (6f58d04), O-1 (30b200a), IO-1 read half (1d160e8). Already correct here, no change: D-1, M-2.

Fixed on branch haiku/adr: IO-1 write half (80b6fea; any-of lookup, `ntio_handle_lookup_any_of`) and IOCP-2 (0b7921b; `NTIO_IOCP_MAX_QUEUED` = 65536). Neither commit has been built in the kernel tree or run.

Partly fixed: H-2 in d7adc94 (FIFO quarantine of closed slots, record 0003 option 1). The generation counter (option 2) is not implemented, so a stale handle can still alias a reused slot.

Not fixed. Each has a design record in `docs/decisions/` (index: [docs/decisions/INDEX.md](../decisions/INDEX.md)):

- P-1: NtOpenProcess and NtOpenThread have no access or owner check and use the registry result unreferenced. Needs a policy for who may open which process. Record 0002.
- M-3: section objects are never freed. Needs an object type with a delete callback; depends on M-1. Record 0004.
- M-4: VAD rollback on failed commit or decommit. Recommended: validate, preallocate, then mutate. Record 0005.
- IO-2: file handle table is global. Interim owner field, long-term per-process table. Record 0007 (owner rule for duplicates needs a decision).
- PT-1: TEB address returned to callers is a kernel heap address. Needs the TEB user mapping read first. Record 0008.
- IOCP-1: concurrency count is never released. Needs a decision on when a thread counts as finished. Record 0009.

Low findings are not addressed in this pass (M-5 and M-6 are still present, as are the others).

## Case-insensitive lookup component overflow (branch haiku/resolveci)

- IO-12 (new in this pass, not in the list above): `ntio_resolve_ci` in `src/kernel/nt/io.c` copied each path component into a `char component[NAME_MAX]` with no bound. The NT-layer check (`kshim_ntpath_to_linux`) allows 255 characters, so a 255-character component wrote its NUL one byte past the buffer. Fixed in 949cf00, which rejected components of 255 or more characters with `STATUS_OBJECT_NAME_INVALID`. That over-rejected legal NTFS names, so it is revised in 25bc012: `component` and the case-insensitive `fold` buffer are now `NAME_MAX + 1` (room for the NUL), and only components longer than `NAME_MAX` (256 or more) are rejected before the copy. The function still returns `NTSTATUS`. Tests: 4a5e4c3 (first, mis-targeted), 667d788 (reproduces the overflow), and 2393db8 (a 255-character lookup must resolve and 256 or 300 must be rejected; 2393db8 fails on the 949cf00 code with `0xC0000033` for 255 and passes after 25bc012). Verified on the host (`tests/kernel-host`, io_test 14/14). Stack impact is two bytes per frame, negligible. This is the same defect as IO-12 in `arch-x86-nt.md`.
- On this host `PATH_MAX` is 260. A 255-character name fits the converter's buffer only under a root of at most three characters, so the 255-character create case is skipped when the path does not fit. The lookup case uses an empty root and does a `stat` only.
- No other fixed-size buffer in `src/kernel/nt` is filled from a user-controlled length without a bound (`kbuf[4096]` in the console write, `registry.c` `vname[256]` capped at 200, `rel`/`parent`/`newpath` `PATH_MAX` buffers bounded by `snprintf` or `sizeof`).

## Medium and Low pass (branch haiku/lows-nt)

Each open Medium and Low finding was re-read against the code on this branch. Status values: fixed (commit named by the finding ID), no change, stale, needs owner.

- H-3: fixed. NtDuplicateObject undoes the duplicate and returns the source's close status when DUPLICATE_CLOSE_SOURCE is refused. Test: object_test `duplicate_close_source_protected`. Residual: an undo close on a duplicate created with OBJ_PROTECT_FROM_CLOSE also fails, which leaves that handle in place.
- H-4: fixed. `ht_alloc_index` returns the entry it claimed, so `ObCreateHandle` no longer re-reads the page pointers after releasing the table lock. No failing test: the defect is a race. Covered by the existing object_test run.
- H-5: fixed by documentation. The okay_to_close contract in `object.h` now says the table spinlock is held and lists the calls an implementation must not make. No implementation exists in the tree, so no code change.
- O-3: fixed. Symlink resolution carries a depth counter (`OB_MAX_SYMLINK_DEPTH` 32) and returns `STATUS_NAME_TOO_LONG` past it. Test: object_test `symlink_self_loop` (crashed before the fix).
- O-4: fixed. `nt_dup_unicodestring` rejects lengths above 0xFFFC with `STATUS_NAME_TOO_LONG` before the terminator is added. No test: no public route reaches it with such a length; syntax check only.
- R-2: fixed. `dup_us` in `registry.c` has the same USHORT wrap and gets the same guard. Latent; syntax check only.
- O-5: fixed. `ObMakeTemporaryObject` clears the permanent flags before removing the object from its directory, so nothing is written after a possible free. Latent (the caller holds a reference); object_test passes, no failing test.
- O-6: fixed. `ObCreateProcessObject` and `ObCreateThreadObject` check the registry insert and destroy the new object on failure. No test: the failure needs an allocation fault, and kshim has no injection hook; syntax check only. Second registration fixed in c4bffb1 (test ee49ebe): `ObRegisterThread` is idempotent in both branches, and the redundant calls in `nt_current_ethread` (object.c, kernel branch) and `NtCreateThreadEx` (proc_thread.c, kernel branch) are removed. Test: object_test `thread_registry_one_node` (failed before with 2 nodes and 1 left after destroy; passes now).
- M-5: fixed. `NtAllocateVirtualMemory` rejects a RegionSize above `SIZE_MAX - granularity` with `STATUS_INVALID_PARAMETER`, so the round-up cannot wrap to a zero-length VAD. Test: mm_test `test_alloc_size_wrap` (failed before with `STATUS_NO_MEMORY` or success).
- M-7: fixed. `NtCreateSection` returns `STATUS_NOT_IMPLEMENTED` for a non-NULL FileHandle, as the finding allows, instead of a zeroed anonymous section. The `is_file` branch is now unreachable and is left in place for the owner who adds file backing. Test: mm_test `test_file_section_unsupported` (failed before: status success).
- M-6: fixed. `NtFreeVirtualMemory` and `NtProtectVirtualMemory` reject a range whose end wraps before computing `end`. No failing test: a wrapping range needs a committed VAD near the top of the address space, which the host shim cannot provide without mapping a huge range. mm_test passes; syntax check.
- SYNC-1: fixed in the kernel branch of `sync.c` only. `nt_wait_on_address` copies `addr` and `cmp` with `copy_from_user` before comparing. The `#ifndef __KERNEL__` copy at `sync.c:1088` is unchanged. No test: the kernel branch cannot be compiled on the host, and the host build uses plain memcpy. sync_test covers the userspace copy only.
- SEC-2: fixed. The DACL branch of `sd_from_self_relative` checks that the 8-byte ACL header is inside the buffer before it reads `AclSize`, as the SACL branch already did. The later size check already rejected the same inputs, so no observable behaviour changes; the defect was an out-of-bounds header read. No failing test; security_test passes (41/41, 609/609 negative cases).
- D-2: fixed. The per-call trace for the six SSNs in `dispatch.c` is `pr_debug` instead of `pr_info`, so it is off in normal builds. `dispatch.c` already fails the syntax check (pre-existing errors, same count before and after).
- R-3: fixed. `build_full_path` adds the separator when the root path is empty instead of reading `buf[-1]`, so the result starts with `\`. Latent; syntax check only.

- H-2 (Medium): needs owner. The generation counter is the ABI change that decision 0003 left open; the free-list restore is not a local change.

- M-2 (Medium): no change. `NtMapViewOfSection` checks `offset > sec->size` before any arithmetic, and checks `actual_size > sec->size - offset` after it. Both guards are present on this branch.

- O-2: needs owner. The exclusive check and the handle-count increment need one lock. The obvious choice, the parent directory lock, would nest under the handle table lock, and the lock order has not been decided. Design record 0033 (blocked, option 3 recommended: atomic claim, no new lock order).

- SYNC-2: needs owner. `timeout_to_jiffies` cannot tell a kernel copy from a kernel address supplied by a user caller. The fix is a copy in each service wrapper that passes a timeout, and the provenance of each caller was only partly traced. Design record 0034 (blocked, recommended: copy in each wrapper).

- SEC-1: needs owner. The fix either stores capacity in the ACL allocation (a layout change) or returns errors from the registration call sites (six sites in `security.c`). The choice is a design decision. Design record 0035 (blocked, recommended: return an error from registration; grep finds five call sites, not six).

## Stress pass: token, section, namespace, SD, registry, timers (branch haiku/stress2)

Six programs were added to `make stress`, each run with 16 workers and seeds 1 to 3 (`KSHIM_POISON=1`). Commits: tests `f23a857`, `29236dd`, `56a4481`, `7ac3e45`, `62b7199`, `097bc60`, wiring `bbc99e3`; fixes `6707c84` (SEC-3) and `2bac4c0` (MM-1).

### SEC-3 [Medium] ACL capacity table updated without a lock (fixed 6707c84)

- Found by `stress_token_test` (`f23a857`). Before the fix, seeds 1 to 3 gave 90 to 464 violations each: `NtOpenProcessToken` and `NtDuplicateToken` returned `STATUS_BUFFER_TOO_SMALL` under load.
- Minimised: eight threads that open and close the process token, with nothing else running, fail 150 to 320 times in 24000 opens. One thread never fails.
- Cause: `acl_capacity_register`, `acl_capacity_unregister` and `acl_capacity_lookup` (`security.c`) read and wrote `g_acl_caps` and `g_acl_cap_used` with no lock. A lost registration makes the lookup fall back to `AclSize`, and `acl_has_space` then rejects the ACE.
- Fix: one spinlock around all three functions (kernel: `DEFINE_SPINLOCK`; userspace: a pthread mutex). After the fix, `stress_token_test` passes seeds 1 to 3 at 16 workers with 0 violations, and the eight-thread repro is clean.

### MM-1 [Medium] NtUnmapViewOfSection releases the VAD lock before the host unmap (userspace fixed 2bac4c0; kernel open)

- Found by `stress_section_test` (`29236dd`). Two threads that loop on map and unmap fault on the first write to a view. Seed 1 at 8 workers and 1024 ops fails, and so does a two-thread repro (3 of 3 runs before the fix, 0 of 3 after). A live-range check in the repro found no overlapping returned ranges before the fault, so the freed range was reused and then unmapped under the other view.
- Cause: `NtUnmapViewOfSection` removes the VAD under `ntmm_lock`, releases the lock, then calls `kshim_munmap`. In that window another `NtMapViewOfSection` can take the freed range, and the late unmap removes its view.
- Fix (userspace branch only, `#ifndef __KERNEL__`): `kshim_munmap` runs before `ntmm_unlock`. Section stress seeds 1 to 3 at 16 workers pass after the fix.
- Open: the kernel branch keeps the old order. `vm_munmap` takes `mmap_lock`, and moving it under the VAD lock changes the lock order. That is a design decision.
- Not kept: a shim self-test for the granule case. It could not be built, because Windows rejects a `MEM_RESERVE` at an address that is not 64K aligned, so a shared allocation base cannot be made from a fixed mapping.

### REG-1 [Medium] NtSetValueKey persists a value after releasing the key lock (fixed fe55b39)

- `stress_registry_test` (`7ac3e45`) faults under `KSHIM_POISON=1` at every seed run in `make stress` (seeds 1 to 3). `addr2line` puts the fault at `registry.c:294`, the read of `v->name.Buffer` in `persist_write_value`, with a bad read address of -1.
- Cause: both branches of `NtSetValueKey` call `persist_write_value(k, target)` or `(k, nv)` after `write_unlock(&k->lock)`. `NtDeleteValueKey` removes the value under the lock and frees it, so a set that is still persisting reads freed memory. Without poison the freed memory usually still holds the old values, which is why unpoisoned runs pass.
- **Status:** fixed in fe55b39, with the regression test in f6535bc. Each key has a sleepable `persist_lock` (`struct mutex`). `NtSetValueKey` and `NtDeleteValueKey` hold it across the in-memory change and the disk write, so a value cannot be freed while a set persists it. Disk writes for one value now land in the order they were applied. Lock order: `persist_lock`, then the key `rwlock_t`. Readers and the snapshot take only the `rwlock_t`, as before. The host shim maps `struct mutex` to a pthread mutex (`kshim.h`). The kernel build was not compiled here.
- The regression test is `registry_ops_test` case 7 (`test_delete_waits_for_persisting_set`). It holds a set in the persist window through the userspace-only seam `ntreg_test_persist_hook` and checks that a delete of the same value waits. It failed before the fix and passes after. `stress_registry_test` at 16 threads with `KSHIM_POISON=1` passes seeds 1 to 3 with no violations (before the fix it faulted at every seed).
- Follow-up, open, not tested: `NtDeleteKey` removes the key's files without taking the value `persist_lock`. A value set racing with the key delete can recreate a file under the deleted key's directory. The stress test checks in-memory state, not disk state, so it does not see this.

### OB-1 [Medium] ObpRemoveFromDirectory reads the parent directory without a reference (fixed c2b32a1)

- `stress_namespace_test` under `KSHIM_POISON=1` faulted in `libwinpthread-1.dll`, reading address -1 inside a lock. Before the fix, 5 of 10 poisoned runs at 8 threads and 6 of 6 at 16 threads faulted (seed 1). Without poison, no fault.
- Cause, confirmed: the stress "delete a name" step calls `ObReferenceObjectByName` on a link name, which follows the link and returns the directory. `ObMakeTemporaryObject` then removed the directory, and its last reference freed it while another worker's `ObpRemoveFromDirectory` had read the directory pointer from a child and was about to lock it.
- Fix: `ob_ns_lock` covers every `parent_dir` and every directory entry list. Lock order is `ob_ns_lock`, then a directory's lock. `directory_delete` and the root cleanup detach entries under the lock and release them with no lock held. Regression test `remove_after_parent_directory_freed` (object_extra_test.c, poison on) faults before the fix and passes after. Under poison, `stress_namespace_test` passes 14 of 14 runs at 8 and 16 threads, and seeds 1 to 3 at 16 threads.

### OB-2 [Low] Symbolic links are followed only as the last path component (fixed db4e61d)

- `object.c` resolved a symlink only when it was the final component. A link in a middle component returned `STATUS_OBJECT_PATH_NOT_FOUND`, so a path through a link never opened. `stress_namespace_test` counted 0 opens through a link on every run.
- Fix: a middle link is resolved with the final-link depth limit (32) and the walk continues from its target. A cycle returns `STATUS_NAME_TOO_LONG`, as the final-link loop does. A missing target is `STATUS_OBJECT_PATH_NOT_FOUND`. A missing final name is `STATUS_OBJECT_NAME_NOT_FOUND`. Tests `middle_symlink_is_followed` and `middle_symlink_cycle_hits_depth_limit` failed before the fix. `stress_namespace_test` now records 1000 to 1400 opens through a link per run.

### SYNC-3 [Medium] stress_sync_test fails about half its runs at seed 2 (open, pre-existing, not minimised)

- With 16 workers, the failing runs report `owned wait` and `close` returning `STATUS_INVALID_HANDLE` on handles the worker still holds (1 to 6 violations per run). On the current tree 5 of 7 runs failed at seed 2, with and without poison. The base commit `45a70b4` failed 1 of 4 runs with the same messages, so this predates this work. Changes `6707c84` and `2bac4c0` do not touch the sync path.
- Not minimised. A handle slot being freed twice, or reused during the race round, is a guess and is not confirmed.

### Confirmed, no change

- SEC-1 (ACL table overflow) reproduces as stated: with 300 live ACLs, `acl_add_allowed_ace` on the 300th returns `STATUS_BUFFER_TOO_SMALL`, and the first ACL still grows. Design record 0035 still applies.
- The host shim maps file-backed section views as zero-filled anonymous memory (`tests/kernel-host/shim/mman_win.c`). `stress_section_test` therefore checks that file views map and read, not their contents.
- Passing with 16 workers and seeds 1 to 3: `stress_token_test` (after the fix), `stress_section_test` (after the fix), `stress_sd_test` (no torn or unknown descriptors), `stress_timer_test`. `stress_namespace_test` passed seed 2 in `make stress` and 3 of 3 unpoisoned runs at 16 workers (under poison it failed before c2b32a1, OB-1).
- `make check-quick` passes after both fixes. `tools/update_doc_counts.py` and the Python unit suite were not run: this host has no working Python, so the count blocks in `docs/review/INDEX.md` are not regenerated.

## Not verified / out of scope

- Nothing was compiled, linked, sanitized, fuzzed, or executed. Overflow and wrap claims come from the arithmetic at the cited lines.
- Read in full: `object.c`, `handle.c`, `dispatch.c` (except the header section).
- Read in part: `mm.c` (lines 802-1644, the allocation, free, protect, query, and section paths; the helper section at 1-800 was not read), `io.c` (handle table, path conversion, read, query and set-information copies; the create and open path at `:1228-1540` and the rename and disposition code at `:1080-1227` were not read), `proc_thread.c` (information-class handling and the kernel creation path around `:932-1200`; the shim branch `:83-931` was not read), `sync.c` (wait engine entry points and the address-wait code; the keyed-event and waiter lists were not read), `registry.c` (value and key write paths and the service wrappers), `security.c` (ACL and SID helpers; the token and access check code was not read), `iocp.c` (`:150-736`), `nt_obj.c` (file, event, and object query wrappers in part).
- Header files were spot-checked for the structures used above (`handle.h`, `object.h`, `sync.h`, `kshim.h`). `mm.h`, `io.h`, `iocp.h`, `proc_thread.h`, `registry.h`, `security.h` were not read in full.
- Finding IDs are local to this report. Cross-references to `arch-x86-nt.md` refer to the same defect in the other review tree.
