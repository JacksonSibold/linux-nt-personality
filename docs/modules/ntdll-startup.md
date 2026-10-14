# Module: ntdll-startup (process entry, import binding and DLL mapping in the MinGW build)

Sources: `src/ntdll/ntdll_main.c`
Related: `ntdll-loader.md` (the separate, Linux-tested loader in `ldr.c`), `ntdll-unwind.md` (`RtlAddFunctionTable` registers with the unwinder), `ntdll-except.md` (`KiUserExceptionDispatcher` calls `RtlDispatchException`), `docs/review/ntdll.md` (findings cited below).

## Purpose

`ntdll_main.c` is the code that runs in a MinGW-built ntdll before a Win32 program's `main`. It has three jobs:

1. `LdrInitializeThunk`: the process entry. It reads the PEB, walks the executable's import directory, maps each imported DLL from disk, binds the executable's IAT, calls the executable's TLS callbacks, and jumps to its entry point.
2. A small PE mapper, `ldr_nt_map_dll`, which loads a DLL file with NT system calls and applies its relocations and section protections.
3. Export lookup for the mapper's clients (`ldr_find_export`, `ldr_find_export_by_ordinal`), including forwarders.

The whole file is compiled only under `__MINGW32__`. The Linux test build does not see it.

## Public and exported entry points

- `LdrInitializeThunk(void)`: the process entry (listed in `ntdll.def`).
- `DllMain`, `_DllMainCRTStartup`, `DllMainCRTStartup`: ntdll's own DllMain, which returns 1 and does nothing.
- `KiUserExceptionDispatcher(rec, ctx)`: forwards to `RtlDispatchException`, and ignores the result.
- `KiUserApcDispatcher(...)`: empty. APCs are not dispatched in this build.
- `RtlGetVersion(OSVERSIONINFOEXW *)`: reports 10.0 build 26200, `VER_PLATFORM_WIN32_NT`, product type 1 (workstation). This is the accepted golden host identity (26200.9457), declared in `tests/conformance/os-identity-policy.json`; see `docs/decisions/0075-os-identity-golden-build.md`. It replaces the earlier server 2022 persona (build 20348), which is no longer used.
- `NtCurrentTeb(void)`: reads the TEB from `gs:0x30`.
- `RtlAddFunctionTable` and `RtlDeleteFunctionTable`: see Known gaps.

## Internal structures

- The loaded-module list is a fixed array of 32 `struct loaded_entry` (`g_loaded`), each with a 63-character name, a base pointer and a size. `add_loaded` refuses new entries when full. `find_loaded` compares names case-insensitively.
- `ntdll` itself is found by probing `0x180000000` with `NtQueryVirtualMemory` (`get_ntdll_base`), so the probe never faults.

## Behaviour as implemented

**`LdrInitializeThunk`.** Validates the executable's `MZ` and `PE` signatures (exit codes 1 to 3 on failure), registers ntdll in the loaded list, then for each import descriptor:
- finds the DLL in the list, or maps it. The DLL may be ntdll itself (`is_already_loaded_ntdll`). Otherwise the candidates are `/tmp/`, `/windows/system32/`, `/` and the bare name, tried in that order;
- for a newly mapped DLL, resolves its own import directory one level deep, mapping missing dependencies from the same candidate list, and writes their IAT slots (with `NtProtectVirtualMemory` around each write);
- calls the DLL's TLS callbacks and then its `DllMain` with `DLL_PROCESS_ATTACH`;
- patches the executable's IAT for that descriptor (by name, or by ordinal when the thunk has the high bit set), with the pages made writable and then restored to READONLY.

Finally it calls the executable's TLS callbacks, reads `AddressOfEntryPoint` from the optional header, and calls it. If it returns, the process exits with 0.

**`ldr_nt_map_dll`.** Opens the file with `NtCreateFile`, reads the first 0x1000 bytes into a committed page, checks `e_lfanew < 0x1000` and the `PE\0\0` signature, reserves `SizeOfImage` at the preferred base (falling back to any address), reads the headers and each section's raw data, zero-fills the rest of each section, applies DIR64 relocations (other types fail the load), sets header and section protections from the section characteristics, and frees the header page.

**Forwarders.** `ldr_find_export` scans names with a case-insensitive compare and returns the function address. A forwarder RVA inside the export directory is resolved through `ldr_resolve_forwarder`, which maps the host DLL on demand and looks up the target by name or `#ordinal`.

## Known gaps and defects (see docs/review/ntdll.md)

- **Writes are not bounded** (MAIN-1 for sections and headers, MAIN-2 for relocations). A DLL found on a candidate path can write outside its image.
- **Import tables are truncated silently** (MAIN-3): 257 entries for nested imports, 513 for the executable.
- **Forwarder recursion is unbounded** (MAIN-4).
- **Search order reaches the working directory and the root** (MAIN-5). This is a DLL-planting risk.
- **Missing imports leave the slot unpatched** (MAIN-6).
- **Dependencies are one level deep.** A DLL's dependencies' dependencies are not resolved here. This loader is not the same as `ldr.c`, and the two do not share state.
- **No reference counting or unload.** `LdrUnloadDll` is not implemented in this file. `RtlDeleteFunctionTable` returns TRUE without removing the table (MAIN-7).
- **Debug output** to the stdout handle on every process start (MAIN-9).
- **Resource leaks on error paths** (MAIN-8).
- `KiUserApcDispatcher` is empty, so APCs queued with `NtQueueApcThread` never run.
