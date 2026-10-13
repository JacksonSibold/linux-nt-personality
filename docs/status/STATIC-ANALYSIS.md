# Static analysis triage: loader, kernel/nt, ntdll, arch/x86/nt

Date: 2026-10-12. Branch `haiku/analyzer`. Tool: `tools/static_analyze.sh`
(informational; it always exits 0 and is run as the `static-analysis` step in
CI). Compile-only: nothing was linked or executed. Toolchain: gcc 16.2.0 (MSYS2
UCRT64), per `C:/Users/Jackson/repos/TOOLCHAIN.md`.

## How it runs

For each translation unit in `src/loader`, `src/kernel/nt`, `src/ntdll` and
`arch/x86/nt`, the script picks the first host-shim profile under which
`-fsyntax-only` succeeds, then runs:

```
gcc -fsyntax-only -Wall -Wextra -Wshadow -Wconversion -Wformat=2 -Wnull-dereference <profile> FILE
timeout 120 gcc -fanalyzer -c -o /dev/null <profile> FILE
```

Profiles: `kernel-syntax` (tests/syntax/check.sh), `kernel-host`
(tests/kernel-host/Makefile), `ntdll-host` (tests/ntdll-host/Makefile). Output
goes to `build/static-analysis/`: `report.txt` (normalised
`file:line: category: message`), `summary.txt` (counts), `units.txt` (per-unit
profile and analyzer exit code).

Usage: `sh tools/static_analyze.sh [FILE...]`

## Coverage

47 translation units. 44 analysed without timeout or analyzer error: 31 under
`kernel-syntax`, 1 under `kernel-host`, 12 under `ntdll-host`. 3 are unbuildable
under every profile, all because of shim gaps rather than code defects:

| Unit | Reason |
|------|--------|
| `arch/x86/nt/dispatch.c`, `src/kernel/nt/dispatch.c` | `struct file_operations` is incomplete in the shim (no `owner` member) |
| `src/ntdll/except_test.c` | needs `ucontext.h`, not present in the MinGW host headers |

## Results

| Measure | Before triage | After fixes |
|---------|---------------|-------------|
| Diagnostics (all categories) | 160 | 153 |
| `-fanalyzer` findings | 19 | 12 |
| Malloc leak, double free, use-after-free, buffer overflow | 0 | 0 |

The seven removed diagnostics are the seven TRUE-BUG fixes below.

Counts per category (after fixes, all units):

| Category | Count |
|----------|-------|
| sign-conversion | 66 |
| conversion | 28 |
| int-to-pointer-cast | 23 |
| analyzer-infinite-loop | 6 |
| unused-parameter | 5 |
| unused-variable | 4 |
| unused-but-set-variable | 4 |
| shadow | 4 |
| pointer-to-int-cast | 4 |
| analyzer-null-dereference | 4 |
| attributes | 3 |
| analyzer-use-of-uninitialized-value | 2 |

### Per file (after fixes)

`analyzer` is the `-fanalyzer` subtotal; the cast column is int-to-pointer and
pointer-to-int combined; `other` is unused-*, shadow and attributes.

