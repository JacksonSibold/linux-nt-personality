# Rtl library ported from Wine

Status date: 2026-10-28. Sources: `src/ntdll/rtl/`. Host tests: `tests/rtl-host/`. Reuse record: [THIRD-PARTY.md](../policy/THIRD-PARTY.md).

## Why

`src/ntdll/ntdll.def` exports 427 names. Wine's `ntdll.spec` lists 1346. About 500 of the missing names are `Rtl*` functions that
every Windows program and every driver-side runtime library calls: string and Unicode handling, code page conversion, 64-bit
integer arithmetic, bitmaps, time, security descriptors, paths, the environment, tables and atoms. Wine implements them in
portable C and tests them against Windows behaviour, so they are taken from Wine rather than rewritten.

## How the port works

- The Wine files are kept close to upstream (same file names, same functions, upstream copyright headers). Every change is a
  line or block marked `ntpers:`.
- They are compiled against Wine's own Windows headers, vendored in `third_party/wine/include/`, and a thin layer of stand-in
  headers in `src/ntdll/rtl/compat/` (`ntdll_misc.h`, `wine/debug.h`, `winbase.h`, ...). The stand-ins carry only what the
  ported files use. Tracing (`TRACE`, `FIXME`) is compiled out.
- The same files build into `ntdll.dll` (MinGW, freestanding) and into the host tests (native gcc, `-fshort-wchar` so that
  `L"..."` strings are 16 bit, and `WINE_UNIX_LIB` so that `WINAPI` is empty). On the host, the C library functions that the
  files define (`wcslen`, `qsort`, `_itoa`, ...) are renamed with an `ntdll_` prefix by `compat/host_rename.h`, which
  `tests/rtl-host/gen_host_rename.py` writes from the sources.
- Wine's own unit tests (`dlls/ntdll/tests/*.c`) run against the result. `tests/rtl-host/include/wine/test.h` replaces Wine's test
  framework, and `host_support.c` supplies the process heap, the few system calls the code reaches, and a `GetProcAddress`
  that finds the functions by name. The platform is reported as Wine, so a `todo_wine` block marks a known divergence from
  Windows: the check may fail, and the run is flagged if it starts to pass.

```
make -C tests/rtl-host test        # needs gcc; uses AddressSanitizer and UBSan when they work
```

## What is in

