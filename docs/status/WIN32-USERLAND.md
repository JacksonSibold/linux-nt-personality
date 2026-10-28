# Win32 userland: what exists and how far it is verified

Status date: 2026-10-28. This page covers the freestanding Windows DLLs under `src/ntdll/` and the C runtime under `src/crt/`.
It does not cover the kernel (see [KERNEL-BOOT.md](KERNEL-BOOT.md) and [KERNEL-GAPS.md](KERNEL-GAPS.md)).

## What "verified" means here

None of these DLLs has been loaded or run by the code in this repository's CI. They are cross-built with MinGW and
checked at link time (`--no-undefined`, with the export lists compared to the sources). The logic is verified in a different
way: it is written as headers that also compile natively, and those headers have host tests with AddressSanitizer and
UBSan, many of them differential against glibc. A bug in the thin layer that calls kernel32 or the kernel (a wrong
structure offset, a wrong calling convention, a missing syscall) would not be found by any of this. The VM differential
probes ([DIFFERENTIAL-RESULTS.md](DIFFERENTIAL-RESULTS.md)) are the first place such bugs show up, and
`ucrtbase.dll` has not been through them.

## DLLs

| DLL | Source | Exports | Notes |
|---|---|---|---|
| `ntdll.dll` | `ntdll*.c`, `ldr*.c`, `rtl_*` | `ntdll.def` | Syscall stubs, loader, heap, Rtl pieces. |
| `kernel32.dll` | `kernel32*.c` (22 files) | `kernel32.def` | Files, memory, threads, sync objects, wide-character file functions, system information, message formatting, NLS (en-US only), private-profile (INI) files, PE resources, fibers, logical-processor topology, console screen-buffer state, the thread pool, timer queues and registered waits. Pure logic in `k32_*_pure.h`. |
| `kernelbase.dll`, `user32.dll`, `win32u.dll`, `gdi32.dll` | `kernelbase.c`, `user32*.c`, `win32u.c`, `gdi32_win32k.c` | `*.def` | Subsets. user32 and gdi32 are far from complete. |
| `advapi32.dll` | `advapi32.c`, `advapi32_reg.c` | `advapi32.def` | The registry API, wide and ANSI (open, create, query, set, delete, enumerate, `RegGetValue`, `RegDeleteTree`, the old forms); the rest of advapi32 (tokens, security descriptors, services, crypto, event tracing) is not there yet. |
| `msvcrt.dll` | `msvcrt.c` | `msvcrt.def` | The older minimal CRT (77 lines of exports). |
| `ucrtbase.dll` | `ucrtbase.c` and `ucrtbase_*.inc` | `ucrtbase.def` (generated) | The Universal C Runtime; see below. |
| `vcruntime140.dll` | `vcruntime140.c`, `vcruntime140_exc.c` | `vcruntime140.def` | C++ exceptions for code built with the Microsoft compiler (`_CxxThrowException`, `__CxxFrameHandler3`), RTTI (`__RTDynamicCast`, `__RTtypeid`, `type_info`), `std::exception` and the other standard exception classes, forwarders for `memcpy` and friends. See below. |

`api-ms-win-crt-*.dll` names resolve to `ucrtbase.dll` in the loader (`ldr_pure.h`).

## Structured exception handling (ntdll)

