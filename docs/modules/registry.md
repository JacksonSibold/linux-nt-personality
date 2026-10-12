# Module: registry (Registry personality)

Sources: `src/kernel/nt/registry.c`, `src/kernel/nt/registry.h`
Related: `object.md` (keys are object-manager objects), `handle.md`, `security.md` (no access check is wired in, see gaps).

## Purpose

Implements the NT registry as key objects inside the object manager. Keys form a tree rooted at well-known hives (`Machine` and `User` are created in `ntreg_init`, `registry.c:503`). Values are typed byte blobs. Persistence is written under the guest filesystem so the tree can be inspected with ordinary tools. The `registry.h` design note describes a per-key directory tree plus a monolithic snapshot file, with a planned regf-compatible format that is not implemented.

## Public API (registry.h)

Lifecycle:

- `NTSTATUS ntreg_init(void)` - create the registry root and hives. Every entry point calls it lazily.
- `void ntreg_cleanup(void)` - tear down.
- `extern struct nt_object_type nt_key_type` - the key object type, registered with `ObRegisterObjectType` (`registry.c:463`).

Key operations (native-style signatures):

- `NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition)` - open or create a key. `Disposition` is `REG_CREATED_NEW_KEY` (1) or `REG_OPENED_EXISTING_KEY` (2).
- `NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes)` and `NtOpenKeyEx(... , ULONG OpenOptions)` - open an existing key.
- `NtDeleteKey(HANDLE KeyHandle)` - delete a key.
- `NtFlushKey(HANDLE KeyHandle)` - persist the hive snapshot (see gaps).
- `NtEnumerateKey(HANDLE, ULONG Index, ULONG KeyInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - enumerate subkeys.
- `NtQueryKey(HANDLE, ULONG KeyInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - query key metadata.

Value operations:

- `NtSetValueKey(HANDLE, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize)` - create or replace a value.
- `NtQueryValueKey(HANDLE, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - read one value.
- `NtEnumerateValueKey(HANDLE, ULONG Index, ULONG KeyValueInformationClass, PVOID, ULONG Length, PULONG ResultLength)` - enumerate values.
- `NtDeleteValueKey(HANDLE, PUNICODE_STRING ValueName)` - delete a value.

Dispatch wrappers: `nt_svc_NtCreateKey`, `nt_svc_NtOpenKey`, `nt_svc_NtOpenKeyEx`, `nt_svc_NtDeleteKey`, `nt_svc_NtFlushKey`, `nt_svc_NtEnumerateKey`, `nt_svc_NtEnumerateValueKey`, `nt_svc_NtQueryKey`, `nt_svc_NtQueryValueKey`, `nt_svc_NtSetValueKey`, `nt_svc_NtDeleteValueKey`. Each takes twelve `unsigned long` arguments (the raw syscall argument block).

Constants declared in the header: value types `REG_NONE` through `REG_QWORD` (0 to 11), create options (`REG_OPTION_VOLATILE`, `REG_OPTION_NON_VOLATILE`, `REG_OPTION_CREATE_LINK`, `REG_OPTION_BACKUP_RESTORE`, `REG_OPTION_OPEN_LINK`), key access masks (`KEY_READ`, `KEY_WRITE`, `KEY_ALL_ACCESS`, and related bits), and the NT information-class structures for `NtQueryKey` and `NtQueryValueKey`.

## Key data structures

- `struct nt_key` - `rwlock_t lock`, `subkeys` list (of `subkey_entry`, each holding a referenced header), `values` list (of `reg_value`), `full_path`, `key_name`, `class_name`, `last_write_time`, `deleted`.
- `struct reg_value` - `name` (owned copy), `type`, `data_len`, `data` (heap buffer, may be zero length).
- Each key body is an object-manager body of `nt_key_type`. `registry_delete` frees values, subkey entries, and the body.

## Persistence layout

- Base path: `<g_kshim_root>/nt_registry`. If `g_kshim_root` is unset, the code falls back to `/tmp/nt_registry` (`registry.c:210-214`).
- Snapshot: `<base>/hive_snapshot.bin`, written with the 6-byte magic `NTREG\x01` followed by a 4-byte count.
- `NtFlushKey` writes the snapshot, fsyncs the file and the directory (shim path), and in the kernel path uses `filp_open`/`kernel_write`/`vfs_fsync`.

## Invariants and locking (as implemented)

- Each key's contents are guarded by its own `rwlock_t`. `registry.c` uses `read_lock`/`read_unlock` (7 and 18 sites) and `write_lock`/`write_unlock` (6 and 11 sites) on `k->lock`.
- Entry points call `ntreg_init()` before doing work (10 call sites), so initialisation is lazy.
- Key handles are validated with `ObReferenceObjectByHandle` against `nt_key_type` before use (see `NtFlushKey`, `registry.c:967-972`).
- Subkey entries hold a counted reference to the child object header (`subkey_entry.hdr`, "ref held").

## Known gaps (evident in code)

- `NtFlushKey` writes only the magic and a count field, and the count is hard-coded to 0 in both the shim branch (`registry.c:990-991`) and the kernel branch (`registry.c:1003`). The snapshot therefore does not contain key or value data, so the "externally parseable" snapshot is not a real dump of the tree.
- The regf-compatible hive writer described in `registry.h` is not implemented. The current format is the `NTREG` stub above.
- `nt_svc_*` wrappers appear twice in `registry.c` (one per build branch). I did not trace which branch each build uses.
- Write access to the Machine hive is restricted by path, not by the security module. `NtCreateKey` and `NtOpenKeyEx` return `STATUS_ACCESS_DENIED` when the path is under Machine and the desired access includes `KEY_SET_VALUE` or `KEY_CREATE_SUB_KEY` (`registry.c:709-713`, `906-908`). WOW64 reserved bits are stripped first (`strip_wow64`, `registry.c:640`). No other per-key access check is visible, and the security module is not called from here.
- `REG_OPTION_CREATE_LINK` and `REG_OPTION_OPEN_LINK` are defined in the header; I did not find code in `registry.c` that acts on them.
- Per-key directory-tree persistence described in the design note is not implemented. Only the snapshot file is written.
