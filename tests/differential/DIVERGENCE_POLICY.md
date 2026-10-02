# Divergence Ranking Policy — Windows Differential Dual-Run

**Purpose:** Decide which observed differences between real Windows (build 26200.9168) and our OS are real behavioural bugs (must fix) vs legitimate environment noise (must ignore). Getting this policy wrong either drowns the signal in false positives (everything fails) or hides real bugs (too loose). This document is the explicit, reviewable policy. `dualrun.py` encodes it as `dualrun_policy.json`.

**Oracle:** The same probe PE (mingw `x86_64-w64-mingw32-gcc`, C11, no CRT exotic deps, dynamic `GetProcAddress` on `ntdll.dll`) is executed natively on Windows (`cmd /c "probe.exe > out.json"`) and inside our VM via `binfmt_pe`/`tests/kernel` harness, with JSON captured over 9p `share/.dodresult/`. The two JSON trees are compared structurally.

**Ranking scale (S1..S4):**
- **S1 Critical** — breaks real software. Wrong error code, wrong `MEMORY_BASIC_INFORMATION.State/Protect/Type`, wrong share-violation decision, wrong exception mapping. Must be zero at release for P0/P1 criteria.
- **S2 Major** — Wrong `AllocationProtect`, wrong `RegionSize` coalescing, wrong `GrantedAccess` bits, wrong exact-size check, wrong reserved-name handling. Must be zero for declared P1.
- **S3 Minor** — Timing/ordering/tolerance band violations (timeout elapsed, frame count). Allowed within stated band.
- **S4 Noise / Legit diff** — Absolute addresses, PIDs, timestamps, handle numeric values, version strings — must be ignored or normalized, never fail.

---

## 1. Fields that MUST match EXACTLY (S1/S2)

