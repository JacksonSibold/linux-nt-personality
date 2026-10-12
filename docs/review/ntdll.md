# Review: src/ntdll (user-mode loader, exception dispatch, unwind, runtime, Win32 shims)

Scope: `src/ntdll/*.c`, `src/ntdll/*.h`, `src/ntdll/syscall_stub.S`, `src/ntdll/syscall_table.def`, and the `*.def` export lists.
Base commit for line references: `7e75069` (branch `haiku/ntdllrev`). Line numbers refer to that commit unless a finding says otherwise.
Method: read-only static review, plus host builds and tests of the four pure files (`ntstatus_to_dos.c`, `unwind.c`, `except.c`, `ntdll_runtime.c`) under `tests/ntdll-host/`. Every finding below was checked against the cited lines. A finding marked "reproduced" was also shown by a host test that fails at the base commit.

Severity: High / Medium / Low / Info.
Status: findings marked `Fixed in <sha>` were fixed in this branch, with a failing test committed before the fix.

## Summary

| Severity | Count |
|---|---|
| High | 20 |
| Medium | 23 |
| Low | 21 |
| Info | 7 |
| **Total** | **71** |

Fixed in this pass: NTS-1 (High), RT-1 (High), EXC-1 (High), UNW-1 (Medium).

The most important point is LDR-1. The real ntdll (MinGW) build compiles only stubs for `LdrLoadDll`, `LdrGetDllHandle` and `LdrGetProcedureAddress`, so every Win32 `LoadLibrary`, `GetModuleHandle` and `GetProcAddress` call fails. The loader code that would do the work is excluded by a preprocessor branch and is never built for the MinGW target. The findings in the loader are therefore mainly about code that runs in the Linux test build, or that would run once LDR-1 is fixed.

## Findings

### Loader (`ldr.c`, `ldr.h`)

### LDR-1 [High] MinGW build compiles only stubs for the loader API
- `src/ntdll/ldr.c:82` opens `#ifndef __MINGW32__`, which contains the whole implementation. `ldr.c:1784` is the matching `#else`, and lines 1784-1809 are stubs. Under `__MINGW32__`, `LdrLoadDll` returns `STATUS_NOT_SUPPORTED` (`ldr.c:1789`), `LdrGetDllHandle` returns `STATUS_DLL_NOT_FOUND` (`ldr.c:1791`), `winldr_ctx_create` returns NULL (`ldr.c:1794`). The `__MINGW32__` file-helper branches at `ldr.c:623-818` and `ldr.c:1541-1553` are inside the excluded block, so they are dead code for the target.
- Callers in `kernel32.c` (lines 225, 247, 263, 271) pass a NULL context, which is a second failure (K32-4).
- Fix: split the platform choice so the NT-syscall implementation is compiled for `__MINGW32__` and the POSIX one only for host tests. Create one process-wide loader context at startup.

### LDR-2 [High] Import address table patched at a file offset, not an RVA
- `ldr.c:1326` computes `iat_off` with `pe_rva_to_offset(FirstThunk)`, and `pe_rva_to_offset` returns a file offset (`src/loader/pe_parse.c:730-766`). `ldr.c:1358` then writes `memcpy(mod->image + iat_slot_off, ...)`, where `mod->image` is the RVA-laid-out image. The IAT is therefore not patched wherever file and virtual layout differ, and the write lands on unrelated bytes. The delay-import path has the same mix at `ldr.c:1428` and `ldr.c:1456`.
- Fix: keep the RVA (`FirstThunk + t * 8`) and write to `mod->image + rva`. Read thunks from the file with the file offset, as now.

### LDR-3 [High] DllMain and TLS callbacks are never called
- `ldr.c:1491-1499` (`ldr_run_dll_main`) only calls the optional test hook. `ldr.c:1467-1486` (`ldr_run_tls_callbacks`) only logs and calls the hook. `mod->entry.EntryPoint` is set at `ldr.c:944` and never called anywhere in the file.
- Effect: no DLL initialisation runs in the Linux build, and none is planned for the MinGW build (LDR-1).
- Fix: call each TLS callback, then `DllMain(hinst, DLL_PROCESS_ATTACH, NULL)`, in the documented order. Keep the hook as an extra observer for tests.

