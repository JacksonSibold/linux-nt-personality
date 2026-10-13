# Module: object (Object Manager)

Sources: `src/kernel/nt/object.c`, `src/kernel/nt/object.h`
Related: `handle.md` (per-process handle table), `nt_obj.c` (service wrappers for object-namespace, event, mutant, semaphore and object-information services; summarised in the module pages that use them), `security.md` (token object and descriptor services), `proc_thread.md` (process and thread objects), `mm.md` (section type), `registry.md` (key type).

## Purpose

Implements the NT object manager model: object headers with a reference count and a separate handle count, typed objects through `struct nt_object_type` callbacks, a `\` namespace built from directories and symbolic links, name lookup with ASCII case-insensitive matching, and a per-object security descriptor slot. Process and thread objects (`struct nt_eprocess`, `struct nt_ethread`) are declared here and created through this module.

## Public API (object.h)

Lifecycle and creation:

- `NTSTATUS ObInitObjectManager(void)` (`object.c:1551`) - creates the root and well-known directories and registers the built-in types.
- `void ObCleanupObjectManager(void)` - tears down the namespace.
- `NTSTATUS ObCreateObjectType(...)` - declared at `object.h:226`. **No definition exists** in `src/kernel/nt`.
- `NTSTATUS ObRegisterObjectType(struct nt_object_type *type)` (`object.c:1533`) - adds a type to the type list, rejecting a duplicate name.
- `NTSTATUS ObCreateObject(struct nt_object_type *type, POBJECT_ATTRIBUTES attrs, SIZE_T body_size, struct nt_object_header **out_hdr, void **out_body)` - allocates header and body as one block. It validates `Attributes` against `OBJ_VALID_ATTRIBUTES`. It does not consume `attrs->SecurityDescriptor` (see below).
- `NTSTATUS ObInsertObject(struct nt_object_header *obj, POBJECT_ATTRIBUTES attrs, ACCESS_MASK access, struct nt_eprocess *proc, PHANDLE out_handle)` - inserts a named object into the namespace and creates a handle in `proc`.
- `NTSTATUS ObInsertOrOpenNamedObject(...)` - create-or-open rules: type mismatch, `STATUS_OBJECT_NAME_COLLISION`, `OBJ_OPENIF` giving `STATUS_OBJECT_NAME_EXISTS` with a handle, `STATUS_OBJECT_PATH_NOT_FOUND` for a missing parent.
- `NTSTATUS ObMakeTemporaryObject` / `ObMakePermanentObject` - set or clear `is_permanent`. The privilege check (`SeCreatePermanentPrivilege`) is in `nt_obj.c`.
- `NTSTATUS ObCreateDirectoryObject(...)` and `ObCreateSymbolicLinkObject(...)` - build a directory or link object not yet in the namespace. The link copies its target.
- `NTSTATUS ObSetObjectSecurityDescriptor(struct nt_object_header *obj, const void *sd, ULONG length)` (`object.c:764` onward) - attaches a copy of a self-relative descriptor; NULL clears it. The replace runs under `ob_sd_lock`.
- `NTSTATUS ObCopyObjectSecurityDescriptor(struct nt_object_header *obj, void **out, ULONG *out_len)` (`object.c:~787`) - returns a heap copy (NULL when there is none). The caller frees it with `kfree`.

Reference counting and lookup:

- `ObReferenceObject` / `ObDereferenceObject` - adjust `ref_count`. The last drop runs `ops.delete_obj` and frees the block.
- `ObReferenceObjectByName(...)` and `ObOpenObjectByName(...)` - resolve a path, then take a reference or create a handle.
- `ObpIncrementHandleCount` (`object.c:811`) and `ObpDecrementHandleCount` (`object.c:821`) - maintain `handle_count`, called from `handle.c`.
- `ObpLookupDirectoryEntry(...)` - single-component lookup.

Namespace roots: `ObGetRootDirectory`, `ObGetDosDevicesDirectory`, `ObGetBaseNamedObjects`, `ObGetDeviceDirectory`, `ObGetKnownDllsDirectory`, `ObGetSessionsDirectory`. Accessors: `ObGetObjectName`, `ObGetObjectType`.

Process and thread objects: `ObCreateProcessObject`, `ObCreateThreadObject`, `ObDestroyProcessObject`, `ObDestroyThreadObject`, the `ObFind*ByHeader`/`ObFind*ByPid`/`ObFind*ByTid` lookups, `ObRegisterThread`/`ObUnregisterThread`, `ObForEachThreadOfProcess`, and the bulk thread controls `ObSuspendProcessThreads`, `ObResumeProcessThreads` and `ObTerminateProcessThreads`.

## Services with a definition in nt_obj.c

Each service has a typed definition in the userspace branch (before the `#else` at `nt_obj.c:1073`) and a raw-argument definition in the kernel branch (`nt_obj.c:1076` to `:1761`). Dispatch uses the raw form.

