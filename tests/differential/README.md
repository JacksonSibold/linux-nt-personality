# Differential Testing Harness — `tests/differential`

This directory is the **Windows oracle** for M0-E (Ubuntu 26.04 + native Windows/NT personality). It provides ground truth by running the *identical* PE binary with identical inputs on real Windows and on our OS, then diffing observable behaviour (return values, `GetLastError`/`NTSTATUS`, `MEMORY_BASIC_INFORMATION`, handle semantics, share-mode results, TEB/PEB layout).

A Windows VM is explicitly permitted for reference behaviour and differential testing; it is only prohibited as the execution mechanism for the NT personality itself. The host of this project is a Windows 11 machine, so real Windows is directly available as an oracle.

---

## Layout

```
tests/differential/
  probes/               # C source for each probe PE (mingw-compiled, runs on both hosts)
    probe_ntstatus.c    # RtlNtStatusToDosError table + two-call size-probe idiom
    probe_teb_peb.c     # TEB/PEB field offsets via GS-relative reads
    probe_virtualquery.c# VirtualAlloc pattern -> VirtualQuery coalescing oracle
    probe_handles.c     # handle values, GrantedAccess, DuplicateHandle, NtQueryObject, GetProcessHandleCount
    probe_sharemode.c   # DesiredAccess × ShareAccess matrix (NtCreateFile) -> NTSTATUS
  build/                # compiled PE probes (probe_*.exe) — gitignored except for CI
  golden/               # versioned Windows results
    <build>/            # e.g. 26200.9168/ (BuildNumber.UBR)
      version.json
      probe_*.json
    latest/             # symlink/copy of most recent golden
  harness.ps1           # build + run + capture (Windows host)
  compare.ps1           # human-readable diff against golden
  README.md             # this file
```

## Prerequisites

- **Windows host**: PowerShell 5.1+, Windows 11 SDK headers (for probes). No extra tooling needed; probes run as native x64 PEs.
- **Linux side via WSL**: `x86_64-w64-mingw32-gcc` (GCC 13-win32) available in the `Ubuntu` distro. Invoked as `wsl bash -c '...'`. All builds copy through the staging directory `%LOCALAPPDATA%\Temp\opencode` (`/mnt/c/Users/<you>/AppData/Local/Temp/opencode` from WSL) to avoid `/mnt/c` spaces/caching issues.

## Environment variables

| Variable | Meaning |
|----------|---------|
| `NTPERS_ROOT` | Repository root. Set it to the Windows path, or to the WSL path (`/mnt/c/...`) for the WSL commands below. `check_*.c` read the goldens from `$NTPERS_ROOT/tests/differential/golden/latest/` when the path relative to the current directory is not found. |
| `NTPERS_STAGE_NT` (C macro) | NT path for the extra copies of probe JSON written by the `*_direct` probes on Windows. Set with `-DNTPERS_STAGE_NT=...`. The primary capture is stdout, so the default (`\??\mnt\share\.dodresult`) is fine when the copy fails. |

## Syntax-check status

Checked with the MSYS2 UCRT64 gcc (16.2.0):

```sh
gcc -fsyntax-only -Wall -Wextra -std=c11 -m64 -Isrc/ntdll -Isrc/loader -Isrc/kernel/nt -Isrc <file.c>
```

- `check_*.c`, `probes/*.c`, `tests/conformance/*.c` and `build/test_ntdll_pe.c` have no errors with these include directories.
- Project headers: `nt_types.h` and `ntstatus.h` live in `src/ntdll`, and `pe_parse.h` in `src/loader`. `tests/conformance/test_nt_types.c` and `tests/differential/check_ntstatus.c` need `-Isrc/ntdll`. `tests/conformance/test_pe_parse.c` and `build/test_ntdll_pe.c` need `-Isrc/loader`.
- Windows-specific headers: the probes include `<windows.h>` and `<winternl.h>` (mingw-w64 or Windows SDK). They do not build against glibc, so Linux needs a mingw-w64 cross toolchain. Under the MSYS2 mingw-w64 gcc they pass with warnings only: some probes re-declare NT structures and constants that `winternl.h` already defines, which gives "redefined" warnings. `tests/conformance/pe_fwd.c` and `pe_smoke.c` also include `<windows.h>`.
- `check_*.c` and `tests/conformance/test_*.c` use only the C standard library and the project headers above.