| Source | Functions | Host test | Result |
|---|---|---|---|
| `rtlstr.c` | `RtlInit*String`, copy, compare, equal, prefix, append, upcase, find, `Rtl*ToInteger`, `Rtl*ToUnicodeString`, `RtlIsTextUnicode`, `RtlFormatMessage`, `RtlIpv4/6AddressToString` and back, GUID text | Wine `rtlstr.c` | 2,178,624 checks, 0 failures, 3 known divergences |
| `string.c`, `wcstring.c`, `printf.c` | the C library names ntdll.dll exports (`str*`, `wcs*`, `_itoa`, `is*`, `sprintf`, ...) | Wine `string.c` | 1,973,158 checks, 0 failures |
| `large_int.c` | `RtlLargeInteger*`, `RtlExtended*`, 64-bit shifts and division | Wine `large_int.c` | 612 checks, 0 failures |
| `rtlbitmap.c` | `RtlInitializeBitMap`, set, clear, find, run searches | Wine `rtlbitmap.c` | 378 checks, 0 failures |
| `time.c` | `RtlTimeToTimeFields`, `RtlTimeFieldsToTime`, seconds-since conversions | Wine `time.c` (the conversion part) | 10,564 checks, 0 failures |
| `nls.c` | `RtlMultiByteToUnicodeN`, `RtlUnicodeToMultiByteN`, OEM and UTF-8 forms, `RtlUpcaseUnicodeChar`, `RtlCompareUnicodeStrings`, `RtlHashUnicodeString`, `towlower`, `towupper` | covered by `rtlstr.c` and `string.c` | passes |
| `misc.c` | `qsort`, `bsearch`, `_lfind`, `Etw*` and `WinSqm*` stubs | covered by `string.c` | passes |
| `path.c` | `RtlDosPathNameToNtPathName_U`, `RtlGetFullPathName_U`, `RtlDetermineDosPathNameType_U`, `RtlIsDosDeviceName_U`, `RtlIsNameLegalDOS8Dot3`, `RtlSetCurrentDirectory_U`, `RtlDoesFileExists_U` | Wine `path.c` (without the two tests that need a file system) | 928 checks, 0 failures |
| `env.c` | `RtlCreateEnvironment`, `RtlSetEnvironmentVariable`, `RtlQueryEnvironmentVariable(_U)`, `RtlExpandEnvironmentStrings(_U)`, `RtlCreateProcessParametersEx`, `RtlNormalizeProcessParams` | Wine `env.c` | 607 checks, 0 failures |
| `rtl.c` | generic tables (splay and AVL), red-black trees, `RtlUniform`, `RtlRandomEx`, CRC-32, LZNT1 compression, pointer encoding, handle tables, IPv4 and IPv6 text, access checks | Wine `rtl.c` (subset) | 599,612 checks, 0 failures, 48 known divergences |
| `sec.c`, `sha.c` | SIDs, ACLs, security descriptors, `RtlDeriveCapabilitySidsFromName`; SHA-1 and SHA-256 (written here, Wine uses SymCrypt) | Wine `rtl.c`; `own/sha.c` | passes; 16 checks against NIST vectors |
| `atom.c`, `handletable.c`, `version.c`, `crypt.c`, `image.c`, `reg.c`, `error.c` | atom tables, handle tables, `RtlVerifyVersionInfo`, `RtlGetNtVersionNumbers` (identity from `os_identity.h`), MD4/MD5/SHA-1 primitives, `RtlImageNtHeader` and `RtlImageDirectoryEntryToData`, `RtlOpenCurrentUser` and the registry helpers, last-error accessors | partly by `rtl.c` | linked into ntdll.dll; no dedicated host test |

## What is not

- Wine's `__TRY ... __EXCEPT_PAGE_FAULT` guards around user pointers (in `sec.c`, `rtl.c`, `image.c`) run the guarded block without a handler (`compat/wine/exception.h`). An invalid pointer faults instead of returning `STATUS_ACCESS_VIOLATION`. This goes away when `seh.c` can carry the frames.
- The tests of `rtl.c` that exercise the loader, heaps, timers, critical sections and debug output are not run on the host; Wine's atom test needs kernel32 and is left out.
- The rest of `time.c`'s tests (time zone, timer resolution, shared user data) and the bad-pointer checks test system calls and
  the shared user data page. They are disabled on the host and belong in the VM run.
- Double-byte code pages (932, 936, 949, 950) are not in `nls_tables.h` yet. Single-byte pages 437 to 874, 1250 to 1258 and
  the ISO 8859 family are. The default pages are 1252 (ANSI) and 437 (OEM); `RtlpSetDefaultCodePages` changes them.
- `build/gen_nls_tables.py` needs a Wine checkout to regenerate the tables; the generated header is checked in.

## In ntdll.dll

`make -C src/ntdll` links the objects into `ntdll.dll` (MinGW, freestanding, no imports). They are built against Wine's own
Windows and C library headers (`third_party/wine/include`, `-nostdinc` so that no MinGW header gets in), as Wine builds
its ntdll. `ntdll_runtime.c` leaves out the string functions that Wine's `string.c` and `wcstring.c` now provide
(`NTDLL_RTL_PORT`); the other DLLs still use the small versions.

`src/ntdll/rtl/rtl_exports.txt` lists the exports: `build/gen_rtl_exports.py` reads Wine's `ntdll.spec` and the objects and
writes every spec entry whose symbol the objects define (aliases such as `sprintf=NTDLL_sprintf` included), and
`build/gen_ntdll_def.py` adds the file to the module definition. `ntdll.dll` exports 941 names, up from 427.

None of this code has run in a Windows process here. The DLL is checked at link time only (`--no-undefined`, export list
against the sources, `make -C src/ntdll check`); the logic is the code that the host tests run.