- Object-namespace: `NtCreateDirectoryObject` (typed `:900`, raw `:1463`), `NtOpenDirectoryObject` (`:1001`, `:1493`), `NtQueryDirectoryObject` (`:1020`, `:1524`), `NtCreateSymbolicLinkObject` (`:923`, `:1583`), `NtOpenSymbolicLinkObject` (`:982`, `:1634`), `NtQuerySymbolicLinkObject` (`:948`, `:1662`).
- Object information and lifetime: `NtQueryObject` (`:774`, `:1100`), `NtSetInformationObject` (`:869`, `:1076`), `NtMakeTemporaryObject` (`:886`, `:1429`), `NtMakePermanentObject` (`:1047`, `:1445`), `NtDuplicateObject` (typed `:566`, raw `:1355`), `NtClose` (typed `:1061`, raw `:1727`).
- `NtQueryDirectoryObject` lists `OBJECT_DIRECTORY_INFORMATION` records under the directory read lock. `Context` is the index of the next entry, and `RestartScan` starts again.
- Opening a link object returns the link. Querying it needs `SYMBOLIC_LINK_QUERY`.
- `NtMakePermanentObject` checks `SeCreatePermanentPrivilege` on the caller's effective token before the handle lookup.
- `NtQueryObject` class `ObjectHandleFlagInformation` (4) reads the handle flags with `ObGetHandleFlags` (`handle.c`). Class `ObjectTypesInformation` (3) is not handled and returns `STATUS_INVALID_INFO_CLASS`.

## Key data structures

- `struct nt_object_header` (`object.h`) - `type`, `ref_count` and `handle_count` (atomic64), `parent_dir`, `name` (UNICODE_STRING), `flags`, `body`, `type_link`, the flags `is_permanent`, `is_exclusive` and `exclusive_owned`, and `security_descriptor` (`object.h:170`) with `security_descriptor_length`. The descriptor is an owned, self-relative copy, or NULL. The body is allocated directly after the header.
- `struct nt_object_type` - `list`, `name`, `ops` (`delete_obj`, `close`, `parse`, `okay_to_close`) and `pool_tag`.
- Built-in types, all listed by `ObInitObjectManager` (`object.c:1557` to `:1562`): `nt_directory_type` (`:172`), `nt_symlink_type` (`:178`), `nt_process_type` (`:184`), `nt_thread_type` (`:190`), `nt_token_type` (`:205`, named `"Token"`), and `nt_test_type` (`:211`, a test type). Other modules register their own types with `ObRegisterObjectType`: `nt_key_type` (`registry.c:465`), the event, mutant, semaphore and file types (`nt_obj.c:126` to `:129`), and `nt_section_type` (`mm.c:1749`, named `"Section"`, registered on first use at `mm.c:1863`, `:1981` and `:2028`).
- Token object body: `struct nt_token_body` (`object.h:145`) holds one `ACCESS_TOKEN` reference. Its delete callback is `token_delete` (`object.c:196`). See `security.md`.
- Process and thread slots: `struct nt_eprocess` has `primary_token` (`object.h:363`), set at creation and released in `ObDestroyProcessObject`. `struct nt_ethread` has `impersonation_token` (`object.h:406`), NULL unless the thread impersonates.
- `struct nt_directory` - `rwlock_t lock` and an entry list of `struct nt_dir_entry`.
- `struct nt_symlink` - `target` UNICODE_STRING.
- `struct nt_eprocess` - header pointer, `handle_table`, `pid`, `name`, image path, `peb`, lock and exit wait, and the per-process information fields added for `NtSetInformationProcess`.