| File | Total | analyzer | sign-conv | conversion | cast | other |
|------|------:|---------:|----------:|-----------:|-----:|------:|
| src/kernel/nt/registry.c | 22 | 1 | 8 | 0 | 12 | 1 |
| src/kernel/nt/security.c | 15 | 0 | 14 | 0 | 0 | 1 |
| src/loader/pe_parse.c | 13 | 0 | 0 | 13 | 0 | 0 |
| src/ntdll/ntdll_main.c | 13 | 3 | 3 | 5 | 1 | 1 |
| arch/x86/nt/nt_io.c | 11 | 1 | 6 | 0 | 0 | 4 |
| arch/x86/nt/nt_services.c | 7 | 0 | 2 | 0 | 5 | 0 |
| src/kernel/nt/io.c | 7 | 1 | 6 | 0 | 0 | 0 |
| src/kernel/nt/nt_services.c | 7 | 0 | 2 | 0 | 5 | 0 |
| src/ntdll/msvcrt.c | 7 | 3 | 0 | 2 | 0 | 2 |
| src/kernel/nt/object_test.c | 6 | 0 | 6 | 0 | 0 | 0 |
| src/kernel/nt/sync_test.c | 6 | 0 | 2 | 4 | 0 | 0 |
| src/kernel/nt/kshim.h | 5 | 1 | 0 | 0 | 4 | 0 |
| src/kernel/nt/mm_test.c | 5 | 0 | 5 | 0 | 0 | 0 |
| src/ntdll/user32.c | 4 | 0 | 1 | 3 | 0 | 0 |
| src/kernel/nt/object.c | 3 | 0 | 1 | 0 | 0 | 2 |
| src/kernel/nt/repro_mem_vad.c | 3 | 0 | 3 | 0 | 0 | 0 |
| src/ntdll/kernel32.c | 3 | 0 | 1 | 0 | 0 | 2 |
| arch/x86/nt/proc_thread.c | 2 | 0 | 0 | 0 | 0 | 2 |
| src/kernel/nt/handle.c | 2 | 2 | 0 | 0 | 0 | 0 |
| src/kernel/nt/proc_thread.c | 2 | 0 | 0 | 0 | 0 | 2 |
| src/kernel/nt/proc_thread_test.c | 2 | 0 | 2 | 0 | 0 | 0 |
| src/kernel/nt/i2_test.c | 1 | 0 | 1 | 0 | 0 | 0 |
| src/kernel/nt/iocp.c | 1 | 0 | 0 | 1 | 0 | 0 |
| src/kernel/nt/kshim.c | 1 | 0 | 1 | 0 | 0 | 0 |
| src/kernel/nt/security_test.c | 1 | 0 | 0 | 0 | 0 | 1 |
| src/ntdll/ldr.c | 1 | 0 | 0 | 0 | 0 | 1 |
| src/ntdll/ntdll_runtime.c | 1 | 0 | 0 | 0 | 0 | 1 |
| src/ntdll/ntstatus.h | 1 | 0 | 1 | 0 | 0 | 0 |
| src/ntdll/unwind.c | 1 | 0 | 1 | 0 | 0 | 0 |

The `-Wconversion` and `-Wsign-conversion` findings are numerous and were not
triaged one by one; they are style-level and do not affect the analyzer result.

## Triage of the 19 analyzer findings (before fixes)

Every `-fanalyzer` finding was read in context. Lines are the diagnostic lines.