### LDR-4 [High] TLS callback table dereferenced at its preferred-base address
- `ldr.c:1481`: `ULONG_PTR *table = (ULONG_PTR *)cb_table_va;` uses `mod->tls.AddressOfCallBacks`. That value was read from the file before relocation (in `ldr_map_and_load`, `pe_tls_directory(&mod->pe, ...)`), so it is the unrelocated VA. For any image not loaded at its preferred base, the read goes to the wrong address.
- Fix: convert to an image RVA (`cb_va - OriginalBase`) and read from `mod->image`, or read the TLS directory from the mapped image after relocation.

### LDR-5 [High] Base relocations write without an image bound
- `ldr.c:855`: `u8 *addr = base + block.VirtualAddress + offset;` followed by an 8- or 4-byte write. Nothing checks `block.VirtualAddress + offset + size <= mod->image_size`. A crafted `VirtualAddress` writes outside the image mapping.
- Related: `pe_file_u16(ctx, off)` (`ldr.c:847`) is an unchecked reader (see `pe_parse.h`), and `n` is derived from `SizeOfBlock` without a bound against the directory size or file size.
- Fix: check each entry's target against `image_size` and each read against the file size. Or drive relocations through `pe_relocs_walk`, which already checks the directory against the file (`src/loader/pe_parse.c:1179`).

### LDR-6 [High] Section copy length is SizeOfRawData, not bounded by the mapping
- `ldr.c:890`: `memcpy(va, mod->file_data + sv.file_offset, sv.file_size);` where `sv.file_size` is `SizeOfRawData` (`pe_parse.c:726`).
- `pe_validate` checks only `VirtualSize` against `SizeOfImage` (`pe_parse.c:506`). A section whose raw size is larger than its virtual size writes past the section, and for the last section past the end of the mapping.
- Fix: copy `min(SizeOfRawData, VirtualSize)` and require `VirtualAddress + VirtualSize <= image_size` before copying.

### LDR-7 [Medium] Export names matched case-insensitively
- `ldr.c:1193`: `ldr_strcasecmp(sym, name) == 0`. Windows export lookup and import binding are case-sensitive, so a request for a name that differs only in case binds to the wrong export.
- Fix: use `strcmp`.

### LDR-8 [Low] LdrGetProcedureAddress ignores ANSI_STRING.Length
- `ldr.c:1765` onward uses `name->Buffer` as a NUL-terminated string. An `ANSI_STRING` is not required to be terminated, so this over-reads.
- Fix: copy `Length` bytes into a terminated buffer, or pass the length to the export search.

### LDR-9 [Low] Double free when the wide base-name allocation fails
- `ldr.c:969-977`: `FullDllName.Buffer` is set to `wname` before `wbase` is allocated. The branch `if (!wbase) { free(wname); goto fail; }` (`ldr.c:977`) then frees `wname` again, because `fail:` frees `FullDllName.Buffer`.
- Fix: remove the explicit `free(wname)` in that branch.

### LDR-10 [Low] File buffer leaked when image allocation fails
- `ldr.c:935-938`: `if (!mod->image) { free(mod); return ...; }` frees the module but not `mod->file_data`.
- Fix: free `file_data` before returning.

### LDR-11 [Low] Process-attach stack has no bound
- `ldr.c:1509`: `ctx->init_stack[ctx->init_stack_depth++] = mod;` into a 64-entry array (`ldr.c:183`). Nested attach from a `DllMain` that loads further DLLs can write past the array, which is part of the context struct.
- Fix: refuse the attach when `init_stack_depth == 64`.

### LDR-12 [Info] Unused or dead code in the loader
- `ldr.c:631` and `ldr.c:713` map a `/tmp/` prefix to a CWD-relative path in the MinGW file helpers, which is dead (LDR-1). `ldr.c:1115` declares `wdll` and fills it, but never uses it. `ldr_resolve_thunk` has an empty hint branch. `ldr_read_file` treats a short read as a complete file.
- Fix: delete the dead paths when LDR-1 is fixed.

### Startup and runtime path (`ntdll_main.c`)

### MAIN-1 [High] Section table and section copy are not bounded
- `ntdll_main.c:231` computes `sec_off = section_table_off + i*40` for `i < num_sections` (up to 65535). There is no check against the 0x1000-byte header buffer, so a large section count reads past the page.
- `ntdll_main.c:238` sets `dst = img_base + vaddr`, and `ntdll_main.c:241` reads `rawsize` bytes into `dst`. Nothing checks `vaddr + rawsize <= size_image`, or `rawptr + rawsize` against the file size.
- This is the live loader for the MinGW build (`LdrInitializeThunk`, `ntdll_main.c:551`). A DLL found on a candidate path (MAIN-5) reaches it.
- Fix: validate the section table within the header, and each section against `SizeOfImage` and the file size, before reading.

