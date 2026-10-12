# ntdll: module reference index

These pages describe the modules under `src/ntdll/`. Each page covers purpose, public API (taken from the headers and exports), data structures, behaviour as implemented, invariants and known gaps. Line references are to the files as they exist in this repository on `haiku/ntdllrev`.

The pages describe code as written. Nothing here has been run under Windows, and the MinGW ntdll build has not been linked. Defects are listed in `docs/review/ntdll.md` and cited by ID. The kernel-side modules are indexed separately in [INDEX.md](./INDEX.md).

| Module | Sources | Summary |
|---|---|---|
| [ntdll-loader](./ntdll-loader.md) | `ldr.c`, `ldr.h` | User-mode PE loader (`Ldr*`). Full implementation in the Linux build only; the MinGW build has stubs. |
| [ntdll-startup](./ntdll-startup.md) | `ntdll_main.c` | Process entry (`LdrInitializeThunk`), the MinGW DLL mapper, export and forwarder lookup, `RtlGetVersion`. |
| [ntdll-unwind](./ntdll-unwind.md) | `unwind.c`, `unwind.h` | x64 table-driven unwinding: lookup, `UNWIND_INFO` decoding, handlers, captured context. |
| [ntdll-except](./ntdll-except.md) | `except.c`, `except.h` | Exception dispatch, vectored handlers, unhandled filter, Linux signal conversion. |
| [ntdll-syscalls](./ntdll-syscalls.md) | `syscall_stub.S`, `syscall_table.def`, `nt_syscalls.h`, `ntdll.def` | The 171 `Nt*` syscall stubs, their SSN table, and the export surface. |
| [ntdll-ntstatus](./ntdll-ntstatus.md) | `ntstatus.h`, `ntstatus_to_dos.c` | NTSTATUS constants and `RtlNtStatusToDosError` (520-entry table, binary search, fallback). |
| [ntdll-runtime](./ntdll-runtime.md) | `ntdll_runtime.c` | Freestanding string, memory and allocator functions, with stubs for stdio. |
| [ntdll-win32](./ntdll-win32.md) | `kernel32.c`, `kernelbase.c`, `user32.c`, `advapi32.c`, `msvcrt.c`, `.def` files | Win32 and CRT shims over the NT calls, including the placeholder functions. |

## Cross-module notes

- **Loader.** `ntdll-startup` is the loader that runs in the MinGW build. `ntdll-loader` is a separate implementation that is tested on Linux. They do not share state, and the Win32 shims call the `ntdll-loader` API, which is stubbed in the MinGW build.
- **Unwind and dispatch.** `ntdll-except` calls `ntdll-unwind` to walk frames. `ntdll-startup` registers dynamic tables with `ntdll-unwind` (`RtlAddFunctionTable`) and forwards `KiUserExceptionDispatcher` to `ntdll-except`.
- **Status to error.** Every shim that reports a failure uses `RtlNtStatusToDosError` from `ntdll-ntstatus`.
- **Syscalls.** `ntdll-syscalls` is the only place user code enters the kernel. `syscall_table.def` says the kernel dispatch table is built from it too. The kernel side (`docs/modules/dispatch.md`) uses `nt_service_list.h`, and this review did not check whether the two agree.

## Host tests

`tests/ntdll-host/` builds and runs host tests for the pure files: `ntstatus_to_dos.c`, `unwind.c` (lookup), `except.c` (vectored handlers) and `ntdll_runtime.c` (allocator). Run `make -C tests/ntdll-host test`. The loader, startup and Win32 shims are not covered by host tests.
