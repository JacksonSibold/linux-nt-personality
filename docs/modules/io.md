# Module: io (NT I/O manager, file objects)

Sources: `src/kernel/nt/io.c`, `src/kernel/nt/io.h`
Related: `iocp.md` (completion ports and user APCs that io associates with files), `handle.md` (handles), `object.md`, `security.md` (not called here), `io_test.c` (tests).

## Purpose

Implements the user-visible NT file services on top of Linux VFS primitives: `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, the information-class queries and sets, flush, and close. It adds NT semantics that VFS does not provide: share-access enforcement across open instances, delete-on-close and delete-pending state, NT path resolution, and console pseudo-handles. The file header describes the design: share access is enforced in-kernel because Linux has no mandatory share model.

## Public API (io.h)

Service entry points (NT-style signatures, defined in `io.c`):

- `nt_svc_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)` - create or open a file, checking share access.
- `nt_svc_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions)` - open an existing file.
- `nt_svc_NtReadFile(HANDLE FileHandle, HANDLE Event, ...)` and `nt_svc_NtWriteFile(HANDLE FileHandle, HANDLE Event, ...)` - read and write, with an optional event for completion.
- `nt_svc_NtQueryInformationFile(HANDLE FileHandle, ...)` and `nt_svc_NtSetInformationFile(HANDLE FileHandle, ...)` - information classes (the structures `FILE_*_INFORMATION` are declared in `io.h`).
- `nt_svc_NtFlushBuffersFile(HANDLE FileHandle, ...)` - flush.
- `nt_svc_NtClose` - close a handle. Four definitions exist in `src/kernel/nt` (`io.c`, `nt_obj.c`). See gaps.

Further services defined in `io.c` (inventory status in `docs/status/SERVICES.md`, all IMPLEMENTED):

- Files and attributes: `NtQueryAttributesFile` (`io.c:2723`), `NtQueryFullAttributesFile` (`:3256`), `NtDeleteFile` (`:3220`).
- Byte-range locks: `NtLockFile` (`:3311`), `NtUnlockFile` (`:3380`).
- Cancel and flush: `NtCancelIoFile` (`:3199`), `NtCancelIoFileEx` (`:3205`), `NtFlushBuffersFileEx` (`:3146`).
- Device and FS control: `NtDeviceIoControlFile` (`:3024`), `NtFsControlFile` (`:3042`).
- Extended attributes: `NtQueryEaFile` (`:3083`), `NtSetEaFile` (`:3115`).
- Directories: `NtQueryDirectoryFile` (`:3747`), `NtQueryDirectoryFileEx` (`:3764`).
- Volumes: `NtQueryVolumeInformationFile` (`:3811`), `NtSetVolumeInformationFile` (`:3860`).

Missing in the dispatch table (NULL slot): the object-directory and symbolic-link services `NtCreateDirectoryObject`, `NtOpenDirectoryObject`, `NtQueryDirectoryObject`, `NtCreateSymbolicLinkObject`, `NtOpenSymbolicLinkObject` and `NtQuerySymbolicLinkObject`. The inventory groups them under io by name only.

Object and handle helpers:

- `void *ntio_handle_peek(HANDLE h)`.
- `NTSTATUS ntio_handle_create(void *obj, ACCESS_MASK granted, HANDLE *out)`, `void *ntio_handle_lookup(HANDLE h, ACCESS_MASK required)`, `NTSTATUS ntio_handle_close(HANDLE h)`, `ACCESS_MASK ntio_handle_granted(HANDLE h)`.
- `NTSTATUS ntio_duplicate_file_handle(struct nt_file_object *f, ACCESS_MASK new_access, HANDLE *out_handle)`.
- `ULONG ntio_count_handles_for_file(struct nt_file_object *f)` and `ULONG ntio_pointer_count_for_file(struct nt_file_object *f)` - counts used for close-pending decisions.
- `struct nt_file_object *ntio_file_get(struct nt_file_object *)` and `void ntio_file_put(struct nt_file_object *)`.

Share registry:

- `void ntio_share_lock(void)` / `void ntio_share_unlock(void)`.
- `bool ntio_check_share_access(dev_t dev, ino_t ino, ACCESS_MASK desired_access, ULONG share_access, struct nt_file_object *exclude)`.
- `void ntio_register_share(struct nt_file_object *)` and `void ntio_unregister_share(struct nt_file_object *)`.

Path and test helpers:

- `NTSTATUS ntio_resolve_path(POBJECT_ATTRIBUTES obj_attr, char *linux_path, size_t pathlen, bool *is_dir)` - map an NT object path to a Linux path.
- `void ntio_reset_handle_table(void)` and `void ntio_reset_share_registry(void)` - test resets.

## Key data structures

- `struct nt_file_object` - `magic`, `ref` (kref), `lock` (spinlock_t), `filp` (struct file), `dentry` (borrowed), `granted_access`, `share_access`, `create_options`, flags `synchronous`, `delete_on_close`, `delete_pending`, `is_directory`, `iocp` and `iocp_key` (association), and `current_byte_offset` for synchronous I/O.
- The share registry: a file-scope list of open instances keyed by `(dev, ino)`, guarded by the spinlock `ntio_share_mutex` (declared at `io.c:691`). `ntio_share_lock` and `ntio_share_unlock` are at `io.c:707` and `io.c:712`.
- `FILE_*_INFORMATION` structures for the information classes, and `FILE_*` constants, declared in `io.h`.

## Invariants and locking (as implemented)

- Share checks run under `ntio_share_lock`. `ntio_check_share_access` (`io.c:774`) is called on every open (`io.c:1632`), comparing requested `DesiredAccess` against the `ShareAccess` of existing instances on the same `(dev, ino)`. The conflicting open gets `STATUS_SHARING_VIOLATION` (`io.c:1636`). `NtDeleteFile` also checks share access (`io.c:3240-3244`).
- File object lifetime is reference-counted with `kref` (`kref_init` at `io.c:1212`, `kref_put` with `ntio_file_release` at `io.c:683` and `:657`). `ntio_file_get` and `ntio_file_put` (`io.c:673` and `:680`) take and drop the same `kref`.
- A file object's own state is protected by its `spinlock_t lock`.
- `FILE_DELETE_ON_CLOSE` sets `delete_on_close` at create (`io.c:1622`). `delete_pending` is set by the disposition information classes (`io.c:2607`, `:2650`).
- Console pseudo-handles -10, -11 and -12 (stdin, stdout, stderr) are described at `io.c:1923-1940`. `NtWriteFile` on -11 and -12 routes to printk in the kernel build and to host stdout or stderr in the shim (`io.c:1974`).
- Completion of an I/O with an associated port posts a packet through `ntio_iocp_post`, defined in `iocp.c:80` and called from `io.c:1919`. The header comment says completion goes through a port unless the caller supplied an event or APC.

## Known gaps (evident in code)

- The header comment (`io.c:12-14`) says async requests are completed by a workqueue and that the unit-test build runs them "synchronously in `schedule_work()`". The file contains no `schedule_work` call, and no `work_struct` or `queue_work` use, only the comment. Completion appears to run inline.
- Stdin is not backed in the shim build: `NtReadFile` on -10 returns `STATUS_END_OF_FILE` with the comment "stdin not yet backed" (`io.c:2001`).
- Generic mapping for files is described as "simplified" (`io.c:318`).
- `nt_svc_NtClose` has four definitions in `src/kernel/nt`: `io.c:2911` (weak, raw-argument signature), `io.c:2950` and `nt_obj.c:777` (strong, `HANDLE` signature), and `nt_obj.c:1125` (raw-argument signature). Which definition is linked depends on the build, which I did not trace. Two strong definitions in the same link would be a conflict. The inventory counts the first strong one, `io.c:2950`.
- The kernel-side wrappers in `io.c` target a specific kernel version. The comment at `io.c:54-60` lists 6.18 API changes (`vfs_rename` taking `renamedata`, `mnt_idmap` arguments) and notes that NFS delegation is not handled ("NFS not exported in the test VM").
- Share enforcement is in-kernel only. Linux has no mandatory share mode, so non-NT opens of the same file are not blocked.
- `ntio_resolve_path` translates NT paths to Linux paths. The `\??` and `\Device` namespace mapping was not traced in full.
- The `security.md` module's access check is not called from `io.c`.
- Directory enumeration (`NtQueryDirectoryFile`, `NtQueryDirectoryFileEx`) takes a name snapshot in the userspace build only. The kernel branch returns `STATUS_NOT_SUPPORTED` until an `iterate_dir` actor is written and built against the kernel.
- Volume information: `NtQueryVolumeInformationFile` answers only `FileFsDeviceInformation` (`io.c:3831`). The other `FileFs*` classes in the valid range return `STATUS_NOT_IMPLEMENTED`, and unknown classes return `STATUS_INVALID_INFO_CLASS`. `NtSetVolumeInformationFile` refuses every class, with the same two statuses (`io.c:3860`).
- Byte-range locks: `NtLockFile` accepts `FailImmediately` but ignores it. Waiting is not supported, so a conflict with a lock held through another file object always returns `STATUS_LOCK_NOT_GRANTED` (comment at `io.c:3305-3310`).
- Extended attributes: `NtSetEaFile` accepts only a zero length. Any other length returns `STATUS_EAS_NOT_SUPPORTED` (`io.c:3115`).
