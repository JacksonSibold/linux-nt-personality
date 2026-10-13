# Module: ntdll-loader (user-mode PE loader, `winldr`)

Sources: `src/ntdll/ldr.c`, `src/ntdll/ldr.h`, `src/ntdll/ldr_pure.h` (IAT and TLS address logic), `src/ntdll/ldr_sched.h` (init order, reference counts, names, search order, API-set lines, the attach result rule)
Tests: `tests/ntdll-host/test_ldr_pure.c` and `test_ldr_sched.c` (host, run by `make -C tests/ntdll-host test`); `src/ntdll/ldr_test.c` (Linux build only; not run here); `tests/ntdll-host/check_ldr_stub.sh` (build check, run by `make -C src/ntdll check`)
Related: `ntdll-startup.md` (the separate loader used by `LdrInitializeThunk` in the MinGW build), `src/loader/pe_parse.c` (PE parser used by this module), `docs/review/ntdll.md` (findings cited below).

## Purpose

`ldr.c` implements the `Ldr*` API of ntdll: load a DLL by name, find a loaded DLL, resolve an export by name or ordinal, unload. The design follows the Windows split, where the loader lives in user mode and the kernel only maps the first image. Import resolution, export forwarders, API-set names, delay-load imports, bound-import handling, DllMain ordering and TLS callbacks are all in scope in the header comment.

## Build reality

The whole implementation is compiled in both builds. The platform layer is chosen by `LDR_USE_POSIX` (defined in `ldr.c`):

- **POSIX host layer** (`LDR_USE_POSIX`=1: any non-MinGW build, or a MinGW build with `LDR_HOST_POSIX`). POSIX `open`, `mmap` and `pthread_mutex`. This is what `ldr_test.c` exercises. Not run in this repository; the MinGW host build cannot compile it (no `sys/mman.h`), so it was only syntax-checked with stub headers.
- **NT layer** (`__MINGW32__`, the ntdll target). Spin-lock shims, `ldr_read_file` and `ldr_file_exists` through `NtCreateFile` and `NtReadFile`, and `ldr_alloc_image` and `ldr_free_image` through `NtAllocateVirtualMemory`. `winldr_ctx_create` sets `run_entry_points`, so DllMain and TLS callbacks are called through their function pointers. The image is committed and copied, not mapped with `SEC_IMAGE` (open).

The MinGW build links `src/loader/pe_parse.c` with `PE_FREESTANDING`. Its diagnostics do not use `stderr`. See LDR-1 and LDR-3 for what is still open.

## Public API (ldr.h)

- Context: `winldr_ctx_create`, `winldr_ctx_destroy`, `winldr_peb_ldr`.
- Configuration: `winldr_set_exe_dir`, `winldr_set_system32_dir`, `winldr_register_known_dll`, `winldr_load_apiset_schema`, `winldr_apiset_resolve`.
- Hooks for tests: `winldr_set_dll_main_hook`, `winldr_set_tls_callback_hook`, `winldr_get_init_log`, `winldr_clear_init_log`.
- Ldr API: `LdrLoadDll`, `LdrUnloadDll`, `LdrGetDllHandle`, `LdrGetProcedureAddress`, `ldr_resolve_delay_imports`.
- Introspection: `winldr_first_module`, `winldr_next_module`, `winldr_find_module`.

`LDR_DATA_TABLE_ENTRY` offsets are checked with `NT_STATIC_ASSERT` (ldr.h): `InLoadOrderLinks` at 0x00, `DllBase` at 0x30, `SizeOfImage` at 0x40, `Flags` at 0x68. `PEB_LDR_DATA` list heads are checked in `ldr.c`.

## Key data structures

- `ldr_module_t` (`ldr.c:~111`): embeds `LDR_DATA_TABLE_ENTRY entry`, the parsed `pe_context_t pe`, the file buffer and size, the mapped image and size, `loading`/`attached`/`datafile`/`pinned` flags, the TLS directory, and a 256-byte lowercase base name.
- `winldr_ctx` (`ldr.c:~141`): `PEB_LDR_DATA`, 256 hash buckets keyed by lowercase base name, search paths, known DLLs, the API-set table, hooks, the init log, a 64-bit TLS bitmap, a recursive lock (`lock_owner`, `lock_depth`), and `init_stack[64]`.

## Behaviour as implemented (Linux build)

- **Lookup order** (`ldr_find_dll_path`, using `ldr_dll_normalize` and `ldr_search_dirs`): search path override, known DLL to system32, executable directory, system32, PATH entries, then the working directory. The name is lower-cased and gets `.dll` appended when missing. Names with a path separator or a drive colon are rejected.
- **Load** (`ldr_load_dll_internal`): check the hash table, then the init stack for a cycle, then find and map the file, insert into the lists and hash *before* resolving imports so cycles resolve to the module being loaded, resolve imports (recursively loading dependencies), resolve delay imports, then run attach.
- **Mapping** (`ldr_map_and_load`): read the file, `pe_context_init` and `pe_validate`, reserve `image_size` rounded to the section alignment, copy headers and sections, apply relocations as a delta from the preferred base.
- **Exports** (`ldr_resolve_export`): by ordinal (`ordinal - Base`) or by name (a linear scan of `AddressOfNames`). Forwarders (`DLL.Name` or `DLL.#ord`) recurse up to depth 32.
- **API sets**: a built-in table of `api-ms-win-*` names mapped to `kernelbase.dll` or `ntdll.dll`, plus an optional text schema (`winldr_load_apiset_schema`). The schema file is read in both builds (the MinGW build through `ldr_read_file`, not yet run), and the built-in fallback is always added.
- **Reference counts**: `LoadCount` is incremented on each load. `LdrUnloadDll` decrements it and frees the module at zero, after running process detach. Modules are not reference-checked against importers.

## Invariants and locking

- The `Ldr*` entry points, the configuration functions and the API-set functions take the recursive loader lock (`ldr_lock_acquire`). The hook setters and the module-list iterators (`winldr_first_module` and similar) do not.
- The init stack records modules whose attach is running. A re-entrant attach of the same module returns success without running it again.

## Known gaps and defects (see docs/review/ntdll.md)

- **Built for the target** (LDR-1, fixed in 10f42c8). The MinGW build compiles the real loader. Win32 callers now use the process loader context published in the PEB (K32-4, fixed in 7d00ac6 and 1e013ac). The run-time behaviour of `LoadLibrary` is not verified on the personality (see `ntdll-win32.md`). `LdrAddRefDll` adds or pins a reference for `GetModuleHandleEx`.
- **Import patch uses a file offset** (LDR-2). `pe_rva_to_offset` returns a file offset, but the patch writes to the mapped image. The IAT is not patched wherever raw and virtual layout differ.
- **DllMain and TLS callbacks** (LDR-3, partly fixed). The MinGW build calls them through function pointers; FALSE from attach fails the load (`STATUS_DLL_INIT_FAILED`). Still open: the dependencies of a failed module are not released, and the attach order is still the recursion order, not `ldr_sched_attach_order`. The host never runs them.
- **TLS table read at the preferred-base VA** (LDR-4).
- **Relocation and section copy are not bounded by the image** (LDR-5, LDR-6).
- **Export names match case-insensitively** (LDR-7). Windows is case-sensitive.
- Smaller items: `ANSI_STRING` length ignored (LDR-8), a double free on one OOM path (LDR-9), a file buffer leak (LDR-10), an unbounded attach stack (LDR-11).
- Unloading does not check whether other modules still import the unloaded DLL.