| Field / probe | Why exactly equal | Severity if mismatched |
|---|---|---|
| `NTSTATUS` values (`ntstatus_hex`, `ntstatus`, `status`, `second_st_hex`) — every probe that records a status (probe_ntstatus, probe_sharemode matrix, probe_path per-path status, probe_handles NtQueryObject status, probe_virtualquery VirtualProtect success mapping to NTSTATUS) | Callers switch on exact code (`if (st==STATUS_SHARING_VIOLATION)`). `GetLastError` translation table also depends on exact input. | **S1** |
| `GetLastError` / `dos` / `dos_via_Rtl` / `lasterror` — `RtlNtStatusToDosError` output and `GetLastError()` after a failure | Many apps check `GetLastError()==ERROR_ALREADY_EXISTS` etc. Off-by-one breaks installers. | **S1** |
| `GrantedAccess` mask (`nt_query_object[0].GrantedAccess`, `duplicate_handle.compare`, `ObjectBasicInformation.GrantedAccess`) | Security-relevant. Mask must equal `GENERIC→specific` mapping plus `SYNCHRONIZE` etc. | **S1** |
| `Structure sizes` — `retLen` for fixed-size info classes, `RegionSize` for MEMORY_BASIC_INFORMATION, `retLen` for ObjectBasic/Type | Size-probe idiom depends on exact required length. | **S1** |
| `MEMORY_BASIC_INFORMATION` fields `State`, `Protect`, `Type`, `AllocationProtect`, `RegionSize`, `BaseAddress` relative coalescing (but not absolute value — see noise) | Coalescing rule `nt_vad_attrs_match` is load-bearing for `VirtualQuery` consumers (debuggers, allocators). | **S1** for `State`/`Protect`/`Type`; **S2** for `AllocationProtect` / `RegionSize` exact coalesced size |
| Share-mode matrix `ntstatus_hex` per cell (`matrix[].ntstatus_hex`, `targeted_examples[].second_st_hex`) | Rule `(new_desired & ~existing_share)==0 && (existing_desired & ~new_share)==0` is a security/compatibility gate. | **S1** |
| `ObjectTypeInformation.type` string (`"File"` vs `"Event"` etc) | Type name is observable via `NtQueryObject`. Apps branch on it. | **S2** |
| `HandleCount`/`PointerCount` counts (exact integer) | Reference counting visible. | **S2** |
| `Exact-size check` results (`process_basic_info_size_probe`, `thread_basic_info_size_probe`: only `Length==48` succeeds) | Windows requires exact length for `ProcessBasicInformation`/`ThreadBasicInformation`. | **S1** |
| Exception mapping (`ExceptionCode`, `ExceptionInformation[0]` for read/write/exec, `STATUS_GUARD_PAGE_VIOLATION` vs `STATUS_ACCESS_VIOLATION`) | SEH handlers switch on code. | **S1** |
| Success-vs-failure boolean (`success`, `handle_valid`, `result=="success"`) where derived from NTSTATUS | If one side says success and the other fails for same inputs, bug. | **S1** |
| `SectionOffset` alignment error `STATUS_MAPPED_ALIGNMENT` (probe_section) | Security: misaligned views must be rejected. | **S1** |
| Path NTSTATUS for reserved/dots/`\\?\`/`..` (probe_path) | Loader/path handling. | **S1** |

**How harness enforces:** JSON diff does deep equality on these fields after normalizing addresses/handles. Any mismatch → S1/S2 divergence record with `field`, `windows`, `vm`, `severity`.

## 2. Fields that MAY differ legitimately — normalized / ignored (S4)

These vary by run, by ASLR, by process tree, or by environment, but the *relation* may still be checked:

| Field | Why it differs | What harness checks instead |
|---|---|---|
| Absolute addresses: `reserve_base`, `BaseAddress`, `AllocationBase`, `AllocationBase`, `reserve_base`, `query.*.BaseAddress`, `teb`, `peb`, `ImageBaseAddress`, `Ldr`, `ProcessHeap`, etc. | ASLR, heap randomization, loader base randomization. Windows and our VMA layout intentionally differ. | **Ignored:** stripped before diff. For coalescing, we compare *relative* offsets (`BaseAddress - reserve_base`) and *RegionSize*/*State* sequence, not absolute. |
| `handle` numeric values (`handles[].handle`, `duplicate_handle.src`, `dst`) | Handle table index allocation differs (our `index<<2`, Windows handle value space includes kernel pool). Both must be multiples of 4, sequential, not reused while live — but numeric equality not required. | **Masked:** check `value %4==0`, monotonic `+4` sequential fresh allocation, not `==`. Pseudo-handles `-1`/`-2` *are* exact (they are constants). |
| `PIDs/TIDs`, `GetCurrentProcessId` in path templates (`testDirA`) | Per-run. | **Masked:** pattern check (`ntpers_path_<digits>` equivalent) not literal. |
| `windows_build`, `windows_version`, `timestamp`, `arch` (except major arch check) | Build string is oracle identity, not behavioral. VM may report our build (or no registry). | **Per-probe tolerance:** `windows_build` must equal `26200.9168` on Windows side; VM side may be `0.0` or missing — recorded as `environment_mismatch` S4 not S1. The `arch` must be `x64` exactly (S1 if not). |
| `GetProcessHandleCount.count` absolute number (63 vs 69) | Depends on how many handles the probe opened plus inherited CRT handles. | **Band:** count must be `>= min_expected` (probes open 4 handles plus stdio → >=5). Exact equality not required, but sudden drop to 0 is S2. |
| `dos_device_C` string exact volume (`\Device\HarddiskVolume3` vs `HarddiskVolume1`) | Volume number is host-dependent. | **Masked:** check prefix `\Device\HarddiskVolume` plus correct suffix `\Windows\System32\ntdll.dll`. |
| `name`/`description` strings that embed temp paths | Temp path differs per machine/user. | **Masked:** if field contains `Users\` or `hdl` or `tmp`, compare outcome not string. |
| Free-region `RegionSize` for arbitrary free address (`free_query.RegionSize`, `after_release.RegionSize` beyond reservation) | Free-region coalescing includes heap allocations that differ per run. For `free_query`, Windows reports `258048` for one extra free gap, but any positive multiple of 64K is plausible. | **Band:** must be `State==MEM_FREE (65536)` and `Protect==1` and `RegionSize >0` and `RegionSize % 4096==0` — not exact numeric equality. For `after_release`, must be `MEM_FREE` and covers at least the 64K reservation. |
| Performance counters: `timeout_ms` elapsed (`timeout.100ms -> 110ms`), performance band `launch cold ≤3× twin` | Scheduling jitter. | **Tolerant:** ±10ms or ±15% whichever larger; harness encodes `timing_tolerance_ms: 15` and `timing_tolerance_ratio: 0.15`. |

**Implementation:** Before diff, harness runs `normalize_windows_json` and `normalize_vm_json` that:
- Replaces hex address fields matching `/^0x[0-9A-Fa-f]+$/` or `/"0x.*"/` with `"ADDR"` placeholder if field name in `address_fields`.
- Replaces `handle` values with `HANDLE` if field in `handle_fields`, while preserving `mod4`/`value%4` check.
- Strips `timestamp`, `windows_build` (kept separately for oracle identity), `dos_device_C` exact volume, temp paths.

## 3. Timing-tolerant fields (S3) — with stated band

| Field | Probe | Band | Verdict |
|---|---|---|---|
| `timeout_ms` elapsed for `WaitForSingleObject` 100ms (probe_sync: `timeout_ms`) | probe_sync | **Expected 100ms ±15ms** (Windows measured 110ms). Harness allows `95..125ms`. Outside → S3 divergence, not S1. | S3 |
| `WaitForMultipleObjects` timeout for `all_timeout` 100ms case | probe_sync | Same ±15ms | S3 |
| `launch cold` time ratio (EXE P2 criteria, not in differential probes yet) | future | `≤3.0×` twin time, measured median over 3 runs | S3 |
| `allocation`/`release` success latency not measured in current probes | — | Not compared | — |

**Reasoning:** Windows scheduling on a loaded Windows 11 host introduces ~5-10ms jitter for 100ms waits (observed 110ms). Failing a test for 102ms vs 110ms would be noise. S3 bands are intentionally narrow enough to catch “hung instead of timed out” (would be >> band, e.g., 500ms or infinite) but wide enough to pass healthy systems.

## 4. Ranking and PASS/FAIL per probe

Each probe has a **per-probe tolerance policy** (see `dualrun_policy.json`). The overall probe verdict:

- `PASS` if zero S1 divergences, and S2 count ≤ allowed (`s2_allowed` per probe, 0 for P0 probes), and S3 within band, and no VM crash/missing_service for *implemented* services.
- `FAIL` if any S1, or S2 > allowed, or S3 outside band, or VM crashed where Windows succeeded (S1).
- `VM_MISSING_SERVICE` (still FAIL for DoD mapping, but categorized) if VM returns `STATUS_NOT_IMPLEMENTED` / `missing_service` / `error` where Windows succeeded — this is still an S1 behavioural difference (service not yet implemented), but it is reported distinctly so the work-list is honest and criteria remain UNIMPLEMENTED.

**Per-probe `s2_allowed` and notes:**

- `probe_ntstatus`: S1=0, S2=0 (table must be 60/60 exact). Currently known 18 mismatches → FAIL, S1.
- `probe_teb_peb`: S1=0 (offsets must be exact). Known PASS.
- `probe_virtualquery`: S1=0 coalescing State/Protect/Type exact, S2=0 RegionSize exact for known reservation (free beyond reservation is S4). Known PASS for core, free S4.
- `probe_handles`: S1=0 for GrantedAccess/mapping, handle mod4 sequential, DuplicateHandle semantics; S2=0 for HandleCount. Known PASS with probe artifact noted.
- `probe_sharemode`: S1=0 for all 7744 matrix cells exactly equal; sampled PASS, exhaustive pending.
- `probe_peloader`, `probe_sync`, `probe_process_thread`, `probe_section`, `probe_path`, `probe_exception`: each has S1=0. Currently P0 bugs in guard-page, section alignment, path dots → FAIL (S1).

**Governance for registry.yaml:** A `windows-differential` criterion may be mapped to `PASS` only if its associated dual-run probe(s) are `PASS` at the criterion's stated volume/threshold (e.g., 224 `windows-differential` criteria need plumbing to *prove* full-volume). `EXT-GATED` stays `GATED`. Never edit `criteria.json`. Honest mapping: a large divergence list is Success, because it converts unknown unknowns into a work-list.

