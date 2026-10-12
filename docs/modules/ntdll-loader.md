# Module: ntdll-loader (user-mode PE loader, `winldr`)

Sources: `src/ntdll/ldr.c`, `src/ntdll/ldr.h`
Tests: `src/ntdll/ldr_test.c` (Linux build only; not run in this review)
Related: `ntdll-startup.md` (the separate loader used by `LdrInitializeThunk` in the MinGW build), `src/loader/pe_parse.c` (PE parser used by this module), `docs/review/ntdll.md` (findings cited below).

## Purpose

`ldr.c` implements the `Ldr*` API of ntdll: load a DLL by name, find a loaded DLL, resolve an export by name or ordinal, unload. The design follows the Windows split, where the loader lives in user mode and the kernel only maps the first image. Import resolution, export forwarders, API-set names, delay-load imports, bound-import handling, DllMain ordering and TLS callbacks are all in scope in the header comment.

## Build reality

The file has two very different builds, selected by `__MINGW32__`:

- **Non-MinGW (Linux test build).** `ldr.c:82` to `ldr.c:1784` is the full implementation. It uses POSIX `open`, `mmap` and `pthread_mutex`. This is what `ldr_test.c` exercises.
- **MinGW (the ntdll target).** The whole implementation is excluded. Only the stubs at `ldr.c:1784-1809` are compiled. `LdrLoadDll` returns `STATUS_NOT_SUPPORTED`, `LdrGetDllHandle` returns `STATUS_DLL_NOT_FOUND`, and `winldr_ctx_create` returns NULL. The NT-syscall helpers (`ldr_read_file`, `ldr_alloc_image` under `__MINGW32__`) are dead code. See LDR-1.

The rest of this page describes the Linux build, because that is the only build where the code runs.

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

- **Lookup order** (`ldr_find_dll_path`): search path override, known DLL to system32, executable directory, system32, PATH entries, then the working directory. The name gets `.dll` appended when missing.
- **Load** (`ldr_load_dll_internal`): check the hash table, then the init stack for a cycle, then find and map the file, insert into the lists and hash *before* resolving imports so cycles resolve to the module being loaded, resolve imports (recursively loading dependencies), resolve delay imports, then run attach.
- **Mapping** (`ldr_map_and_load`): read the file, `pe_context_init` and `pe_validate`, reserve `image_size` rounded to the section alignment, copy headers and sections, apply relocations as a delta from the preferred base.
- **Exports** (`ldr_resolve_export`): by ordinal (`ordinal - Base`) or by name (a linear scan of `AddressOfNames`). Forwarders (`DLL.Name` or `DLL.#ord`) recurse up to depth 32.
- **API sets**: a built-in table of `api-ms-win-*` names mapped to `kernelbase.dll` or `ntdll.dll`, plus an optional text schema (`winldr_load_apiset_schema`). The schema file is ignored in the MinGW build.
- **Reference counts**: `LoadCount` is incremented on each load. `LdrUnloadDll` decrements it and frees the module at zero, after running process detach. Modules are not reference-checked against importers.

## Invariants and locking

- The `Ldr*` entry points, the configuration functions and the API-set functions take the recursive loader lock (`ldr_lock_acquire`). The hook setters and the module-list iterators (`winldr_first_module` and similar) do not.
- The init stack records modules whose attach is running. A re-entrant attach of the same module returns success without running it again.

## Known gaps and defects (see docs/review/ntdll.md)

- **Not built for the target** (LDR-1). The MinGW build has only stubs.
- **Import patch uses a file offset** (LDR-2). `pe_rva_to_offset` returns a file offset, but the patch writes to the mapped image. The IAT is not patched wherever raw and virtual layout differ.
- **No DllMain and no TLS callbacks run** (LDR-3). Only the test hooks are called. `EntryPoint` is never called.
- **TLS table read at the preferred-base VA** (LDR-4).
- **Relocation and section copy are not bounded by the image** (LDR-5, LDR-6).
- **Export names match case-insensitively** (LDR-7). Windows is case-sensitive.
- Smaller items: `ANSI_STRING` length ignored (LDR-8), a double free on one OOM path (LDR-9), a file buffer leak (LDR-10), an unbounded attach stack (LDR-11).
- Unloading does not check whether other modules still import the unloaded DLL.