### MAIN-2 [High] Relocations write without an image bound
- `ntdll_main.c:270-272`: `if (type!=10)` accepts DIR64 only, then `*p += delta` for `p = img_base + page_rva + off2`. `page_rva` comes from the block and is not checked against `size_image`.
- Fix: as LDR-5.

### MAIN-3 [Medium] Import thunk count silently capped
- `ntdll_main.c:661` (nested dependency imports) stops counting at 257 entries (`if(cnt>256) break;` after `cnt++`). `ntdll_main.c:700` (executable imports) stops at 513. The patch loops then use the capped count, so later IAT slots stay as ILT values and the first call through one jumps to a bogus address.
- Fix: count to the terminating zero thunk, and fail the load if the table is larger than the buffer allows.

### MAIN-4 [Medium] Forwarder resolution recurses without a depth limit
- `ntdll_main.c:330` (`ldr_resolve_forwarder`) and `ntdll_main.c:389` (`ldr_find_export`) call each other. A forwarder cycle recurses until the stack overflows. The MinGW-excluded loader caps depth at 32 (`ldr.c` `ldr_resolve_export`), but this path has no cap.
- Fix: pass a depth counter and stop at 16.

### MAIN-5 [Medium] DLL search reaches the working directory and the root
- `ntdll_main.c:360`, `:603` and `:646` try `"/tmp/"`, `"/windows/system32/"` and `"/"` before the bare name. The bare-name fallback (`ntdll_main.c:615` onward, `ldr_nt_map_dll(dll_name, ...)`) resolves relative to the current directory. A DLL placed in the working directory is loaded ahead of system32 (CWE-426).
- Fix: load only from the system directory, with no bare-name or root fallback.

### MAIN-6 [Low] Missing import leaves the IAT slot unpatched and the load continues
- `ntdll_main.c:740`: `dbg_print("Ldr: proc not found\n");` is the only handling. The slot keeps its ILT value, and the first call jumps to that value.
- Fix: fail process start with a clear status.

### MAIN-7 [Low] RtlDeleteFunctionTable does not unregister the table
- `ntdll_main.c:798-801` returns TRUE and leaves the entry in the unwind registry, so the table is still searched after deletion.
- Fix: remove the entry from the registry in `unwind.c`.

### MAIN-8 [Low] Header and image leaks on error paths; wrong MEM_RELEASE size
- `ntdll_main.c:316`: `NtFreeVirtualMemory(..., &tmp_sz, 0x8000)` passes a nonzero `RegionSize` with `MEM_RELEASE`. Windows requires `RegionSize == 0` for that flag, so the header page is not released. The early-error returns in `ldr_nt_map_dll` do not free the header page or the image.
- Fix: pass size 0 with `MEM_RELEASE`, and free on each error path.

### MAIN-9 [Info] Debug output to the console for every import
- `ntdll_main.c:552` and the `dbg_print` calls through `ntdll_main.c:740` write module and symbol names to the stdout handle on every process start.
- Fix: remove, or gate behind a build flag.

### Unwind (`unwind.c`, `unwind.h`)

### UNW-1 [Medium] Tables registered with size 0 are never searched (reproduced; Fixed in a09f0a3)
- `ntdll_main.c:796` (`RtlAddFunctionTable`) registers with `sizeOfImage = 0`. `unwind.c:115` (`lookup_in_module`) rejected every PC for a zero size, so dynamically added tables were not found.
- Test: `tests/ntdll-host/test_unwind.c`, `zero_size_module_finds_function`.
- Fix applied: zero means unknown, and the table bounds the match. A 4 GiB guard on the RVA is added.

### UNW-2 [High] RtlUnwindEx runs no unwind handlers and does not transfer control
- `unwind.c:738-770`: the handler returned by `RtlVirtualUnwind` is discarded at `unwind.c:754` (`(void)handler;`). No termination or unwind handler is called. `TargetIp` is never jumped to: the function writes the walked context back to the caller, which then continues as if it had returned normally.
- The header (`unwind.h`) and `ntdll.def:354` export `RtlUnwindEx` as implemented.
- Fix: implement the second pass. Call each UNW_FLAG_UHANDLER handler in order, then restore the context with `TargetIp` in `Rip`.

