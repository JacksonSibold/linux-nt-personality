# Module: ntdll-win32 (Win32 and CRT shims: kernel32, kernelbase, user32, advapi32, msvcrt)

Sources: `src/ntdll/kernel32.c`, `src/ntdll/kernelbase.c`, `src/ntdll/user32.c`, `src/ntdll/advapi32.c`, `src/ntdll/msvcrt.c`, and the export lists `kernel32.def`, `kernelbase.def`, `user32.def`, `advapi32.def`, `msvcrt.def`, `ucrtbase.def`
Related: `ntdll-ntstatus.md` (`RtlNtStatusToDosError`, the source of `GetLastError` values), `ntdll-syscalls.md` (the `Nt*` calls these shims make), `ntdll-loader.md` (the `Ldr*` calls, see K32-4), `docs/review/ntdll.md` (findings cited below).

## Purpose

These DLLs are the Win32 surface that programs import. Each shim turns a Win32 call into one or more NT calls, and turns an NTSTATUS failure into a `GetLastError` value. Most of the code is a thin forwarder. A large part is also placeholder code that returns a fixed value, and that matters for correctness. The MinGW-only guard applies to each file.

## Layout

- `kernel32.c` (about 500 lines): file I/O, virtual memory, module loading (via `Ldr*`), heap, critical sections, TLS, synchronization, temp paths, file mapping, timing.
- `kernelbase.c`: a stub DLL with one dummy export. Its real surface is in `kernelbase.def`, which forwards 22 names, plus the module APIs, to `kernel32` (for example `CreateFileW=kernel32.CreateFileW`). Forwards are resolved at load time by the loader, which is compiled (LDR-1 fixed in 10f42c8). The module forwards use the process loader context (K32-4, fixed in 1e013ac); whether they run is not verified.
- `user32.c`: `wsprintf` family only.
- `advapi32.c`: registry stubs.
- `msvcrt.c`: the C runtime export surface (`malloc`, `printf`, `sprintf`, `_initterm`, `exit`, startup helpers). Heap calls go to kernel32's heap.
- The `.def` files list the exports. `ucrtbase.def` mirrors `msvcrt.def`.

## Error reporting

`SetLastError` writes `TEB.LastErrorValue`. `SetLastErrorFromStatus` converts an NTSTATUS with `RtlNtStatusToDosError` and stores the result. A success status sets 0. Most failing paths follow this pattern, but some paths set a fixed code without consulting the status, and a few return success without setting anything.

## Behaviour

**Files.** `CreateFileW` maps Win32 access and disposition to `NtCreateFile` (`kernel32.c:126`). `ReadFile` and `WriteFile` call `NtReadFile` and `NtWriteFile`. `CloseHandle` calls `NtClose`. `CreateFileA` widens the name and calls `CreateFileW`.

**Memory.** `VirtualAlloc` maps the protection constant to the NT value and calls `NtAllocateVirtualMemory`. `VirtualFree` calls `NtFreeVirtualMemory`. `VirtualQuery` calls `NtQueryVirtualMemory` with class 0.

**Modules.** The module APIs are listed in the Module APIs section below. They call `LdrLoadDll`, `LdrGetDllHandle`, `LdrGetProcedureAddress`, `LdrUnloadDll` and `LdrAddRefDll` with the process loader context (K32-4, fixed in 1e013ac).

**Heap.** `HeapAlloc` is a bump allocator over a 1 MiB static array. `HeapFree` does nothing. `HeapReAlloc` returns NULL. `HeapSize` returns 0.

**Process and thread.** `ExitProcess` calls `NtTerminateProcess`. `GetCurrentProcessId` and `GetCurrentThreadId` read the TEB client IDs. `CreateThread` fails with `ERROR_CALL_NOT_IMPLEMENTED`. `CreateProcessA` and `CreateProcessW` fail with `ERROR_FILE_NOT_FOUND`.

**Stubs with fixed results.** These return a constant and do not reach the kernel:

| Function | Returns | Defect |
|---|---|---|
| `WaitForSingleObject`, `WaitForMultipleObjects` | `WAIT_OBJECT_0` | K32-5 |
| `CreateEventA/W`, `CreateMutexA/W`, `CreateSemaphoreA/W` | handles 10, 11, 12 | K32-7 |
| `DeleteFileA/W` | fails with 2 | K32-8 |
| `CreateDirectoryA/W`, `RemoveDirectoryA/W` | succeed without acting | K32-8 |
| `GetTempFileNameA/W` | `/tmp/tmpfile.tmp` | K32-9 |
| `QueryPerformanceCounter` | 123456 | K32-10 |
| `SetFilePointer` | 0, pointer not moved | K32-11 |
| `CreateFileMappingA/W` | handle 1 | K32-12 |
| `MapViewOfFile` | fails with `ERROR_OUTOFMEMORY` | K32-12 |
| `DuplicateHandle` | copies the value | K32-13 |
| `EnterCriticalSection`, `LeaveCriticalSection` | nothing | K32-14 |
| `TryEnterCriticalSection` | TRUE | K32-14 |
| `TlsAlloc` | 0 | K32-2 |
| `VirtualProtect` | always PAGE_READWRITE | K32-1 |
| `GetExitCodeProcess`, `GetExitCodeThread` | 0 | K32-17 |
| `RegOpenKeyExA/W`, `RegQueryValueExA/W` | not found | ADV-1 |
| `wvsprintfA/W` | 0, no output | U32-2 |