## Building probes

Probes are plain C11, no CRT exotic deps, linked only against `ntdll.dll`/`kernel32.dll` via dynamic `GetProcAddress` (so no import-lib required).

**Via harness (recommended):**
```powershell
# Build all probes via mingw in WSL
.\tests\differential\harness.ps1          # builds + runs + captures
.\tests\differential\harness.ps1 -Probe probe_ntstatus   # single probe
.\tests\differential\harness.ps1 -NoBuild                # only run, skip compile
```

**Manual (WSL):**
```powershell
# PowerShell -> WSL staging (handles spaces)
Copy-Item tests\differential\probes\*.c -Destination $env:LOCALAPPDATA\Temp\opencode\ -Force
wsl bash -c 'cp /mnt/c/Users/<you>/AppData/Local/Temp/opencode/*.c /tmp/ && x86_64-w64-mingw32-gcc -O2 -m64 -o /tmp/probe_ntstatus.exe /tmp/probe_ntstatus.c && cp /tmp/probe_*.exe /mnt/c/Users/<you>/AppData/Local/Temp/opencode/'
Copy-Item $env:LOCALAPPDATA\Temp\opencode\probe*.exe -Destination tests\differential\build\ -Force
```

## Running probes on Windows (capture golden)

```powershell
# Capture all probes; output is JSON to stdout and to golden/<build>/
$build = (Get-CimInstance Win32_OperatingSystem).BuildNumber
$ubr   = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion").UBR
$goldenDir = "tests\differential\golden\$build.$ubr"
New-Item -ItemType Directory -Force -Path $goldenDir | Out-Null
foreach($p in @("probe_ntstatus","probe_teb_peb","probe_virtualquery","probe_handles","probe_sharemode")){
  $exe = "tests\differential\build\$p.exe"
  $out = "$goldenDir\$p.json"
  cmd /c "`"$exe`" > `"$out`" 2>&1"   # use cmd to avoid PowerShell truncation
}
# also update latest
Copy-Item "$goldenDir\*.json" -Destination tests\differential\golden\latest\ -Force
```

Each probe emits a single JSON object to stdout:
```json
{
  "probe": "probe_ntstatus",
  "windows_build": "26200.9168",
  "windows_version": "10.0.26200",
  "arch": "x64",
  "rtl_mappings": [ {"name":"STATUS_SUCCESS","ntstatus_hex":"0x00000000","dos":0}, ... ],
  "size_probe": [ {"api":"NtQueryInformationFile(FileNameInformation)","buffer_len":8,"ntstatus_hex":"0x80000005"}, ... ]
}
```

The `windows_build` is `BuildNumber.UBR` (e.g. `26200.9168` = Windows 11 Pro 25H2). The `version.json` in each golden directory records the full `windows_version` and timestamp.

## Running probes on our OS (differential)

The same PE (`tests/differential/build/probe_*.exe`) is executed under the NT personality (via `binfmt_pe` / `kshim`). Capture is identical:

```bash
# in WSL, after booting ntpers
"$NTPERS_ROOT/tests/differential/build/probe_ntstatus.exe" > /tmp/out.json
# then diff
diff -u tests/differential/golden/latest/probe_ntstatus.json /tmp/out.json
```

The harness `compare.ps1` provides a human-readable summary:
```powershell
.\tests\differential\compare.ps1 -Build latest
```

## Adding a new probe

1. Create `tests/differential/probes/probe_<name>.c`:
   - Include `<windows.h>` and `<winternl.h>` as needed.
   - Use `GetProcAddress(GetModuleHandleA("ntdll.dll"), "...")` for NT APIs (avoids import-lib).
   - Emit **one** JSON object to stdout (no extra logging). Use `printf` with `"%s"` for strings, `"%lu"` for DWORD, `"%llX"` for pointers (x64). Ensure output is UTF-8 and starts with `{`.
   - Record `windows_build`/`windows_version`/`arch` at top (copy the `RtlGetVersion` + registry `UBR` snippet from existing probes).
2. Add the probe name to `harness.ps1`'s `$probes` array and to `compare.ps1` if needed.
3. Build and capture:
   ```powershell
   .\tests\differential\harness.ps1 -Probe probe_<name>
   ```
4. Verify the new golden is present in `golden/<build>/` and `golden/latest/`, then update this README's probe table.

## Probe catalog

| Probe | What it captures | Why it matters |
|-------|------------------|----------------|
| `probe_ntstatus` | `RtlNtStatusToDosError` for 60 NTSTATUS values; `NtQueryInformationFile`/`NtQueryObject`/`NtQuerySystemInformation` with undersized buffers (distinguishes `STATUS_BUFFER_OVERFLOW` 0x80000005 vs `STATUS_BUFFER_TOO_SMALL` 0xC0000023 vs `STATUS_INFO_LENGTH_MISMATCH` 0xC0000004); fallback for unmapped codes | Validates `src/ntdll/ntstatus_to_dos.c` table and fallback (`FACILITY_NTWIN32`, `FACILITY_NULL`). The two-call size-probe idiom is load-bearing for every file/query API. |
| `probe_teb_peb` | GS-relative reads (`gs:0x30` TEB, `gs:0x60` PEB, `gs:0x68` LastError, `gs:0x1480` TlsSlots) vs raw memory at offsets; PEB fields `BeingDebugged` (0x02), `ImageBaseAddress` (0x10), `Ldr` (0x18), `ProcessParameters` (0x20), `ProcessHeap` (0x30), `ApiSetMap` (0x68), `NtGlobalFlag` (0xBC), `OSBuildNumber` (0x120) | Validates `src/ntdll/nt_types.h` offsets. Several offsets were marked UNVERIFIED; a single wrong offset breaks every `/GS` binary and TLS access. |
| `probe_virtualquery` | Scripted pattern: `VirtualAlloc RESERVE 64K` → `COMMIT 4K RW at +0x0000` → `COMMIT 4K RO at +0x1000` → `COMMIT 4K RX at +0x4000` → `VirtualProtect RW→RO` → dumps `MEMORY_BASIC_INFORMATION` after each step; decommit/recommit cycle; free-region query | Oracle for `src/kernel/nt/mm.c` coalescing rule (`nt_vad_attrs_match`). Windows coalesces adjacent committed pages with identical `Protect`/`State`/`AllocBase`/`Type` into one `RegionSize`; reserve gaps coalesce; decommit merges into reserve. |
| `probe_handles` | Handle values (multiples of 4, sequential), `GetProcessHandleCount`, `NtQueryObject` (`ObjectBasicInformation` GrantedAccess/handleCount, `ObjectTypeInformation` type name, `ObjectNameInformation`), small-buffer `STATUS_INFO_LENGTH_MISMATCH`, `DuplicateHandle` same-access and narrowing, `DUPLICATE_CLOSE_SOURCE` | Validates `src/kernel/nt/handle.c` (3-level table, `index<<2 | attr`, LIFO free list) and `src/kernel/nt/io.c` handle table. Pseudo-handles `-1`/`-2`, `CloseHandle` error codes. |
| `probe_sharemode` | Full `DesiredAccess × ShareAccess` matrix (11 desired × 8 share = 88 first × 88 second = 7744 `NtCreateFile` attempts) recording `NTSTATUS` (`STATUS_SHARING_VIOLATION` 0xC0000043 vs `STATUS_SUCCESS`); targeted examples (exclusive, compatible, delete sharing) | Oracle for `src/kernel/nt/io.c` `ntio_check_share_access` (`(new_desired & ~existing_share)==0 && (existing_desired & ~new_share)==0`). |
| `probe_peloader` | Windows PE loader oracle: `CreateFileMapping(SEC_IMAGE)` + `LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)` verdict for any PE file; reports `sec_image_accept`, `sec_image_error` (Win32), `loadlibrary_accept`; used to ground `pe_validate` accept/reject decisions | Validates `src/loader/pe_parse.c` hardening. Generates empirical ground truth for malformed PEs (overlapping VA, bad NumberOfSections, SizeOfImage, VA alignment, SizeOfHeaders, entry point, RVA cycles, overflow, SizeOfOptionalHeader, data-directory bounds) so our 90%+ structured-mutation rejection rate matches real Windows rather than assumption. |

## Two-Target Rule for `_direct` Probes (Critical — See Task Z2)

**Defect fixed 2026-08-31 (agent Y5):** The `*_direct` probes previously issued **raw syscalls** (`mov eax, SSN; syscall`) with SSNs pinned to Windows Server 2022 (build 20348, `src/ntdll/syscall_table.def`). The Windows host that captures goldens is build 26200. NT syscall numbers are **not stable across builds** — e.g. `NtResetEvent` 0x180→0x187, `NtPulseEvent` 0x143→0x148, `NtCreateMutant` 0xB6→0xBA, `NtCreateThreadEx` 0xC5→0xC9, `NtSuspendThread` 0x1C4→0x1CF — so on the Windows host the probes invoked **different services** than intended, returning `0xC0000061` / `0xC0000008` artifacts. An unknown fraction of recorded divergences in `probe_sync_direct` and `probe_process_thread_direct` were measurement artifacts, and goldens captured this way are untrustworthy.

**Correct method — one source, two binaries:**

- **WINDOWS build** (golden oracle): **Must** reach NT services through `ntdll`'s **exported stubs** via `GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateFile" …)`. Those stubs contain the correct SSN for whatever build they run on. **Never** hardcode raw SSNs on the Windows side.
- **VM build** (our kernel): **Must** use **raw syscall stubs** with SSNs from `src/ntdll/syscall_table.def` (pinned to 20348), because our kernel implements that pinned table.

