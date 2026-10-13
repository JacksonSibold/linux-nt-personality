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
- The share registry: a file-scope list of open instances keyed by `(dev, ino)`, guarded by the spinlock `ntio_share_mutex` (declared at `io.c:581`, lock and unlock helpers at `io.c:585-603`).
- `FILE_*_INFORMATION` structures for the information classes, and `FILE_*` constants, declared in `io.h`.

## Invariants and locking (as implemented)

- Share checks run under `ntio_share_lock`. `ntio_check_share_access` is called on every open, comparing requested `DesiredAccess` against the `ShareAccess` of existing instances on the same `(dev, ino)`. The conflicting open gets `STATUS_SHARING_VIOLATION` (`io.c:1493`).
- File object lifetime is reference-counted with `kref` (`kref_init` at `io.c:1087`, `kref_put` with `ntio_file_release` at `io.c:573`). `ntio_file_get` and `ntio_file_put` (`io.c:563-574`) take and drop the same `kref`.
- A file object's own state is protected by its `spinlock_t lock`.
- `FILE_DELETE_ON_CLOSE` sets `delete_on_close` at create (`io.c:1497`), and `delete_pending` tracks the deferred delete.
- Console pseudo-handles -10, -11 and -12 (stdin, stdout, stderr) are defined in `io.c` (`io.c:1794-1815`).
- Completion of an I/O with an associated port posts a packet to that port through `ntio_iocp_post` (`io.c:1790`). The header comment says completion goes through a port unless the caller supplied an event or APC.

## Known gaps (evident in code)

- The header comment (`io.c:12-14`) says async requests are completed by a workqueue and that the unit-test build runs them "synchronously in `schedule_work()`". The file contains no `schedule_work` call, and no `work_struct` or `queue_work` use, only the comment. Completion appears to run inline.
- Stdin is not backed in the shim build: `io.c:1872-1874` returns `STATUS_END_OF_FILE` with the comment "stdin not yet backed".
- Generic mapping for files is described as "simplified" (`io.c:318`).
- `nt_svc_NtClose` has four definitions in `src/kernel/nt`: `io.c:2467` (weak, kernel), `io.c:2506` and `nt_obj.c:762` (non-kernel, strong), and `nt_obj.c:1086` (kernel). Which definition is linked depends on the build, which I did not trace. Two non-weak definitions in the same link would be a conflict.
- The kernel-side wrappers in `io.c` target a specific kernel version. The comment at `io.c:54-60` lists 6.18 API changes (`vfs_rename` taking `renamedata`, `mnt_idmap` arguments) and notes that NFS delegation is not handled ("NFS not exported in the test VM").
- Share enforcement is in-kernel only. Linux has no mandatory share mode, so non-NT opens of the same file are not blocked.
- `ntio_resolve_path` translates NT paths to Linux paths. The `\??` and `\Device` namespace mapping was not traced in full.
- The `security.md` module's access check is not called from `io.c`.
- Directory enumeration (`NtQueryDirectoryFile`, `NtQueryDirectoryFileEx`) takes a name snapshot in the userspace build only. The kernel branch returns `STATUS_NOT_SUPPORTED` until an `iterate_dir` actor is written and built against the kernel.
- Volume information: only `FileFsDeviceInformation` is answered. The other `FileFs*` classes return `STATUS_NOT_IMPLEMENTED`, and `NtSetVolumeInformationFile` refuses every class.
- Byte-range locks refuse a conflict at once. Waiting for a lock (`FailImmediately` FALSE) is not supported. Extended attributes are not stored.
