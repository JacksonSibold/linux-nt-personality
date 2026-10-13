# Module: proc_create_args (NtCreateUserProcess argument copy-in)

Sources: `src/kernel/nt/proc_create_args.h` (header only, `static inline`)
Used by: `src/kernel/nt/proc_thread.c` (`nt_svc_NtCreateUserProcess`, kernel branch, `proc_thread.c:1454`) and `arch/x86/nt/proc_thread.c` (`:1366`)
Tests: `tests/kernel-host/proc_create_args_test.c`
Process-creation milestone M5 (partial), see [process-creation](../architecture/process-creation.md).
Related: [process_params](process_params.md) (the block format `ProcessParameters` would carry)

## Purpose

`NtCreateUserProcess` takes two structures from user memory: `PS_CREATE_INFO` (`CreateInfo`) and `PS_ATTRIBUTE_LIST` (`AttributeList`). User memory can change at any time, so each structure is copied into a kernel buffer once, and only the copy is checked and used. The copy function is the probe. In the kernel it is `copy_from_user`, which checks the address range and returns the number of bytes it could not copy.

Nothing here reads user memory any other way. Sizes are checked before any arithmetic on them, so a huge `TotalLength` cannot wrap an entry count.

This is validation only. Nothing here creates a process.

## Layout constants (x64)

- `PS_CREATE_INFO` is 0x58 bytes. `Size` (SIZE_T) is at offset 0 and `State` (ULONG) at offset 8. Only `PsCreateInitialState` (0) is accepted.
- `PS_ATTRIBUTE_LIST`: header of 8 bytes (`TotalLength`), then entries of 0x20 bytes each. At most 32 entries (`PCA_MAX_ATTRIBUTES`), so `PCA_ATTR_LIST_MAX_BYTES` is 0x408.

## API

- `typedef int (*pca_copy_fn)(void *to, const void *from, size_t n)` - returns 0 on success.
- `PcaCopyInCreateInfo(copy, user, out)` - copies 0x58 bytes. The copy must succeed, `Size` must equal 0x58 (`STATUS_INFO_LENGTH_MISMATCH` otherwise), and `State` must be `PsCreateInitialState` (`STATUS_INVALID_PARAMETER` otherwise). A read fault is `STATUS_ACCESS_VIOLATION`.
- `PcaCopyInAttributeList(copy, user, out)` - a NULL pointer is an empty list. Otherwise the header is read first and its `TotalLength` checked, then the whole list is copied, and the header of that second copy must match the first. The checks are:

| Condition | Status |
|---|---|
| `TotalLength` below the header | `STATUS_INFO_LENGTH_MISMATCH` |
| `TotalLength` above the maximum | `STATUS_INVALID_PARAMETER` |
| `(TotalLength - header)` not a multiple of the entry size | `STATUS_INFO_LENGTH_MISMATCH` |
| header changed between the two reads | `STATUS_INVALID_PARAMETER` |
| a read faults | `STATUS_ACCESS_VIOLATION` |

- `PcaAttributeAt(list, i, attr)` decodes entry `i` of a validated list. Out of range is `STATUS_INVALID_PARAMETER`.
- `PcaCopyInUserProcessArgs(copy, create_info_user, attr_list_user, ci, al)` runs both copy-ins and returns the first failure. `AttributeList` is optional. `ProcessParameters` is not read at all.

## Behaviour of the NtCreateUserProcess stubs

Both kernel-branch stubs check their handle output pointers (`ProcessHandle` and `ThreadHandle`, `STATUS_INVALID_PARAMETER` if NULL), call `PcaCopyInUserProcessArgs` with `pca_copy_from_user` (a wrapper over `copy_from_user`), and then return `STATUS_NOT_IMPLEMENTED`. The argument validation therefore runs, and no process is created.

The userspace branch of `NtCreateUserProcess` does not use this header. It creates a process and a thread directly, with a dummy start address (see [process-creation](../architecture/process-creation.md), section 2).

## Known gaps

- The validator does not inspect individual attribute entries beyond the sizes. `PcaAttributeAt` decodes an entry on request; nothing consumes the entries yet, and the attribute types (handle list, parent process, image name) are not interpreted.
- `ProcessParameters` is not copied or checked. The format in the process-parameter header is the planned one, not the `RTL_USER_PROCESS_PARAMETERS` structure the stub receives ([process_params](process_params.md)).
- The host test (`proc_create_args_test.c`) replaces the copy function with one that can fault on a chosen call and can change memory between reads. The kernel `copy_from_user` is not exercised on the host.
- Only the x64 layout is defined.
