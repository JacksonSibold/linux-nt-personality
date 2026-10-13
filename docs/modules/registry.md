# Module: registry (Registry personality)

Sources: `src/kernel/nt/registry.c`, `src/kernel/nt/registry.h`
Tests: `src/kernel/nt/registry_test.c` (run as `registry_test` in `tests/kernel-host`), `tests/kernel-host/registry_ops_test.c`
Related: `object.md` (keys are object-manager objects), `handle.md`, `security.md` (no access check is wired in, see gaps).

## Purpose

Implements the NT registry as key objects inside the object manager. Keys form a tree rooted at well-known hives (`Machine` and `User` are created in `ntreg_init`). Values are typed byte blobs. Persistence is written under the guest filesystem so the tree can be inspected with ordinary tools. The `registry.h` design note describes a per-key directory tree plus a monolithic snapshot file, with a planned regf-compatible format that is not implemented.

## Public API (registry.h)

Lifecycle:

- `NTSTATUS ntreg_init(void)` - create the registry root and hives. Every entry point calls it lazily.
- `void ntreg_cleanup(void)` - tear down.
- `extern struct nt_object_type nt_key_type` - the key object type, registered with `ObRegisterObjectType`.

Key operations (native-style signatures):

- `NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition)` - open or create a key. `Disposition` is `REG_CREATED_NEW_KEY` (1) or `REG_OPENED_EXISTING_KEY` (2). `REG_OPTION_CREATE_LINK` returns `STATUS_NOT_IMPLEMENTED` before any side effect (no key is created). Other `CreateOptions` bits are ignored.
- `NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes)` and `NtOpenKeyEx(... , ULONG OpenOptions)` - open an existing key. Missing keys return `STATUS_OBJECT_NAME_NOT_FOUND`. `OpenOptions` is ignored, so `REG_OPTION_OPEN_LINK` opens the key itself (there are no links to follow).
- `NtDeleteKey(HANDLE KeyHandle)` - delete a key. A key that still has subkeys returns `STATUS_ACCESS_DENIED`. A leaf is unlinked from its parent's subkey list and namespace, its object is made temporary, and its snapshot record disappears at the next flush. The handle stays valid until `NtClose`.
- `NtFlushKey(HANDLE KeyHandle)` - write the whole registry tree to the hive snapshot (see Persistence layout). The handle is only validated; the snapshot covers every key under `\Registry`, not just the key named.
- `NtEnumerateKey(HANDLE, ULONG Index, ULONG KeyInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - enumerate subkeys. Supports `KeyNodeInformation` as well as the basic classes.
- `NtQueryKey(HANDLE, ULONG KeyInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - query key metadata. Supports `KeyNodeInformation`. A short buffer sets `ResultLength` and returns `STATUS_BUFFER_TOO_SMALL`.

Value operations:

- `NtSetValueKey(HANDLE, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize)` - create or replace a value.
- `NtQueryValueKey(HANDLE, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - read one value. Supports the full and partial classes and their `...Align64` variants (`KeyValueFullInformationAlign64`, `KeyValuePartialInformationAlign64`, classes 3 and 4). The Align64 full class pads the name so the data starts at an 8-byte aligned `DataOffset`. A NULL output buffer or unknown class is rejected, and a short buffer still reports the required length in `ResultLength`.
- `NtEnumerateValueKey(HANDLE, ULONG Index, ULONG KeyValueInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - enumerate values, with the same classes as `NtQueryValueKey`.
- `NtDeleteValueKey(HANDLE, PUNICODE_STRING ValueName)` - delete a value.

Dispatch wrappers: `nt_svc_NtCreateKey`, `nt_svc_NtOpenKey`, `nt_svc_NtOpenKeyEx`, `nt_svc_NtDeleteKey`, `nt_svc_NtFlushKey`, `nt_svc_NtEnumerateKey`, `nt_svc_NtEnumerateValueKey`, `nt_svc_NtQueryKey`, `nt_svc_NtQueryValueKey`, `nt_svc_NtSetValueKey`, `nt_svc_NtDeleteValueKey`. Each takes twelve `unsigned long` arguments (the raw syscall argument block). Each wrapper is defined twice in `registry.c`, once in the `__KERNEL__` branch and once in the `#else` (host shim) branch, with the same argument handling.

The services in `src/kernel/nt/nt_service_list.h` that touch the registry are the eleven listed above; `docs/status/SERVICES.md` marks all eleven `IMPLEMENTED (userspace) / IMPLEMENTED (kernel)`.

Constants declared in the header: value types `REG_NONE` through `REG_QWORD` (0 to 11), create options (`REG_OPTION_VOLATILE`, `REG_OPTION_NON_VOLATILE`, `REG_OPTION_CREATE_LINK`, `REG_OPTION_BACKUP_RESTORE`, `REG_OPTION_OPEN_LINK`), key access masks (`KEY_READ`, `KEY_WRITE`, `KEY_ALL_ACCESS`, and related bits), and the information classes and structures for `NtQueryKey`, `NtEnumerateKey`, `NtQueryValueKey` and `NtEnumerateValueKey`.

## Key data structures

- `struct nt_key` - `rwlock_t lock`, `subkeys` list (of `subkey_entry`, each holding a referenced header), `values` list (of `reg_value`), `full_path`, `key_name`, `class_name`, `last_write_time`, `deleted`.
- `struct reg_value` - `name` (owned copy), `type`, `data_len`, `data` (heap buffer, may be zero length).
- Each key body is an object-manager body of `nt_key_type`. `registry_delete` frees values, subkey entries, and the body.

## Persistence layout

- Base path: `<g_kshim_root>/nt_registry`. If `g_kshim_root` is unset, the code falls back to `/tmp/nt_registry`.
- Key creation calls `persist_create_key`, value writes call `persist_write_value`, value deletes call `persist_delete_value`, and `NtDeleteKey` calls `persist_delete_key`. These write per-key files and are separate from the snapshot below.
- Snapshot: `<base>/hive_snapshot.bin`, written by `NtFlushKey`. Layout, all integers little-endian:
  - the 6-byte magic `NTREG\x01`, then a u32 key count;
  - one record per key, depth-first from each top-level key under `\Registry`: a u32 length and the UTF-16 full path, a u32 value count, and for each value a u32 name length and name, a u32 type, a u32 data length and the data bytes.
  - The key count is patched into the header after the walk. The walk takes each key's read lock and holds references to child keys while it runs. If the walk runs out of memory, `NtFlushKey` returns `STATUS_NO_MEMORY` and does not replace the file.
- `NtFlushKey` replaces the file (`O_TRUNC`), fsyncs it, and in the shim branch also fsyncs the base directory. The kernel branch uses `filp_open`/`kernel_write`/`vfs_fsync`. A write failure returns `STATUS_ACCESS_DENIED`.
- `REG_OPTION_VOLATILE` is not honoured: volatile keys are written and included in the snapshot like any other key.

## Invariants and locking (as implemented)

- Each key's contents are guarded by its own `rwlock_t`. `registry.c` uses `read_lock`/`read_unlock` and `write_lock`/`write_unlock` on `k->lock`.
- Entry points call `ntreg_init()` before doing work, so initialisation is lazy.
- Key handles are validated with `ObReferenceObjectByHandle` against `nt_key_type` before use (see `NtFlushKey` and `NtDeleteKey`).
- Subkey entries hold a counted reference to the child object header (`subkey_entry.hdr`, "ref held").

## Known gaps (evident in code)

- Not implemented, and not in `nt_service_list.h`, so no handler is added: `NtRenameKey`, `NtSaveKey`/`NtSaveKeyEx`, `NtNotifyChangeKey`, `NtQueryMultipleValueKey`, `NtCreateKeyTransacted`. Adding any of them needs a service-table entry with an SSN first.
- `REG_OPTION_CREATE_LINK` is refused, so symbolic-link keys (`REG_LINK` values and `SymbolicLinkValue` resolution) do not exist. `REG_OPTION_OPEN_LINK` has no effect for the same reason.
- `REG_OPTION_VOLATILE` and `REG_OPTION_BACKUP_RESTORE` are ignored. `NtOpenKeyEx` ignores `OpenOptions` entirely (`registry.c:903`).
- The regf-compatible hive writer described in `registry.h` is not implemented. The current format is the `NTREG` snapshot above.
- Per-key directory-tree persistence described in the design note is not implemented. The per-key files above are the only per-key output.
- Write access to the Machine hive is restricted by path, not by the security module. `NtCreateKey` (`registry.c:714`) and `NtOpenKeyEx` (`registry.c:911`) return `STATUS_ACCESS_DENIED` when the path is under Machine and the desired access includes `KEY_SET_VALUE` or `KEY_CREATE_SUB_KEY`; `NtCreateKey` also refuses the bit `0x00020000`. WOW64 reserved bits are stripped first (`strip_wow64`). No other per-key access check is visible, and the security module is not called from here.