Same source, two build modes via `#ifdef NTPERS_VM`:

```powershell
# PowerShell -> WSL staging (handles spaces)
Copy-Item tests\differential\probes\probe_sync_direct.c -Destination $env:LOCALAPPDATA\Temp\opencode\ -Force
# WINDOWS build (GetProcAddress, correct SSN per host)
wsl bash -c 'cp /mnt/c/Users/<you>/AppData/Local/Temp/opencode/probe_sync_direct.c /tmp/probe_sync_direct.c && x86_64-w64-mingw32-gcc -mconsole -O2 -std=c11 -m64 -o /tmp/probe_sync_direct.exe /tmp/probe_sync_direct.c && cp /tmp/probe_sync_direct.exe /mnt/c/Users/<you>/AppData/Local/Temp/opencode/probe_sync_direct.exe'
Copy-Item $env:LOCALAPPDATA\Temp\opencode\probe_sync_direct.exe -Destination tests\differential\build\probe_sync_direct.exe -Force
# VM build (raw syscalls, pinned SSNs)
wsl bash -c 'cp /mnt/c/Users/<you>/AppData/Local/Temp/opencode/probe_sync_direct.c /tmp/probe_sync_direct.c && x86_64-w64-mingw32-gcc -DNTPERS_VM -nostdlib -ffreestanding -fno-builtin -Wl,-e,_start -O2 -std=c11 -m64 -o /tmp/probe_sync_direct_vm.exe /tmp/probe_sync_direct.c && cp /tmp/probe_sync_direct_vm.exe /mnt/c/Users/<you>/AppData/Local/Temp/opencode/probe_sync_direct_vm.exe'
Copy-Item $env:LOCALAPPDATA\Temp\opencode\probe_sync_direct_vm.exe -Destination tests\differential\build\probe_sync_direct_vm.exe -Force
```