`unwind.c`, `except.c` and `seh.c` implement the x64 model: `RtlCaptureContext` and `RtlRaiseException` (assembly, capturing the
caller's state), `RtlVirtualUnwind` (the establisher frame is the base of the fixed allocation, as frame handlers expect),
`RtlDispatchException` (vectored handlers, then frame handlers called with the original context and a dispatcher context),
`RtlUnwindEx` and `RtlUnwind` (second pass: termination handlers run, the target frame's handler is called with
`EXCEPTION_TARGET_UNWIND`, control continues at the target with the return value in Rax through `RtlRestoreContext`), and
`__C_specific_handler` (scope tables of `__try`/`__except`/`__finally`). msvcrt and ucrtbase forward it to ntdll.
`tests/ntdll-host/test_seh.c` runs this on the host with hand-assembled functions that have real unwind data, scope tables
and filters, and checks where control lands, which registers and stack it lands with, and the order of termination handlers.
Not implemented: the C++ frame handlers (`__CxxFrameHandler3/4`), `STATUS_LONGJUMP` and `STATUS_UNWIND_CONSOLIDATE`
handling in `RtlUnwindEx`, and nested-exception bookkeeping beyond a depth guard. Whether `KiUserExceptionDispatcher`
reaches this code correctly on the booted kernel is not tested here.

## ucrtbase.dll

The library is in `src/crt/` as headers (`ucrt_*.h`): strings, `ctype`, number conversion with exact `strtod` and shortest
`printf` output, `printf` and `scanf` engines for narrow and UTF-16 text, `stdio` with the 48-byte `FILE` and text-mode
rules, low-level file descriptors, `math` (x87 extended-precision evaluation with a precision-control guard so results
match a Windows process that runs at 53-bit precision), time with the Windows time-zone model, locale and multibyte
conversion, command-line splitting, environment, exit and `atexit`. It reaches the system through small tables
(`ucrt_os`, `ucrt_time_ops`, `ucrt_proc_ops`) so the same code runs on POSIX in the tests.

`ucrtbase.c` supplies the kernel32 backends, per-thread `errno`, the heap, and the DLL entry point. The `.inc` files hold the
wrappers whose Windows signature differs from the library function:

| File | Contents |
|---|---|
| `ucrtbase_gen.inc`, `ucrtbase.def` | Written by `build/gen_ucrtbase.py` from its tables: one-to-one wrappers, `_l` locale variants, aliases (POSIX names, long double names). |
| `ucrtbase_heap.inc` | `malloc` family, aligned allocators, new handler hooks. |
| `ucrtbase_stdio.inc` | `__stdio_common_v*printf/scanf` with the options word, and the classic `printf` family. |
| `ucrtbase_start.inc` | `argc`/`argv`, `environ`, `atexit` tables, `_beginthreadex`, `rand_s`, error accessors. |
| `ucrtbase_misc.inc` | Strings with state, ctype tables, `_itoa` family, `_ecvt`/`_fcvt`/`_gcvt`, multibyte, `localtime` buffers. |
| `ucrtbase_fp.inc` | `_controlfp_s` and `fenv` on MXCSR, `_fpclass`, `_dclass`. |
| `ucrtbase_fs.inc`, `ucrtbase_path_t.inc` | `_stat64` and variants, `_findfirst64`, current directory, `_fullpath`, `_splitpath`, temp names. |
| `ucrtbase_extra.inc`, `ucrtbase_more.inc` | Word I/O, `_sopen`, file times, collation, 32-bit time variants, `setjmp`/`longjmp`, and more. |

To add an export: write `w_<name>` in an `.inc` file and either list the name in a `manual(...)` call in the generator or put
it in a `/* UCT_EXPORTS name ... */` comment next to the definition, then run `python3 build/gen_ucrtbase.py`.

The export list was compared with the real `ucrtbase.dll` (the names in MinGW's `libucrtbase.a`). Present: the functions
the C and POSIX-style interfaces use, the `__stdio_common_*` entry points, startup and exit support, and the `*l` long double
names (as aliases of the double functions, because `long double` is `double` in the Microsoft ABI). Absent:

- Complex arithmetic (`cexp`, `cabs` on `_Dcomplex` and the rest), Bessel functions, `remquo`, and most single-precision
  functions beyond the common ones.
- Child processes: `system`, `_spawn*`, `_exec*`, `_popen`, `_pipe` (kernel32 has no `CreateProcess` or `CreatePipe` yet).
- Multibyte-character-set functions (`_mbs*`, `_mbc*`), `wctype`/`wctrans`, `_cgets`/`_cputs` and the wide console functions.
- Data exports (`__sys_errlist`, `__initenv`, `_environ`-style variables); programs get the values through the accessor functions.
- The `_o_*` forwarders of the API-set DLLs.
- `vcruntime140.dll` and `msvcp140.dll`: C++ exception handling and the C++ library. `__C_specific_handler` and the
  frame handlers are not implemented.

Known simplifications: narrow paths are widened byte by byte (Latin-1), so the process code page is not applied;
`_getch` reads standard input without console modes; `_gcvt` follows the Microsoft form but is covered by few tests;
`longjmp` does not run unwind handlers; collation is the "C" locale's; `rand_s` uses RDRAND when the CPU has it and otherwise a
non-cryptographic generator.

## vcruntime140.dll

The logic is in four headers that the host tests compile natively against hand-built tables: `vcr_rtti_pure.h` (RTTI and
`dynamic_cast` over single, multiple and virtual inheritance), `vcr_typeinfo_pure.h` (`type_info` compare, hash and
name decoding), `vcr_eh_pure.h` (the fixed-size tables of `__CxxFrameHandler3`: state from address, cleanup chains, try/catch
selection, funclet detection, the copy into the catch parameter) and `vcr_eh4_pure.h` (the compressed tables of
`__CxxFrameHandler4`: variable-length numbers, split state maps, the cleanup chain, try and catch records). The host tests
build their tables with an encoder written from the same reading of the format, so they show the decoder is self-consistent,
not that it matches what the compiler emits. The DLL files apply them to a real stack and are only built and link-checked: nothing in them has run
against code built by the Microsoft compiler.

How a catch runs: the frame handler finds the catch block in the first pass, moves the thrown object to the heap, builds the
catch parameter and unwinds to the catching frame with `vcr_catch_entry` as the target; that routine looks like a call from
the catching frame, runs the catch funclet and jumps to the address it returns. A bare `throw;` raises the exception of the
innermost running catch again.

`__CxxFrameHandler4` is implemented in `vcruntime140.dll`; `vcruntime140_1.dll`, where the Microsoft runtime keeps it, forwards
its one export there. Not done: the SEH translator, WinRT exceptions, `__NLG_Dispatch2`, `msvcp140.dll`. A thrown object is moved with a byte copy, so a type that points into itself
does not survive a throw.

## Host test suites

`make -C tests/crt-host test` builds each test with ASan and UBSan and runs it: strings, ctype, `dtoa`, conversions,
`strtod`, `printf`, `stdlib`, two math suites, low-level I/O, `stdio`, `scanf`, time, locale, runtime, and four suites for the
`.inc` files (`ucrtbase_stdio`, `ucrtbase_misc`, `ucrtbase_fp`, `ucrtbase_path`). Most compare against glibc; where Windows
differs from glibc on purpose (32-bit `long`, wrapping overflow in `scanf`, `(null)`, two-digit exponents), the test says so
and checks the Windows value instead. `make -C tests/ntdll-host test` covers the pure headers of the other DLLs.

### advapi32 registry

`advapi32_reg.c` is also compiled into `tests/kernel-host/advapi_reg_test.c`, which runs it against the kernel's registry
module (`src/kernel/nt/registry.c`) with host stand-ins for the few kernel32 calls: so lifecycle, value sizes and `ERROR_MORE_DATA`,
string termination, `RegGetValue` type filtering and expansion, enumeration, `RegQueryInfoKey`, tree deletion, the ANSI forms and
the predefined keys are checked against the real NT registry code. The code page is taken as Latin-1 in that test. Not done:
key change notification, key security, hive load and save (these return `ERROR_CALL_NOT_IMPLEMENTED`), the WOW64 views.

### kernel32 additions

Each of these has its logic in a `k32_*_pure.h` header that a host test compiles natively, plus a thin DLL file.

| Area | Host test | What the host test does not cover |
|---|---|---|
| Private-profile (INI) functions | `test_k32_ini_pure` | The file reads and writes in `kernel32_ini.c`. |
| PE resources (`FindResource*`, `LoadResource`, `EnumResourceNames*`) | `test_k32_res_pure` | Handing out module images from the loader. |
| Fibers | `test_k32_fiber_pure` (real stack switches) | TEB updates with a real TEB. |
| Logical-processor information | `test_k32_y_pure` | Real topology (the records describe one package, one core per CPU). |
| Console screen buffer, cell access, scrolling, title, Ctrl handlers, key records | `test_k32_con_pure` | `kernel32_con.c` (only built and export-checked). The state is in this process; `WriteConsole` does not update the cells, and `ReadConsoleInput` builds key records from bytes read on the input handle. |
| Thread pool, timer queues, `RegisterWaitForSingleObject` | `test_k32_tp_pure` (engine on pthreads) | `kernel32_tp.c`, which supplies the engine's operations from kernel32 and is only built and export-checked. |

Not implemented in the thread pool: cleanup groups do not track `TrySubmitThreadpoolCallback` items, a closed private pool is
not freed, and `SetThreadpoolStackInformation` is accepted and ignored.

`tools/run_all_checks.sh` runs both.