## Invariants and locking (as implemented)

- Callback contract (`object.h`): `delete_obj` tears down type-specific state but does not free the header. The object manager frees the header-plus-body block afterwards.
- Header and body share one allocation. `nt_body_to_header` depends on that layout.
- Directory bodies are protected by `rwlock_t`. Other object-manager state uses `spinlock_t` in the kernel build and pthread primitives in the shim.
- Names are duplicated into owned buffers (`nt_dup_unicodestring`). Lengths must be a multiple of `sizeof(WCHAR)`, otherwise `STATUS_OBJECT_NAME_INVALID`. Buffers are NUL-terminated.
- Name comparison is ASCII-only: `nt_toupper` maps only `a` to `z`.
- A descriptor is replaced under `ob_sd_lock`, a file-scope spinlock. Readers copy it with `ObCopyObjectSecurityDescriptor`, so a replace never frees a buffer in use.
- `ObCreateObject` clears `security_descriptor` and `is_permanent`. It does not read `OBJECT_ATTRIBUTES.SecurityDescriptor`, because the object never borrows a caller buffer of unknown length. A descriptor is attached only with `ObSetObjectSecurityDescriptor`, so a creation-time descriptor is not applied.

## Known gaps (evident in code)

- **Permanence has no effect.** `is_permanent` is set when an object with `OBJ_PERMANENT` is linked into a directory (`ObpInsertDirectoryEntry`, `object.c:304`) and by `ObMakePermanentObject` (`object.c:916`). It is never read. The only reader is `object_extra_test.c`. A named object is not removed from the namespace when its last handle closes. Changing that would change the lifetime of every named object. (Owner decision, see `docs/OWNER-DECISIONS.md`.)
- `ObCreateObjectType` is declared but not defined. Only `ObRegisterObjectType` with a static type works.
- `NtQueryDirectoryObject` uses an index as its context. If entries are inserted or removed between calls, a listing can skip or repeat entries.
- `NtQueryObject` class `ObjectTypesInformation` is `STATUS_INVALID_INFO_CLASS`.
- `NtCompareObjects` is not in `nt_service_list.h`, so it has no service entry.
- `OBJECT_ATTRIBUTES.RootDirectory` is not referenced in `object.c`, so a relative name fails (audit pass 4 item 2, open). This is a missing feature, not an unsafe path.
- A symbolic link in a middle path component is not followed: `object.c:434` returns `STATUS_OBJECT_PATH_NOT_FOUND` when a component is not a directory. A link is resolved only as the final component (`ObpResolveSymlink`, `object.c:355`).
- `ObpRemoveFromDirectory` (`object.c:310`) reads `parent_dir` before it takes the directory lock (audit pass 4 item 15). If `directory_delete` could free the directory between those two steps, the lock is taken on freed memory. Not resolved; see `docs/OWNER-DECISIONS.md`.
- The `ops.parse` hook is consulted during lookup. Only `registry.c` sets it (`registry_parse`, `registry.c:90`, wired at `registry.c:159`), so no generic parse routine exists.
- Case-insensitive comparison is ASCII-only; `object.c` says full Unicode case folding is not implemented.
- The kernel branch of `NtDuplicateObject` (`nt_obj.c:1365`) rejects a NULL `TargetHandle` with `STATUS_ACCESS_VIOLATION`. The userspace branch accepts NULL. The kernel branch was not compiled on this host.
- The kernel branches of the directory, link and permanence services in `nt_obj.c` were written against kernel helpers but not compiled. The userspace branches are covered by `object_extra_test.c`.
- Object security descriptors are enforced only by the security services in `security.c` (`NtQuerySecurityObject`, `NtSetSecurityObject`, `NtAccessCheck`). Object creation and open paths here do not check them. `NtSetSecurityObject` refuses an object that has no descriptor (`STATUS_ACCESS_DENIED`), so the owner cannot attach one (audit pass 4 item 4, open).