| # | file:line | Category | Class | Reason | Status |
|---|-----------|----------|-------|--------|--------|
| 1 | arch/x86/nt/nt_io.c:838 | uninitialized `resolved` | FALSE-POSITIVE | `resolved[0]` is set to NUL on entry, and every advance of `rlen` follows a NUL-terminated `memcpy`, so bytes `0..rlen` are always written; the path-insensitive analysis loses this. | none |
| 2 | src/kernel/nt/io.c:870 | uninitialized `resolved` | FALSE-POSITIVE | Same code as item 1 (kernel copy). | none |
| 3 | src/kernel/nt/handle.c:156 | NULL `e` | SHIM-ARTIFACT | `KASSERT(e && e->free)` is `BUG_ON` in the kernel build; in the userspace shim it is MinGW `assert`, whose failure path is not noreturn to `-fanalyzer`, so analysis continues to `e->next_free`. The free-list head is always an allocated entry. | none |
| 4 | src/kernel/nt/handle.c:391 | NULL `te` | SHIM-ARTIFACT | Same as item 3 (`KASSERT(te)` then `te->next_free`). | none |
| 5 | src/kernel/nt/i2_test.c:349 | NULL `proc` | TRUE-BUG (test) | `make_proc` returns NULL after counting a failure; the test dereferenced it before any check. | fixed `a275418` |
| 6 | src/kernel/nt/object_test.c:408 | NULL `proc` | TRUE-BUG (test) | Same helper contract as item 5. | fixed `a275418` |
| 7 | src/kernel/nt/object_test.c:413 | NULL `thr` | TRUE-BUG (test) | `make_thread` returns NULL on failure; same as item 5. | fixed `a275418` |
| 8 | src/kernel/nt/registry.c:1124 | NULL `p` | TRUE-BUG | `NtQueryValueKey` with `KeyValueInformation == NULL` and `Length >= needed` wrote through the NULL pointer. | fixed `7c66a22` |
| 9 | src/kernel/nt/registry.c:1130 | NULL `p` | TRUE-BUG | Same as item 8 (basic information branch). | fixed `7c66a22` |
| 10 | src/kernel/nt/registry.c:1134 | NULL `p` | TRUE-BUG | Same as item 8 (full information branch). | fixed `7c66a22` |
| 11 | src/kernel/nt/registry.c:200 | NULL `Buffer` (Length > 0) | FALSE-POSITIVE | `NtQueryValueKey` rejects a `ValueName` with NULL `Buffer` and nonzero `Length` (registry.c:1088) before any comparison; other callers pass registry-owned names. | none |
| 12 | src/kernel/nt/kshim.h:230 | NULL `prev` in `__list_del` | FALSE-POSITIVE | Reached only by calling `list_del` on an entry already removed (`list_del` NULLs its links). That is a caller contract violation; `nt_obj.c:289/294` guard their re-deletes, but not every caller was checked. | none |
| 13 | src/ntdll/msvcrt.c:336 | infinite loop | FALSE-POSITIVE | `while(1){}` after `NtTerminateProcess` in `exit`, a deliberate spin for a call that does not return. | none |
| 14 | src/ntdll/msvcrt.c:339 | infinite loop | FALSE-POSITIVE | Same as item 13 (`_exit`). | none |
| 15 | src/ntdll/msvcrt.c:340 | infinite loop | FALSE-POSITIVE | Same as item 13 (`_Exit`). | none |
| 16 | src/ntdll/ntdll_main.c:584 | infinite loop | FALSE-POSITIVE | Deliberate spin after `NtTerminateProcess` in the loader entry error paths. | none |
| 17 | src/ntdll/ntdll_main.c:587 | infinite loop | FALSE-POSITIVE | Same as item 16. | none |
| 18 | src/ntdll/ntdll_main.c:589 | infinite loop | FALSE-POSITIVE | Same as item 16. | none |
| 19 | src/ntdll/ntdll_runtime.c:189 | infinite recursion | TRUE-BUG | With `errno.h` in scope, `errno` expands to `(*_errno())`, so `_errno` returned the address of a call to itself. | fixed `02f989c` |

Classes after triage: TRUE-BUG 7 (fixed), SHIM-ARTIFACT 2, FALSE-POSITIVE 10.

The `attributes` diagnostic at `src/ntdll/ntdll_runtime.c:192` (`_errno`
redeclared without dllimport) is a SHIM-ARTIFACT: the MinGW `errno.h`
declaration carries `dllimport`. It fails `-Werror` in `tests/ntdll-host`
both before and after this change; the `-Werror` build there needs a separate
decision.

## Fixes

Each TRUE-BUG has its own commit, with the same reasoning as the table above:

- `7c66a22` kernel/registry: reject NULL `KeyValueInformation` in `NtQueryValueKey` (registry.c:1124/1130/1134).
- `02f989c` ntdll/runtime: `#undef errno` before the `_errno` definition (ntdll_runtime.c:189).
- `a275418` tests(kernel): stop dereferencing failed `make_proc`/`make_thread` results (i2_test.c:349, object_test.c:408/413).

The test fixes are built and analysed here but not run: Windows Application
Control blocks freshly built executables, so the host tests were not run.

## Not analyzer findings, not fixed

`ntio_resolve_ci` in `arch/x86/nt/nt_io.c` (lines 800 and 887) and
`src/kernel/nt/io.c` (lines 832 and 919) copy each path component into
`char component[NAME_MAX]` with `component[i++] = *p++;` and no bound check,
and then copy into `resolved[PATH_MAX]` without a length bound. A path
component longer than `NAME_MAX - 1` overflows `component`. `-fanalyzer` did
not report it. The fix needs an error return for over-long components, so it
was left for a separate change.