### UNW-3 [Medium] RtlVirtualUnwind ignores HandlerType
- `unwind.c:615`: `(void)HandlerType;`. The function returns a handler for either UNW_FLAG_EHANDLER or UNW_FLAG_UHANDLER. An exception filter (EHANDLER) is therefore returned during unwind and runs as a termination handler.
- Fix: return the handler only when its flag matches the requested `HandlerType`.

### UNW-4 [Low] RUNTIME_FUNCTION.EndAddress masked with ~3
- `unwind.c:121`: `ULONG end = mod->funcTable[mid].EndAddress & ~0x3U;`. The x64 `RUNTIME_FUNCTION` has no flag bits (ARM64 does). The mask moves the end down by up to 3 bytes, so the last bytes of a function fail the range check.
- Fix: remove the mask. Confirm against the PE/COFF specification first, since this review did not consult the text.

### UNW-5 [Low] UNWIND_INFO and chain reads are not bounds-checked
- `unwind.c:187` (`build_chain`) and `unwind.c:153` (`get_unwind_info`) read `UnwindInfoAddress`, `Flags` and `CountOfCodes` with no check against the module size. Only trusted images are unwound today.
- Fix: check each RVA and count against `sizeOfImage` (when known).

### Exception dispatch (`except.c`, `except.h`)

### EXC-1 [High] Vectored handler EXCEPTION_CONTINUE_EXECUTION ignored (reproduced; Fixed in 655c9ca)
- `except.c:473` (base): `if (ret == 1 ...)`. Vectored handlers return -1 to resume (`EXCEPTION_CONTINUE_EXECUTION`), so a handler that repaired the fault was ignored and dispatch kept searching.
- Test: `tests/ntdll-host/test_except.c`, `veh_continue_execution_stops_dispatch`.
- Fix applied: compare with -1.

### EXC-2 [Medium] Frames without RUNTIME_FUNCTION cannot be unwound
- `except.c:488-496`: when the lookup fails, `imgBase` stays 0 and `RtlVirtualUnwind(0, 0, ...)` returns -1 without changing the context (`unwind_one_frame` finds neither a module nor an image). The loop runs 128 times and stops, so dispatch cannot pass a leaf frame.
- Fix: for a PC with no entry, apply the leaf rule (pop the return address from RSP) as `unwind_one_frame` already does for `entry == NULL`.

### EXC-3 [Medium] No second pass: termination handlers never run
- `except.c:543-550` documents that the unwind pass is not implemented, so `__finally` and cleanup code never runs for a dispatched exception. `RtlUnwindExWrapper` (`except.c:594`) is a partial implementation with no caller (EXC-6).
- Fix: implement the unwind pass once, shared with UNW-2.

### EXC-4 [Low] Dispatch state is global and unsynchronised
- `except.c:456` (`g_nested_depth`), `except.c:41-42` (`g_veh_head`, `g_vch_head`) and the registration functions are not protected by a lock. `except.c:449` declares `t_in_dispatch` `__thread` for non-MinGW builds only; the MinGW build shares one flag across threads.
- Fix: per-thread dispatch state (for example in the TEB) and a lock around the handler lists.

### EXC-5 [Low] A handler removed during dispatch can be freed while in use
- `except.c:68-80` (`RemoveVectoredExceptionHandler`) frees the node. The dispatch loop at `except.c:471-472` reads `n->next` after calling the handler, so a handler that removes itself is a use-after-free.
- Fix: defer the free, or copy the list under a lock.

### EXC-6 [Low] RtlUnwindExWrapper is dead code with the wrong handler type
- `except.c:594-657` is not referenced from `ntdll.def` or any other file. It passes `1` as HandlerType where `UNW_FLAG_UHANDLER` is `0x2` (`unwind.h`).
- Fix: delete it, or fold it into the unwind pass from EXC-3.

### EXC-7 [Info] Exit status from exception code
- `except.c:568` (`_NtRaiseException`) exits with `128 + (ExceptionCode & 0xFF)`, not the NTSTATUS. Not a defect in isolation, but it loses information.

### Runtime and allocator (`ntdll_runtime.c`)

