# Kernel NT personality: module reference index

These pages describe the modules under `src/kernel/nt/`. Each page covers purpose, public API (taken from the headers), key data structures, invariants and locking as they appear in the code, and known gaps visible in the code or the project README.

The project is an incomplete research prototype (see the top-level `README.md`). Nothing here has been built or run. Claims about behaviour come from reading the source, and line references are to the files as they exist in this repository.

| Module | Sources | Summary |
|---|---|---|
| [object](./object.md) | `object.c`, `object.h` | Object manager: headers, types, `\` namespace, reference counts, process and thread objects. |
| [handle](./handle.md) | `handle.c`, `handle.h` | Per-process three-level handle table, pseudo-handles, duplication and inheritance. |
| [registry](./registry.md) | `registry.c`, `registry.h` | Registry keys and values as object-manager objects, with a snapshot-file persistence stub. |
| [sync](./sync.md) | `sync.c`, `sync.h` | Dispatcher objects (event, mutant, semaphore, timer, keyed event), WaitAny/WaitAll engine, address waits. |
| [security](./security.md) | `security.c`, `security.h` | SIDs, tokens, ACLs and ACEs, security descriptors, `se_access_check`. Not wired into object paths. |
| [mm](./mm.md) | `mm.c`, `mm.h` | Virtual memory: reserve and commit, 64 KiB granularity, VAD map, guard pages, sections. |
| [io](./io.md) | `io.c`, `io.h` | File services over the Linux VFS, share-access enforcement, delete-on-close, console pseudo-handles. |
| [iocp](./iocp.md) | `iocp.c`, `iocp.h` | I/O completion ports with a concurrency limit, and user APC queueing. |
| [proc_thread](./proc_thread.md) | `proc_thread.c`, `proc_thread.h` | Process and thread creation, suspend counts, thread start stub, information classes. |
| [dispatch](./dispatch.md) | `dispatch.c`, `nt_service_list.h`, `nt_internal.h` | SSN-indexed system-call table with weak service symbols, per-CPU counters, debugfs. |
| [ntstatus_kernel](./ntstatus_kernel.md) | `ntstatus_kernel.c`, `ntstatus_kernel.h`, `gen_ntstatus_table.py` | NTSTATUS to DOS error mapping, generated from the userspace table. |

## Cross-module notes

- **Handles and objects.** `handle` resolves handles to `object` headers and takes references. `io`, `registry`, `sync`, `iocp` and `proc_thread` create their objects through the object manager.
- **Dispatch.** `dispatch` routes SSNs to `nt_svc_*` functions defined in the modules above. Unimplemented or unlisted service names resolve to NULL and return `STATUS_NOT_IMPLEMENTED`.
- **Locking.** The modules use two build flavours: a userspace shim (`USERSPACE_KSHIM`, pthreads) and the kernel build (`__KERNEL__`). Lock primitives differ between them, and some functions have separate bodies per branch.
- **Security.** `security` provides SeAccessCheck-style evaluation, but no call sites outside its own tests were found. The access checks that do exist in other modules are path or access-mask rules local to each module (for example the Machine-hive write rule in `registry`).

## Files not covered here

These files are in `src/kernel/nt/` but are not part of the modules above: `nt_obj.c` (object-handle service wrappers for events, mutants, semaphores and files), `nt_services.c` (event, mutant and semaphore query and set wrappers), `kshim.c` and `kshim.h` (the kernel shim layer), `weak.c` (weak thread-teardown hooks), `repro_mem_vad.c`, and the `*_test.c` and `debugfs_tests.c` test drivers. The `nt_svc_*` definitions in `nt_obj.c` and `nt_services.c` are referenced from the module pages where they matter.