The indirection is a small macro/function-pointer layer:

```c
#ifdef NTPERS_VM
__asm__(".globl NtCreateFile\nNtCreateFile: mov %rcx,%r10; mov $0x55,%eax; syscall; ret");
NTSTATUS NtCreateFile(...);
#else
#include <windows.h>
static NTSTATUS (NTAPI *pNtCreateFile)(...);
#define NtCreateFile pNtCreateFile
static void init_ntdll(void){ g_ntdll=GetModuleHandleA("ntdll.dll"); pNtCreateFile=(void*)GetProcAddress(g_ntdll,"NtCreateFile"); ... }
#endif
// In main (Windows): init_ntdll(); do_probe();
// In _start (VM): do_probe();
```

Operation sequence and JSON output are **byte-identical** between the two (only the dispatch differs), so results stay comparable. The existing non-direct probes (`probe_ntstatus.c`, `probe_handles.c` etc) already use `GetProcAddress` and are correct on Windows; only the `*_direct` variants had regressed.

**Build-mismatch guard:** Every golden records `windows_build` (`BuildNumber.UBR`, e.g. `26200.9168`). `dualrun.py` now **FAILS** if a golden's `windows_build` does not match the host that captured it (`get_host_build()` via `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` `CurrentBuild`+`UBR`). This catches the class of error automatically rather than by inspection. If you see `build mismatch: golden 20348.x vs host 26200.9168 - regenerate golden`, rebuild the Windows probe with the correct two-target method and recapture.