### RT-1 [High] calloc and malloc accept sizes that wrap (reproduced; Fixed in 9c733c3)
- `ntdll_runtime.c:154` (base): `calloc` multiplied `n * sz` without a check, so a wrapped product returned a short block for a huge request. `ntdll_runtime.c:144` (base): `malloc` rounded `sz` up to 16 before the heap-bound check, so a size near SIZE_MAX wrapped to a small block.
- Test: `tests/ntdll-host/test_runtime.c`, `calloc_rejects_multiplication_overflow` and `malloc_rejects_huge_request`. Both failed at the base commit, and the test binary must be built with `-fno-builtin` (see the Makefile).
- Fix applied: reject `n > SIZE_MAX / sz` and any size above `HEAP_SIZE` before rounding.

### RT-2 [Medium] realloc copies a block whose size is unknown
- `ntdll_runtime.c:160-170`: `memcpy(np, ptr, sz)` copies the new size from the old block. When the new size is larger, it reads past the old allocation. The bump allocator stores no sizes, so the copy length cannot be bounded.
- Fix: store the block size in a header before each allocation, and copy `min(old, new)`.

### RT-3 [Low] printf-family functions are silent stubs
- `ntdll_runtime.c:126` (`snprintf`) writes an empty string and returns 0. `ntdll_runtime.c:193` (`printf`) prints nothing. Callers get no indication of failure.
- Fix: implement, or fail at link time, so a caller cannot silently lose output.

### RT-4 [Low] errno definition clashes with MinGW's _errno declaration
- `ntdll_runtime.c:184`: `int errno;` and `int *_errno(void)` conflict with the dllimport declaration in MinGW's headers. Under `-Werror` this is a hard error on a MinGW host (`tests/ntdll-host/Makefile` relaxes `-Wattributes` for this object only).
- Fix: use the CRT's `_errno`, or drop the definition.

### RT-5 [Info] Allocator never reuses memory
- `ntdll_runtime.c:153` (`free`) is a no-op on a 2 MB bump heap shared by the loader, unwinder and exception code. Long-running processes will exhaust it.
- Fix: a real allocator, or a stated limit.

### RT-6 [Low] _exit spins on hlt, a privileged instruction
- `ntdll_runtime.c:199` (base): `void _exit(int c){ (void)c; while(1) { __asm__ volatile("hlt"); } }`. `hlt` is privileged, so in user mode it faults rather than exiting. The exit status is also discarded.
- Fix: call `NtTerminateProcess(NtCurrentProcess(), c)`, as `ExitProcess` does before its `hlt` fallback (`kernel32.c:172`).

### C runtime (`msvcrt.c`)

### CRT-1 [High] sprintf and vsprintf write without a size
- `msvcrt.c:273-279`: `sprintf` and `vsprintf` format into the caller's buffer with a size of 8192 (`my_vsprintf(s, 8192, ...)`), whatever the buffer really is. Any caller with a smaller buffer overflows it.
- Fix: remove these, or require callers to use `snprintf`.

### CRT-2 [Medium] snprintf writes a NUL even when the size is zero
- `msvcrt.c:206`: `out[pos]='\0';` runs after the loop, so `snprintf(NULL, 0, fmt)` (the standard size query) writes to address 0. The function also returns the number written, not the length it would need.
- Fix: write the NUL only when `outsz > 0`, and return the required length.

### CRT-3 [High] calloc multiplies without an overflow check
- `msvcrt.c:85-86`: `SIZE_T tot=n*s;`. The same defect as RT-1, in the msvcrt copy. Not fixed in this pass, because `msvcrt.c` is a separate DLL.
- Fix: as RT-1.

### CRT-4 [Medium] realloc over-reads and frees the block on allocation failure
- `msvcrt.c:91-96`: `memcpy(np, p, s)` copies the new size from the old block (as RT-2). On `malloc` failure, `free(p)` at `msvcrt.c:96` still runs, so the original data is lost. The C standard requires the original block to stay valid.
- Fix: store block sizes, copy `min(old, new)`, and return NULL without freeing on failure.

### CRT-5 [Medium] Floating-point formats print 0.0
- `msvcrt.c:195`: `// simplified: output "0.0"`. `%f`, `%e`, `%g` and their upper-case forms always print `0.0`.
- Fix: implement float formatting, or refuse the conversion.

### CRT-6 [Medium] __C_specific_handler returns ExceptionContinueExecution
- `msvcrt.c:300`: returns 0, which is `ExceptionContinueExecution` in `except.h`. Routed here, any exception resumes at the faulting instruction and can loop. For "no matching handler" the correct value is `ExceptionContinueSearch` (1).
- Fix: return 1 when no handler matches.

