# Module: object (Object Manager)

Sources: `src/kernel/nt/object.c`, `src/kernel/nt/object.h`
Related: `handle.c` (per-process handle table), `nt_obj.c` (NT service wrappers, not covered here), `proc_thread.c`.

## Purpose

Implements the NT object manager model: object headers with a reference count and a separate handle count, typed objects via `struct nt_object_type` callbacks, a global `\` namespace built from directories and symbolic links, and name lookup with optional case-insensitive matching. Process and thread objects (`struct nt_eprocess`, `struct nt_ethread`) are declared here and created through this module.

## Public API (object.h)

Object manager lifecycle and creation:

- `NTSTATUS ObInitObjectManager(void)` - create the root and well-known directories and register the built-in types.
- `void ObCleanupObjectManager(void)` - tear down the namespace.
- `NTSTATUS ObCreateObjectType(const char *name, const struct nt_object_type_ops *ops, struct nt_object_type **out_type)` - declared only; **no definition exists in the tree** (see Known gaps).
- `NTSTATUS ObRegisterObjectType(struct nt_object_type *type)` - add a type to the type list (`object.c`).
- `NTSTATUS ObCreateObject(struct nt_object_type *type, POBJECT_ATTRIBUTES attrs, SIZE_T body_size, struct nt_object_header **out_hdr, void **out_body)` - allocate header and body as one contiguous block.
- `NTSTATUS ObInsertObject(struct nt_object_header *obj, POBJECT_ATTRIBUTES attrs, ACCESS_MASK access, struct nt_eprocess *proc, PHANDLE out_handle)` - insert into the namespace (if named) and create a handle in `proc`.
- `NTSTATUS ObMakeTemporaryObject(struct nt_object_header *obj)` - drop the permanent flag so the name is removed when unreferenced.
- `NTSTATUS ObMakePermanentObject(struct nt_object_header *obj)` - set the permanent flag. The privilege check (`SeCreatePermanentPrivilege`) is in `nt_obj.c`, not here.
- `NTSTATUS ObCreateDirectoryObject(POBJECT_ATTRIBUTES oa, struct nt_object_header **out_hdr)` and `NTSTATUS ObCreateSymbolicLinkObject(POBJECT_ATTRIBUTES oa, PCUNICODE_STRING target, struct nt_object_header **out_hdr)` - build an unnamed-or-named directory or link object, not yet in the namespace. The link copies its target.
- `NTSTATUS ObInsertOrOpenNamedObject(struct nt_object_header *obj, POBJECT_ATTRIBUTES oa, ACCESS_MASK access, struct nt_eprocess *proc, PHANDLE out_handle)` - insert with NT create-or-open rules (type mismatch, `STATUS_OBJECT_NAME_COLLISION`, `OBJ_OPENIF` giving `STATUS_OBJECT_NAME_EXISTS` with a handle, `STATUS_OBJECT_PATH_NOT_FOUND` for a missing parent). The lookup does not follow a link at the last component.
- `NTSTATUS ObSetObjectSecurityDescriptor(struct nt_object_header *obj, const void *sd, ULONG length)` - attach a copy of a self-relative descriptor (NULL clears). The object owns the copy and frees it on the final dereference.
- `NTSTATUS ObCopyObjectSecurityDescriptor(struct nt_object_header *obj, void **out, ULONG *out_len)` - return a heap copy of the descriptor (NULL when none); the caller frees it with `kfree`.

Reference counting and lookup:

- `void ObReferenceObject(struct nt_object_header *obj)` / `void ObDereferenceObject(struct nt_object_header *obj)` - adjust `ref_count`; the last drop runs `delete_obj` and frees the block.
- `NTSTATUS ObReferenceObjectByName(PUNICODE_STRING path, struct nt_object_type *type, KPROCESSOR_MODE mode, ACCESS_MASK access, struct nt_object_header **out)` - resolve a path and take a reference.
- `NTSTATUS ObOpenObjectByName(POBJECT_ATTRIBUTES attrs, struct nt_object_type *type, KPROCESSOR_MODE mode, ACCESS_MASK access, struct nt_eprocess *proc, PHANDLE out_handle)` - resolve a path and create a handle.
- `NTSTATUS ObpIncrementHandleCount(struct nt_object_header *obj)` / `void ObpDecrementHandleCount(struct nt_object_header *obj, HANDLE handle)` - maintain `handle_count` (called from `handle.c`).
- `struct nt_object_header *ObpLookupDirectoryEntry(struct nt_directory *dir, const UNICODE_STRING *comp)` - single-component lookup, for `handle.c` use.

Namespace services (`nt_obj.c`, wrappers over the functions above):

- `NtCreateDirectoryObject`, `NtOpenDirectoryObject`, `NtQueryDirectoryObject`. The query lists `OBJECT_DIRECTORY_INFORMATION` records (name and type name) under the directory read lock, with a zeroed terminator. `Context` is the index of the next entry, and `RestartScan` starts again. The kernel branch stages the listing through a kernel buffer of at most 64 KiB.
- `NtCreateSymbolicLinkObject`, `NtOpenSymbolicLinkObject`, `NtQuerySymbolicLinkObject`. Opening a link object returns the link (`ObOpenObjectByName` sets `OBJ_OPENLINK` for the link type). Querying needs `SYMBOLIC_LINK_QUERY`.
- `NtMakePermanentObject` checks `SeCreatePermanentPrivilege` on the effective token (`se_caller_has_privilege` in `security.c`) before the handle lookup.
- `NtQueryObject` class `ObjectHandleFlagInformation` reads the per-handle flags with `ObGetHandleFlags` (`handle.c`).
- Generic rights for the directory and link requests are mapped in `nt_obj.c` (`nt_map_directory_access`, `nt_map_symlink_access`).

Namespace roots:

- `ObGetRootDirectory`, `ObGetDosDevicesDirectory`, `ObGetBaseNamedObjects`, `ObGetDeviceDirectory`, `ObGetKnownDllsDirectory`, `ObGetSessionsDirectory` - return the header of the corresponding directory.
- `const UNICODE_STRING *ObGetObjectName(struct nt_object_header *obj)` and `struct nt_object_type *ObGetObjectType(struct nt_object_header *obj)` - accessors.

Process and thread objects:

- `ObCreateProcessObject`, `ObCreateThreadObject` - allocate the process or thread object.
- `ObDestroyProcessObject`, `ObDestroyThreadObject` - destroy them.
- `ObFindProcessByHeader`, `ObFindProcessByPid`, `ObFindThreadByHeader`, `ObFindThreadByTid` - lookups.
- `ObRegisterThread`, `ObUnregisterThread` - add or remove a thread from the registry.
- `ObForEachThreadOfProcess(proc, fn, ctx)` - iterate threads of a process.
- `ObSuspendProcessThreads`, `ObResumeProcessThreads`, `ObTerminateProcessThreads(proc, status)` - bulk thread control.

Inline helpers in the header: `nt_object_body`, `nt_body_to_header` (body pointer to header by fixed offset), `nt_object_is_directory`.

## Key data structures

- `struct nt_object_header` - `type`, `ref_count` and `handle_count` (atomic64), `parent_dir`, `name` (UNICODE_STRING), `security_descriptor`, `flags`, `body`, `type_link`, and the flags `is_permanent`, `is_exclusive`, `exclusive_owned`. The body is allocated directly after the header.
- `struct nt_object_type` - `list`, `name`, `ops` (`delete_obj`, `close`, `parse`, `okay_to_close`), `pool_tag`. Built-in types, registered in the list by `object.c:1336-1340`: `nt_directory_type`, `nt_symlink_type`, `nt_process_type`, `nt_thread_type` and `nt_token_type` (`object.c:202`, named `"Token"`). Other modules register their own types with `ObRegisterObjectType`: `nt_key_type` (`registry.c:465`), event, mutant, semaphore and file types (`nt_obj.c:125-128`), and `nt_section_type` (named `"Section"`, `mm.c:1700`). The section type is registered on the first `NtCreateSection` call (`mm.c:1747`), not at init.
- Token object body: `struct nt_token_body` holds one `ACCESS_TOKEN` reference (`object.h:116`). Its delete callback is `token_delete` (`object.c:193`). See `security.md`.
- Process and thread slots: `struct nt_eprocess` has `primary_token` (`object.h:281`), set in `ObCreateProcessObject` (`object.c:1175`) and released in `ObDestroyProcessObject` (`object.c:1250`). `struct nt_ethread` has `impersonation_token` (`object.h:318`), NULL unless impersonating and released at `object.c:1273`.
- Security descriptor slot: `struct nt_object_header` has `security_descriptor` (`object.h:130`). `ObCreateObject` clears it (`object.c:514`) and sets it from the object attributes when present (`object.c:521`).
- `struct nt_object_header` - `type`, `ref_count` and `handle_count` (atomic64), `parent_dir`, `name` (UNICODE_STRING), `flags`, `body`, `type_link`, the flags `is_permanent`, `is_exclusive`, `exclusive_owned`, and last `security_descriptor` (owned, self-relative copy, NULL when none) with `security_descriptor_length`. The body is allocated directly after the header.
- `struct nt_object_type` - `list`, `name`, `ops` (`delete_obj`, `close`, `parse`, `okay_to_close`), `pool_tag`. Built-in types: `nt_directory_type`, `nt_symlink_type`, `nt_process_type`, `nt_thread_type`, `nt_test_type`. Other modules add `nt_key_type` (registry), event/mutant/semaphore/file types (`nt_obj.c`).
- `struct nt_directory` - `rwlock_t lock` and `entries` list of `struct nt_dir_entry` (name, object pointer, link).
- `struct nt_symlink` - `target` UNICODE_STRING.
- `struct nt_eprocess` - object header pointer, `handle_table`, `pid`, `name`, image path (narrow and wide), `peb`, lock and exit wait (spinlock in kernel, pthread mutex and cond in the shim), `ref_ext`.
- `struct nt_ethread` - header, owning process, `tid`, TEB, stack bounds, suspend state, and a lock and wait queues (kernel and shim variants).

## Invariants and locking (as implemented)

- Callback contract (object.h): `delete_obj` must tear down type-specific state but must not free the header; the object manager frees the header-plus-body block after it returns.
- Header and body share one allocation; `nt_body_to_header` relies on that layout.
- Directory bodies are protected by `rwlock_t`. `object.c` takes `write_lock`/`write_unlock` on them at several sites. `object.c` also uses `spin_lock`/`spin_unlock` at about 15 sites; I did not map each one to a field.
- Names are duplicated into owned buffers (`nt_dup_unicodestring`). Lengths must be a multiple of `sizeof(WCHAR)`, otherwise `STATUS_OBJECT_NAME_INVALID`. Buffers are NUL-terminated.
- Name comparison is ASCII-only: `nt_toupper` maps only `a`-`z`.
- A descriptor is replaced under `ob_sd_lock`, a file-scope lock in `object.c`. Readers copy it with `ObCopyObjectSecurityDescriptor`, so a replace never frees a buffer in use.
- Each process and thread object guards its state with a lock: a `spinlock_t` in the kernel build, a `pthread_mutex_t` in the shim.

## Known gaps (evident in code)

- `OBJ_PERMANENT` has no lifetime effect yet. A named object that is not permanent is not removed from the namespace when its last handle closes (`ObpDecrementHandleCount` does nothing of the kind). `NtMakePermanentObject` only records the flag. Removal on last close would change the lifetime of every named object, so it is a separate change.
- `NtQueryDirectoryObject` uses an index as its context. If entries are inserted or removed between calls, a listing can skip or repeat entries. NT keeps a cursor that is not index based.
- `NtQueryObject` class `ObjectTypesInformation` (3) is still `STATUS_INVALID_INFO_CLASS`.
- `NtCompareObjects` is not in `nt_service_list.h`, so it has no SSN entry and no handler.
- The kernel branch of `NtDuplicateObject` still rejects a NULL `TargetHandle` with `STATUS_ACCESS_VIOLATION`. The userspace branch accepts it. The kernel code was not built on this host.
- The kernel branches of the new services in `nt_obj.c` (directory, link, and `NtMakePermanentObject` and `NtMakeTemporaryObject` in kernel form) were written against the kernel helpers but not compiled, because there is no kernel build on this host. The userspace branches are covered by `object_extra_test`.
- `ObCreateObjectType` is declared in `object.h` but has no definition anywhere under `src/kernel/nt/`. Callers can only use `ObRegisterObjectType` with a statically defined type.
- Case-insensitive comparison is ASCII-only. `object.c:31` says full Unicode case folding is not implemented.
- A symbolic link in a middle path component is not followed. `object.c:427` returns `STATUS_OBJECT_PATH_NOT_FOUND` when the component is not a directory. A symlink is resolved only when it is the final component (`object.c:438-442`, via `ObpResolveSymlink` at `object.c:348`).
- The `ops.parse` hook is consulted during lookup (`object.c:404-410`). Only `registry.c` sets it in this tree (`registry_parse`, `registry.c:159`), so no generic parse routine is provided here.
- Object security descriptors are carried in the header (`object.h:130`), but this module does not enforce them. `se_access_check` is called only from inside `security.c` (the token-open path and an access-check helper behind `NtAccessCheck`). No call site exists in this module (see `security.md`).
- Sections and tokens are object types, so their handles go through the same table. The section services are not in the syscall table (see `mm.md`).
- A symbolic link in a middle path component is not followed. `object.c:426-429` returns `STATUS_OBJECT_PATH_NOT_FOUND` in that case. A symlink is resolved only when it is the final component (`object.c:400`, via `ObpResolveSymlink`).
- The `ops.parse` hook is consulted during lookup (`object.c:364`). Only `registry.c` sets it in this tree (`registry_parse`), so no generic parse routine is provided here.
- Object security descriptors are attached with `ObSetObjectSecurityDescriptor` and are enforced only by `NtQuerySecurityObject`, `NtSetSecurityObject` and the access-check services in `security.c` (see `security.md`). `ObCreateObject` does not consume `OBJECT_ATTRIBUTES.SecurityDescriptor`, because the object never borrows a caller buffer of unknown length. Creation-time descriptors are therefore not applied yet.
