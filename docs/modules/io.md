# Module: io (NT I/O manager, file objects)

Sources: `src/kernel/nt/io.c`, `src/kernel/nt/io.h`
Related: `iocp.md` (completion ports and user APCs that io associates with files), `handle.md` (handles), `object.md`, `security.md` (not called from this module), `io_test.c` (tests). Object-directory and symbolic-link services are in `nt_obj.c` and are not part of this module.

## Purpose

Implements the user-visible NT file services on top of Linux VFS primitives: `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, the information-class queries and sets, flush, and close. It adds NT semantics that VFS does not provide: share-access enforcement across open instances, delete-on-close and delete-pending state, NT path resolution, byte-range locks, and console pseudo-handles. The file header says share access is enforced in the kernel because Linux has no mandatory share model.

## Service entry points (io.c)

Each service has one definition in `io.c`, in the form that dispatch calls. Line numbers below are the definitions.

- Create and open: `NtCreateFile` (`:1712`), `NtOpenFile` (`:1733`).
- Read and write: `NtReadFile` (`:2036`), `NtWriteFile` (`:2106`).
- Information: `NtQueryInformationFile` (`:2463`), `NtSetInformationFile` (`:2816`), `NtQueryAttributesFile` (`:2790`), `NtQueryFullAttributesFile` (`:3325`), `NtDeleteFile` (`:3289`).
- Flush and close: `NtFlushBuffersFile` (`:2925`), `NtFlushBuffersFileEx` (`:3215`), `NtClose` (`:3019`).
- Cancel: `NtCancelIoFile` (`:3268`), `NtCancelIoFileEx` (`:3274`).
- Device and FS control: `NtDeviceIoControlFile` (`:3093`), `NtFsControlFile` (`:3111`).
- Extended attributes: `NtQueryEaFile` (`:3152`), `NtSetEaFile` (`:3184`).
- Byte-range locks: `NtLockFile` (`:3383`), `NtUnlockFile` (`:3452`).
- Directories: `NtQueryDirectoryFile` (`:3897`), `NtQueryDirectoryFileEx` (`:3914`).
- Volumes: `NtQueryVolumeInformationFile` (`:3961`), `NtSetVolumeInformationFile` (`:4010`).

`docs/status/SERVICES.md` classes every service in this list as IMPLEMENTED. The class says the body is not a refusal, not that it is complete; the gaps are below.

`NtClose` has four definitions across this module and `nt_obj.c`: `io.c:2980` (weak, raw-argument form), `io.c:3019` (`HANDLE` form), `nt_obj.c:1061` (`HANDLE` form) and `nt_obj.c:1727` (raw form). Which one links in a given build was not traced in full. See the gaps.

Object and handle helpers: `ntio_handle_peek`, `ntio_handle_create`, `ntio_handle_lookup(h, required)`, `ntio_handle_close`, `ntio_handle_granted`, `ntio_duplicate_file_handle`, the counts `ntio_count_handles_for_file` and `ntio_pointer_count_for_file`, and `ntio_file_get` (`io.c:684`) and `ntio_file_put` (`io.c:691`). The last drop of a file's `kref` calls `ntio_file_release` (`io.c:668`).

Share registry: `ntio_share_lock` (`io.c:737`) and `ntio_share_unlock` (`io.c:744`), `ntio_check_share_access` (`io.c:804`), `ntio_register_share` and `ntio_unregister_share`.

Path and test helpers: `ntio_resolve_path` (`io.c:1094`) maps an NT object path to a Linux path. `ntio_reset_handle_table` and `ntio_reset_share_registry` are test resets.

## Key data structures

- `struct nt_file_object` (`io.h`) - `magic`, `ref` (a kref), `lock` (spinlock), `filp`, `dentry` (borrowed), `granted_access`, `share_access`, `create_options`, flags `synchronous`, `delete_on_close`, `delete_pending` and `is_directory`, the completion association `iocp` and `iocp_key`, and `current_byte_offset` for synchronous I/O.
- The share registry is a file-scope list of open instances keyed by `(dev, ino)`. It is guarded by `ntio_share_mutex`, a `spinlock_t` declared at `io.c:721` and initialised at `io.c:732`.
- `FILE_*_INFORMATION` structures and `FILE_*` constants, declared in `io.h`.

## Invariants and locking (as implemented)

- Share checks run under `ntio_share_mutex`. `ntio_check_share_access` is called on open (`io.c:1662`), comparing the requested access against the share mode of existing instances on the same `(dev, ino)`. A conflict gives `STATUS_SHARING_VIOLATION`. `NtDeleteFile` also checks share access (`io.c:3308`-`:3311`).
- File object lifetime is reference-counted with `kref`. `ntio_file_get` and `ntio_file_put` take and drop the same count, and the last drop runs `ntio_file_release`.
- A file object's own state is protected by its `spinlock_t lock`.
- `FILE_DELETE_ON_CLOSE` sets `delete_on_close` at create. `delete_pending` is set by the disposition information classes.
- Console pseudo-handles -10, -11 and -12 (stdin, stdout, stderr). `NtWriteFile` on -11 and -12 writes to the kernel log in the kernel build and to host stdout or stderr in the shim.
- Completion of an I/O on a file with an associated port posts a packet through `ntio_iocp_post` (called at `io.c:1949`; defined in `iocp.c`).

## Known gaps (evident in code)

- The header comment (`io.c:12`-`:14`) says async requests complete through `schedule_work()`. The file has no `schedule_work`, `work_struct` or `queue_work` call. Completion runs inline.
- Stdin is not backed in the shim: `NtReadFile` on -10 returns `STATUS_END_OF_FILE` with the comment "stdin not yet backed" (`io.c:2031`).
- Generic mapping for files is described as "simplified" (`io.c:318`).
- `NtClose` has four definitions (see above). Two strong `HANDLE`-form definitions in one link would conflict. `tests/kernel-host` works around the duplicate symbol with `objcopy`. The link for each build was not traced.
- Share enforcement is in the kernel only. Linux has no mandatory share mode, so non-NT opens of the same file are not blocked.
- `ntio_resolve_path` translates NT paths to Linux paths. The `\??` and `\Device` mapping was not traced in full.
- The kernel wrappers target a specific kernel version. The comment near `io.c:55` lists the 6.18 API changes (`vfs_rename` takes `struct renamedata`), and NFS delegation is not handled.
- The rename path resolves a `RootDirectory` relative to that directory (audit pass 4 item 10, `32816fb`). Earlier rename checks in the same commit are fixed (`170edb9`, `520cf42`, `e3bf004`).
- Directory enumeration (`NtQueryDirectoryFile`, `NtQueryDirectoryFileEx`): the kernel branch returns `STATUS_NOT_SUPPORTED` (`io.c:3501` and `:3893`), as the comment there says. The name of each entry is converted from UTF-8 to UTF-16 (`7e43c43`, `3b187d5`).
- Volume information: `NtQueryVolumeInformationFile` answers `FileFsDeviceInformation` (`io.c:3981`). Other `FileFs*` classes in the valid range return `STATUS_NOT_IMPLEMENTED`, and unknown classes return `STATUS_INVALID_INFO_CLASS`. `NtSetVolumeInformationFile` refuses every class.
- Byte-range locks: `NtLockFile` accepts `FailImmediately` but ignores it. Waiting is not supported, so a conflict always returns `STATUS_LOCK_NOT_GRANTED` (`io.c:3434`). The conflict test saturates the byte-range end at `LLONG_MAX` (`fcca36a`).
- Extended attributes: `NtSetEaFile` accepts only a zero length. Any other length returns `STATUS_EAS_NOT_SUPPORTED` (`io.c:3202`).
- Four-KiB stack path buffers remain in `NtQueryAttributesFile`, `NtDeleteFile` and `NtQueryFullAttributesFile` (audit pass 3 item 7). Whether those move to the heap is an owner decision (`docs/OWNER-DECISIONS.md`).