### CRT-7 [Low] atexit and signal are stubs
- `msvcrt.c:78` (`atexit`) discards the function and returns 0, so exit handlers never run. `msvcrt.c:128` (`signal`) installs nothing.
- Fix: keep a handler table and run it from `exit`.

### Win32 shim: kernel32 (`kernel32.c`)

### K32-1 [High] VirtualProtect ignores the requested protection
- `kernel32.c:210` (base): `ULONG ntProt=4;` and `newProt` is never used. Every `VirtualProtect` call sets PAGE_READWRITE, so code pages become writable and lose execute.
- Fix: map `newProt` the way `VirtualAlloc` does (`kernel32.c:180-193`).

### K32-2 [High] TlsAlloc always returns index 0
- `kernel32.c:314` (base): `DWORD WINAPI TlsAlloc(void) { return 0; }`. Every TLS user shares slot 0.
- Fix: allocate from a bitmap of `TlsSlots` in the TEB, and free it in `TlsFree`.

### K32-3 [High] CreateFileW maps two creation dispositions to the wrong NT values
- `kernel32.c:145`: `CREATE_NEW` maps to `disp=1` (FILE_OPEN), but its comment says FILE_CREATE. FILE_CREATE is 2. `kernel32.c:146`: `CREATE_ALWAYS` maps to `disp=2` (FILE_CREATE), but its comment says FILE_OVERWRITE_IF, which is 5.
- Effect: CREATE_NEW opens an existing file instead of failing, and CREATE_ALWAYS fails on an existing file.
- Fix: CREATE_NEW to 2, CREATE_ALWAYS to 5.

### K32-4 [High] Every loader call passes a NULL context
- `kernel32.c:225` (`LdrGetProcedureAddress`), `:247` and `:263` (`LdrGetDllHandle`), `:271` (`LdrLoadDll`) all pass `NULL` as the context. The loader rejects a NULL context (`ldr.c` `LdrLoadDll`, `LdrGetProcedureAddress`). In the MinGW build the stubs fail regardless (LDR-1).
- Effect: `GetProcAddress`, `GetModuleHandle*`, `LoadLibrary*` cannot succeed.
- Fix: a process-wide loader context, created during startup.

### K32-5 [High] WaitForSingleObject returns success without waiting
- `kernel32.c:464` (base): `{ (void)h;(void)ms; return 0; /* WAIT_OBJECT_0 */ }`. Callers proceed as though the object were signalled.
- Fix: call `NtWaitForSingleObject`.

### K32-6 [High] HeapAlloc rounds the size before checking it
- `kernel32.c:294` (base): `size = (size+15)&~15;` runs before the bounds check on `g_k32_off`. A size near SIZE_MAX wraps to a small block (same class as RT-1).
- Fix: reject `size` above the heap size before rounding.

### K32-7 [Medium] Events, mutexes and semaphores are fixed handles
- `kernel32.c:454-459`: `CreateEvent`, `CreateMutex` and `CreateSemaphore` return the constants 10, 11 and 12 for every object. `SetEvent`, `ReleaseMutex` and `ReleaseSemaphore` do nothing.
- Fix: create real kernel objects.

### K32-8 [Medium] File-system operations report false success or always fail
- `kernel32.c:392-398`: `DeleteFileA` and `DeleteFileW` always set `ERROR_FILE_NOT_FOUND` and return FALSE. `kernel32.c:399-402`: `CreateDirectoryA/W` and `RemoveDirectoryA/W` return TRUE without doing anything.
- Fix: implement with `NtCreateFile`, `NtSetInformationFile` and `NtDeleteFile`, or fail with `ERROR_CALL_NOT_IMPLEMENTED`.

### K32-9 [Medium] GetTempFileNameA uses a fixed, predictable path with an unbounded copy
- `kernel32.c:422-427` (base): always writes `/tmp/tmpfile.tmp` with `strcpy(out, name)`. The name is predictable (CWE-377), ignores the prefix and `uniq`, and the copy has no size limit.
- Fix: generate a unique name in `dir`, honour `uniq`, and bound the copy by the buffer size.

### K32-10 [Medium] QueryPerformanceCounter returns a constant
- `kernel32.c:473` (base): returns 123456 every time. Elapsed time is always zero, so timing loops never finish.
- Fix: read a monotonic clock.

