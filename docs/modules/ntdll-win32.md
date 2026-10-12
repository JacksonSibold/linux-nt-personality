# Module: ntdll-win32 (Win32 and CRT shims: kernel32, kernelbase, user32, advapi32, msvcrt)

Sources: `src/ntdll/kernel32.c`, `src/ntdll/kernelbase.c`, `src/ntdll/user32.c`, `src/ntdll/advapi32.c`, `src/ntdll/msvcrt.c`, and the export lists `kernel32.def`, `kernelbase.def`, `user32.def`, `advapi32.def`, `msvcrt.def`, `ucrtbase.def`
Related: `ntdll-ntstatus.md` (`RtlNtStatusToDosError`, the source of `GetLastError` values), `ntdll-syscalls.md` (the `Nt*` calls these shims make), `ntdll-loader.md` (the `Ldr*` calls, see K32-4), `docs/review/ntdll.md` (findings cited below).

## Purpose

These DLLs are the Win32 surface that programs import. Each shim turns a Win32 call into one or more NT calls, and turns an NTSTATUS failure into a `GetLastError` value. Most of the code is a thin forwarder. A large part is also placeholder code that returns a fixed value, and that matters for correctness. The MinGW-only guard applies to each file.

## Layout

- `kernel32.c` (about 500 lines): file I/O, virtual memory, module loading (via `Ldr*`), heap, critical sections, TLS, synchronization, temp paths, file mapping, timing.
- `kernelbase.c`: a stub DLL with one dummy export. Its real surface is in `kernelbase.def`, which forwards 22 names to `kernel32` (for example `CreateFileW=kernel32.CreateFileW`). Forwards are resolved at load time by the loader, which is a stub in this build (LDR-1).
- `user32.c`: `wsprintf` family only.
- `advapi32.c`: registry stubs.
- `msvcrt.c`: the C runtime export surface (`malloc`, `printf`, `sprintf`, `_initterm`, `exit`, startup helpers). Heap calls go to kernel32's heap.
- The `.def` files list the exports. `ucrtbase.def` mirrors `msvcrt.def`.

## Error reporting

`SetLastError` writes `TEB.LastErrorValue`. `SetLastErrorFromStatus` converts an NTSTATUS with `RtlNtStatusToDosError` and stores the result. A success status sets 0. Most failing paths follow this pattern, but some paths set a fixed code without consulting the status, and a few return success without setting anything.

## Behaviour

**Files.** `CreateFileW` maps Win32 access and disposition to `NtCreateFile` (`kernel32.c:126`). `ReadFile` and `WriteFile` call `NtReadFile` and `NtWriteFile`. `CloseHandle` calls `NtClose`. `CreateFileA` widens the name and calls `CreateFileW`.

**Memory.** `VirtualAlloc` maps the protection constant to the NT value and calls `NtAllocateVirtualMemory`. `VirtualFree` calls `NtFreeVirtualMemory`. `VirtualQuery` calls `NtQueryVirtualMemory` with class 0.

**Modules.** `LoadLibraryW` and `LoadLibraryA` call `LdrLoadDll`. `GetModuleHandle*` calls `LdrGetDllHandle`. `GetProcAddress` calls `LdrGetProcedureAddress`. All of them pass a NULL loader context (K32-4).

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

## Known gaps (see docs/review/ntdll.md)

- The loader context is NULL for every module call (K32-4), so the module functions cannot succeed in this build (LDR-1).
- `VirtualProtect` ignores its protection argument (K32-1), and `TlsAlloc` always returns slot 0 (K32-2). Both break callers that rely on them.
- `CreateFileW` swaps the CREATE_NEW and CREATE_ALWAYS dispositions (K32-3).
- `WaitForSingleObject` never waits (K32-5).
- `HeapAlloc` checks its size after rounding (K32-6).
- Most of the table above is placeholder code. Programs that use these calls will misbehave silently, not fail.
- `kernelbase.def` forwards 22 names that resolve only through the loader (LDR-1).
- `DllMain` in each shim returns TRUE and does nothing. The shim DLLs have no initialisation.