**Audit 2026-08-31:** All 7 `*_direct` goldens are suspect. `probe_virtualquery_direct` and `probe_path_direct` reported 0 divergences before — **correct by luck** (their SSNs `NtAllocateVirtualMemory` 0x18, `NtQueryVirtualMemory` 0x23, `NtCreateFile` 0x55 etc happen to match across 20348→26200). Their goldens were still regenerated for hygiene (diff: `probe_virtualquery_direct.json` 46 lines, `probe_path_direct.json` 20005 lines but largely identical). The other 5 changed substantially: `probe_sync_direct.json` 31 lines (S1 25→4), `probe_process_thread_direct.json` 19 lines (S1 47→39), `probe_handles_direct.json` 22 lines, `probe_sharemode_direct.json` 65283 lines (matrix length 57344→65536 after fixing `NtCreateFile` sharing), `probe_ntstatus_direct.json` 3 lines (but `probe_ntstatus_direct` VM now correctly uses `RtlNtStatusToDosError` via `GetProcAddress` on Windows). `probe_path.json` (non-direct) also showed 20000 lines diff due to `C:\tmp` regeneration — not part of the direct defect but noted.

**Never reintroduce raw SSNs on the Windows side.** The two-target rule is enforced by `dualrun.py`'s build guard and by this README. If you add a new `*_direct` probe, copy the `#ifdef NTPERS_VM` pattern from `probe_sync_direct.c` and ensure `harness.ps1` builds both variants.

## Regenerating goldens after a Windows update

Windows build changes invalidate goldens (offsets and some NTSTATUS→DOS mappings drift). To regenerate:

```powershell
# 1. Check current build
Get-CimInstance Win32_OperatingSystem | Select BuildNumber,Version
Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion" | Select CurrentBuild,UBR,DisplayVersion

# 2. Rebuild and recapture
.\tests\differential\harness.ps1

# 3. Verify
.\tests\differential\compare.ps1
Get-ChildItem tests\differential\golden\26200.* | Format-Table Name,Length

# 4. Commit (if repo is git)
git add tests\differential\golden\ tests\differential\README.md
git commit -m "differential: update goldens for Windows 26200.9168"
```

Keep old builds under `golden/<build>/`; do not delete them. `latest` is always the most recent.

## Troubleshooting

- **`/mnt/c` copy invisible on Windows**: WSL's `/mnt/c` cache can lag. Always stage through `%LOCALAPPDATA%\Temp\opencode` (visible to both sides) then `Copy-Item` via PowerShell.
- **Probe prints non-JSON**: Ensure probe writes only JSON to stdout; use `fprintf(stderr, ...)` for diagnostics. The harness validates that output starts with `{`.
- **Large `probe_sharemode.json` (2 MB) truncation**: Use `cmd /c '"exe" > "file"'` instead of PowerShell pipeline (`Out-String` truncates at 1000 chars). The harness does this correctly.
- **GS inline asm errors**: `movl %gs:0x68, %k0` must use 32-bit tmp for `LastErrorValue`. See `probe_teb_peb.c` for the `uint32_t tmp` pattern.