### K32-11 [Medium] SetFilePointer does not move the file pointer
- `kernel32.c:451` (base): returns 0 and changes nothing.
- Fix: `NtSetInformationFile` with `FilePositionInformation`.

### K32-12 [Medium] File mappings return a fake handle, and views fail
- `kernel32.c:440-448`: `CreateFileMapping` returns the handle 1 and reports success. `MapViewOfFile` fails with `ERROR_OUTOFMEMORY`.
- Fix: `NtCreateSection` and `NtMapViewOfSection`.

### K32-13 [Medium] DuplicateHandle copies the value, not the object
- `kernel32.c:466-468` (base): `if(dst) *dst=src; return 1;`. The target gets the same handle value, which in the source process names something else.
- Fix: `NtDuplicateObject`.

### K32-14 [Medium] Critical sections are no-ops
- `kernel32.c:306-311` (base): `EnterCriticalSection` and `LeaveCriticalSection` do nothing. `TryEnterCriticalSection` always returns TRUE. Multi-threaded code races.
- Fix: a real lock, for example a keyed event or a futex wrapper.

### K32-15 [Low] CreateFileW passes a Win32 path as an NT name, with unchecked length
- `kernel32.c:126-131` (`CreateFileW`): the name is used without an `\??\` prefix, and `us.Length = len*sizeof(WCHAR)` is a USHORT with no check against 65535.
- Fix: convert to an NT path and bound the length.

### K32-16 [Low] VirtualAlloc maps unknown protections to PAGE_READWRITE
- `kernel32.c:180-193` (`VirtualAlloc`): `default: ntProt=4;` silently replaces PAGE_WRITECOPY and other values.
- Fix: fail with `ERROR_INVALID_PARAMETER`.

### K32-17 [Info] Placeholder return values
- `kernel32.c:363-365`: `GetExitCodeProcess` and `GetExitCodeThread` return 0 and TRUE for any handle. `GetPriorityClass` returns a constant.
- Fix: return real values, or fail.

### Win32 shim: user32 (`user32.c`) and advapi32 (`advapi32.c`)

### U32-1 [Medium] wsprintfW reads %s as a narrow string
- `user32.c:37`: `%s` reads `const char *`. In `wsprintfW`, `%s` is `LPCWSTR`, and `%S` (`user32.c:40`) is a branch with no body.
- Fix: `%s` as wide, `%hs` as narrow, and implement `%S` as in the Win32 documentation.

### U32-2 [Medium] wvsprintfA and wvsprintfW return 0 and write nothing
- `user32.c:115-121` (base): both are stubs that return 0.
- Fix: share one formatter with `wsprintf` that takes a `va_list`.

### U32-3 [Low] wsprintfA does not implement %c
- `user32.c` `wsprintfA` (base lines ~94-120): an unrecognised conversion copies `%` and the letter. `%c` is therefore printed literally.
- Fix: implement `%c`.

### U32-4 [Info] No output bound in the wsprintf family
- Same contract as Win32, where callers must size the buffer. Noted because the callers in this tree are not checked.

### ADV-1 [Low] Registry functions always report "not found"
- `advapi32.c:21` onward: `RegOpenKeyExA/W` and `RegQueryValueExA/W` return `ERROR_FILE_NOT_FOUND`. Installers that check for an existing key will misbehave.
- Fix: back the calls with the object manager registry, or document the limit.

### Exports (`*.def`)

### DEF-1 [Low] _DllMainCRTStartup@12 export name is an x86 decoration
- `ntdll.def:372` and the other `.def` files export `_DllMainCRTStartup@12`. `ntdll_main.c:36` defines `_DllMainCRTStartup` without a decoration, and x64 builds do not decorate `__stdcall` names. The export names a symbol that is not defined, which the link may reject. Not linked here, so this is unverified.
- Fix: export `_DllMainCRTStartup` (without `@12`), or drop the entry.


### Status codes (`ntstatus.h`, `ntstatus_to_dos.c`)

### NTS-1 [High] STATUS_DLL_NOT_FOUND had the wrong value (reproduced; Fixed in 08920c8 and d855c17)
- `ntstatus.h` (base line 580): `STATUS_DLL_NOT_FOUND` was `0xC0000228`. The table maps that value to 599, so a failed `LoadLibrary` reported `GetLastError` 599 instead of `ERROR_MOD_NOT_FOUND` (126). The golden column in `tests/differential/DISCREPANCIES.md` gives 0xC0000135 -> 126 for DLL not found.
- Similarly `STATUS_ENTRYPOINT_NOT_FOUND` was `0xC0000233` (base line 584), which maps to 1908 instead of `ERROR_PROC_NOT_FOUND` (127).
- Test: `tests/ntdll-host/test_ntstatus_to_dos.c`, `loader_symbols_map_to_win32_errors`. It failed at the base commit (599 and 1908).
- Fix applied: header values set to `0xC0000135` and `0xC0000139`. Two table rows now use literal values (`d855c17`), so the mapping does not depend on the symbol.

### NTS-2 [Info] Stale differential report and mislabelled header names
- `tests/differential/DISCREPANCIES.md` section 3 lists the "ours" column from an older table: 0xC0000002 -> 120, 0xC0000139 -> 1015, and STATUS_TIMEOUT missing. The current table gives 1, 127, and 1460, which match the golden column. The report also names 0xC0000139 `STATUS_REGISTRY_CORRUPT`, and `ntstatus.h` keeps that name for the same value, which is the entry-point status. `STATUS_INVALID_IMAGE_WIN_16` shares 0xC0000135 with the DLL status.
- Fix: refresh the report, and rename or remove the two header symbols. Not changed here, because their correct values are not established by this review.

## Not verified / out of scope

- Nothing in this review was built for the MinGW target or run under Windows. The ntdll link, the `.def` exports and the loader's live path were checked by reading only.
- `ldr.c` cannot be built in this environment, because its Linux implementation needs POSIX headers that the MinGW host lacks. The LDR-2 to LDR-11 findings come from reading the code paths and the parser they call (`src/loader/pe_parse.c`).
- `tests/differential` golden JSON (`probe_ntstatus.json`) is not in the repository. The golden column in `DISCREPANCIES.md` was used as the reference, and it was not re-run.
- `ldr_test.c` and `except_test.c` were not reviewed beyond their size and their own TODO notes (`except_test.c:609-610`).
- `ntdll.def` exports were checked against source definitions for non-syscall names only. The `Nt*` and `Zw*` stubs come from the generated macro in `syscall_stub.S` and were not checked one by one. The SSNs in `syscall_table.def` were not verified against any reference.
- The unwind chain semantics (`UNW_FLAG_CHAININFO` ordering in `build_chain`) were not verified against the x64 unwind specification.
- No sanitizer was run. The host tests were built without AddressSanitizer or UBSan, because the MSYS2 UCRT64 gcc ships no runtime for them.

## Coverage

- **Read in full:** `ldr.c` (1809 lines), `ntdll_main.c` (802 lines), `ntdll_runtime.c` (203 lines), `kernel32.c` (503 lines), `msvcrt.c` (327 lines), `user32.c` (122 lines), `advapi32.c`, `kernelbase.c`, `unwind.c` (772 lines, with the lookup, UWOP decoding and dispatch sections read closely), `except.c` (dispatch, vectored handlers, unwind wrapper, signal section skimmed).
- **Read for structure and the cited lines:** `ldr.h`, `except.h`, `unwind.h`, `ntstatus_to_dos.c` (table sortedness and golden values checked by test), `nt_types.h` and `ntstatus.h` (value and name checks only), `syscall_stub.S`, `syscall_table.def` (count only: 171), `nt_syscalls.h`, `ntdll.def` (export names).
- **Not read:** `except_test.c` (616 lines), `ldr_test.c` (716 lines), the `kernel32.def`, `user32.def`, `msvcrt.def`, `ucrtbase.def`, `kernelbase.def` and `advapi32.def` contents beyond their export counts.

## Suggested order

1. Make the loader real for MinGW: LDR-1, then LDR-2 to LDR-6, with the IAT and relocation bounds. Those are the prerequisites for any Win32 process to start.
2. Stop out-of-bounds writes in the startup mapper: MAIN-1 and MAIN-2, and the image checks in LDR-5 and LDR-6.
3. Fix the allocator and C runtime overflows: CRT-1 to CRT-4 (RT-1 is fixed; RT-2 applies to both copies).
4. Fix the exception path: UNW-2, UNW-3, EXC-2, EXC-3 (EXC-1 and UNW-1 are fixed).
5. Fix the Win32 surface that reports false results: K32-1 to K32-6, then K32-7 to K32-14.
6. Work through the remaining Medium and Low items, and refresh the differential report (NTS-2).