**CRT (`msvcrt.c`).** `printf`, `fprintf` and `puts` write to the stdout handle through `WriteFile`. `my_vsprintf` implements `%s %S %c %d %i %u %x %X %p` and the length modifiers. Floating-point conversions print `0.0`. `sprintf` and `vsprintf` format into an 8192-byte limit regardless of the caller's buffer (CRT-1). `calloc` and `realloc` follow the allocator in `ntdll-runtime.md`, with the same defects (CRT-3, CRT-4). `exit` and `_exit` call `NtTerminateProcess`.

**user32.** `wsprintfW` expands `%s %d %i %u %x %X %c %%` into a caller buffer with no size limit, as the Win32 function does. `wsprintfA` expands only `%s %d %%`, and copies other conversions literally. `%s` in `wsprintfW` reads a narrow string (U32-1).

## Module APIs (K32-4)

Functions: `LoadLibraryA/W`, `LoadLibraryExA/W`, `FreeLibrary`, `GetModuleHandleA/W`, `GetModuleHandleExA/W`, `GetModuleFileNameA/W`, `GetProcAddress`, `DisableThreadLibraryCalls`. The argument rules, flag checks, name normalisation (drops the directory, adds `.dll` when there is no extension), ANSI and wide conversion, the GetModuleFileName truncation rule and the NTSTATUS to Win32 error mapping are pure functions in `src/ntdll/k32_module_pure.h`. `tests/ntdll-host/test_k32_module_pure.c` checks them, and cross-checks the error mapping against `RtlNtStatusToDosError`.

Handles: a Win32 `HMODULE` is the image base (`LDR_DATA_TABLE_ENTRY.DllBase`). The loader handle is the entry address. kernel32 converts between them by walking `PEB.Ldr`. The main executable's handle is `PEB.ImageBaseAddress`; it is not in the loader list.

Context: `LdrInitializeThunk` creates the loader context and publishes it at `PEB.SparePointers[0]` (`LDR_PEB_CONTEXT_SLOT`, `ldr.h`). The slot is a personality convention; the PEB tail is not verified against Windows.

Error policy: `ERROR_CALL_NOT_IMPLEMENTED` for `DONT_RESOLVE_DLL_REFERENCES` (the loader always runs DllMain). `ERROR_INVALID_PARAMETER` for unknown flag bits. `ERROR_FILENAME_EXCED_RANGE` for a name of 260 characters or more. Successful calls leave the last-error value unchanged, as Win32 does. The module-specific status mapping is in `k32_module_status_to_error`; anything else goes to `RtlNtStatusToDosError`.

**Verified by host tests and build checks:** the pure helpers (`test_k32_module_pure`, 31 cases); the export set and `LdrLoadDll` not being the stub (`make -C src/ntdll check`); `kernel32.c` compiles with `gcc -fsyntax-only` for the MinGW target.

**Needs the running personality (not verified):** `LdrInitializeThunk` runs before the first module call; `LoadLibraryA("kernel32.dll")` returns a non-NULL handle; by-name and by-ordinal `GetProcAddress` both resolve; forwarders resolve through the loader; `GetModuleFileNameW` returns the DLL path; `FreeLibrary` pairs with `LoadLibrary` and the reference count; `GetModuleHandleEx(FROM_ADDRESS)` finds the module; `GetLastError` is per thread (it reads `TEB.LastErrorValue`); the PEB has room for the slot.

**Open (not fixed here):** the main executable's exports are not in the loader list, so `GetProcAddress` on the main module fails. `DisableThreadLibraryCalls` stores the flag, but no thread notifications are sent (LDR-3). A directory in a `LoadLibrary` name is a search override only. ANSI conversion is Latin-1, not a code page. The kernel32 list walks read `PEB.Ldr` without the loader lock. `STATUS_DLL_INIT_FAILED` maps to 1114 here, but `ntstatus_to_dos.c` has no row for it (K32-18).

## Known gaps (see docs/review/ntdll.md)

- The module functions use the process loader context (K32-4, fixed in 7d00ac6 and 1e013ac). Their run-time behaviour is not verified; see Module APIs.
- `VirtualProtect` ignores its protection argument (K32-1), and `TlsAlloc` always returns slot 0 (K32-2). Both break callers that rely on them.
- `CreateFileW` swaps the CREATE_NEW and CREATE_ALWAYS dispositions (K32-3).
- `WaitForSingleObject` never waits (K32-5).
- `HeapAlloc` checks its size after rounding (K32-6).
- Most of the table above is placeholder code. Programs that use these calls will misbehave silently, not fail.
- `kernelbase.def` forwards 22 names plus the module APIs. They resolve through the loader, which is compiled (LDR-1 fixed in 10f42c8). Whether they run is not verified (see Module APIs).
- `DllMain` in each shim returns TRUE and does nothing. The shim DLLs have no initialisation.
