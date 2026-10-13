# Golden corpus status (Windows oracle)

Status date: 2026-10-12. Scope: `tests/differential/golden/`, the readers that
consume it, and the build-pin policy. Owner decision is still open (section 4).

## 1. Expected layout (what the readers actually read)

| Reader | Reads | Requires |
|--------|-------|----------|
| `tests/differential/check_*.c` (`check_ntstatus`, `check_path`, `check_sync`, `check_section`, `check_process_thread`, `check_exception`) | `golden/latest/<probe>.json`, then `$NTPERS_ROOT/tests/differential/golden/latest/<probe>.json` | `latest` present and holding the probe JSON |
| `tests/differential/check_golden_integrity.py` (called from `tests/harness/lint_integrity.py` and `dualrun.py`) | `golden/<pinned build>/`, else `golden/latest`, else newest numeric version dir | `version.json` with `windows_build`; `probe_path.json` > 100 KB; `probe_path_direct.json`; `probe_ntstatus.json`; each policy probe JSON with `windows_build`, `windows_version`, `arch`; volume matching `dualrun_policy.json` |
| `tests/differential/dualrun.py` | `golden/latest` plus policy `windows_golden_build` | `windows_build` in each JSON equal to host build |
| `vm/golden_latest.sh` | creates symlink `golden/latest` -> `golden/26200.9168` | the pinned directory |

Directory naming: readers only accept directory names matching `^\d+(\.\d+)+$`
(`BuildNumber.UBR`, for example `26200.9168`). `version.json` is required in
each directory. Each probe emits one JSON object on stdout, and every object
records `windows_build`, `windows_version` and `arch`.

Policy (`tests/differential/dualrun_policy.json`): `windows_golden_build` is
`26200.9168`, and `windows_version` is `10.0.26200`. The pin is repeated in
`check_golden_integrity.py` as a fallback, and in `docs/harness/OVERVIEW.md`.

## 2. Mismatches found

1. **Pinned build vs host.** The pin is `26200.9168`. This host reports
   `10.0.26200.9457`. Every build guard (`check_golden_integrity.py`,
   `dualrun.py`) rejects a golden whose `windows_build` differs from the host or
   from the pin. A corpus captured here cannot satisfy the pin.
2. **Directory name.** The requested location `golden/windows-<build>/` is not
   matched by any reader. `resolve_golden_dir` only accepts `^\d+(\.\d+)+$`
   names, so a `windows-*` directory is invisible to the integrity gate.
3. **Probes that cannot be captured safely here.** See section 3.
4. **Volume gates.** `check_golden_integrity.py` requires `probe_path.json` and
   `probe_path_direct.json`, plus a sentinel count for `probe_process_thread`.
   Those two probes are skipped (section 3), so the gate cannot pass on this
   corpus even after the build question is settled.

## 3. Capture plan and skipped probes

Result of the capture run (`tests/differential/capture_golden.py`, host
`10.0.26200.9457`, 2026-10-12 UTC): 24 probe sources, **16 ran, 0 blocked,
0 crashed, 0 timed out, 0 failed to build, 8 skipped**. Application Control
did not block any binary. Probes are built with the MSYS2 UCRT64 gcc
(`gcc 16.2.0`, x86_64) into `C:/Users/Jackson/repos/golden-out`, outside the
repository. The text outputs and manifest are in
`tests/differential/golden/windows-26200.9457/`. Two outputs are too large to
commit (`probe_sharemode` 4.9 MB, `probe_virtualquery_direct` 1.1 MB). They are
listed by sha256 in the manifest.

Three of the 16 (`probe_virtualquery_direct`, `probe_sync_direct`,
`probe_process_thread_direct`) print `"windows_build": "26200.9168"`. The probe
source hardcodes that string. It is not the host build, and the captured
output is not relabelled.

Rules applied: a probe is skipped if its source (a) issues raw `syscall` stubs
outside an `#ifdef NTPERS_VM` guard, (b) writes or deletes files outside
`%TEMP%` (for example under `C:\tmp` or `C:\Windows\Temp`), (c) changes registry
state, or (d) feeds network paths (UNC) into file APIs.

Skipped (reason in brackets):

- `probe_direct_simple.c`: unguarded raw SSN stubs (`NtWriteFile` 0x08,
  `NtTerminateProcess` 0x2c). Not a Win32 probe.
- `probe_section_direct.c`: unguarded raw SSN stubs.
- `probe_handles_direct.c`: raw SSN stubs outside the guard; writes
  `C:\tmp` and `C:\Windows\Temp`.
- `probe_ntstatus_direct.c`: raw SSN stubs outside the guard; writes
  `C:\tmp\probe_ntstatus*`.
- `probe_path.c`, `probe_path_direct.c`: path fuzz includes `C:\tmp`, UNC
  `\\server\share` and `\Device\...` prefixes passed to create/open APIs. Can
  create files outside `%TEMP%` and can start SMB connections.
- `probe_sharemode_direct.c`: writes and deletes `C:\tmp\probe_cross_*`, spawns
  copies of itself.
- `probe_registry.c`: creates, writes and deletes keys under `HKCU`.

Section 7 supersedes this list. `probe_section_direct` and `probe_registry`
were audited and captured later (section 7), which leaves 6 skipped.

## 4. Proposed policy decision (for owner sign-off, not applied)

The corpus must record the build it was captured on. It must not be relabelled
as `26200.9168`. Three options:

- **A. Keep the pin, capture on 26200.9168.** Needs a machine at that build.
  Nothing in this repo can do it, and the pin is unchanged.
- **B. Re-pin to the host build (`26200.9457`).** Changes the oracle, and the
  VM tables (`nt_io.c`, `io.c`, `kshim.c`, `syscall_table.def`) were calibrated
  against 26200.9168. Every divergence would have to be re-audited.
- **C. Allow a set of pins.** Replace `windows_golden_build` with a list
  (`windows_golden_builds`). A golden is accepted if its `version.json` build is
  in the list, and `dualrun.py` reports which build each comparison used. Build
  drift between 9168 and 9457 then appears as a separate, reviewable finding.

Recommendation: C, with directory names `<BuildNumber.UBR>` so the readers
find them, and `version.json` carrying `"oracle": "host-captured"` and the
capture manifest hash. Until the owner decides, the corpus stays in
`golden/windows-26200.9457/`, which no reader consumes.

## 5. Harness status

Run on 2026-10-12 with the committed corpus (`golden/windows-26200.9457/`).
Nothing was changed to make a gate pass: the policy, the checker and the
selftest are untouched.

### 5.1 `tests/harness/lint_integrity.py`: FAIL, 2 violations, one root cause

Both violations come from `check_golden_integrity`:

- `no valid golden build directory under .../golden (pinned 26200.9168 missing, latest missing, no version dirs found)`
- `unable to resolve golden build directory for pinned 26200.9168`

Cause: the checker only accepts `<BuildNumber.UBR>` directories, and it needs
the pinned `26200.9168` directory (or a `latest` link) with `probe_path.json`
(over 100 KB), `probe_path_direct.json` and `probe_ntstatus.json`. The corpus
has none of these, because `probe_path` and `probe_path_direct` are skipped
for safety. The other 10 lint checks report 0 violations.

### 5.2 `tests/harness/selftest.py`: does not run

`main()` stops at the lint gate and returns 1 before any test. Each
`test_*` function was therefore called directly, from a script outside the repo
(`golden-out/run_selftests_direct.py`), against this tree: **42 functions,
24 pass, 18 fail**. Of the 24 passes, 4 are skips reported as passes:
`DLL staleness` (`ntdll.dll` absent), `sentinel test` and `stale test` (golden
`26200.9168` absent), and `initramfs staleness` (files absent). These four prove
nothing here.

Failures, grouped by cause:

| Count | Tests | Cause |
|-------|-------|-------|
| 10 | `test_crashing_test_is_error`, `test_hanging_test_is_timeout`, `test_regression_exits_nonzero`, `test_missing_result_is_error_not_pass`, `test_positive_control_canary`, `test_unregistered_is_unimplemented`, `test_gated_with_evidence_can_pass`, `test_gated_without_evidence_is_gated`, `test_timeout_ignores_stale_file`, `test_exception_not_swallowed` | `KeyError 'CRIT-*'`: `dodrun.py` writes no record for the criterion. This matches `docs/harness/OVERVIEW.md`, which says `dodrun.py` cannot parse `tests/harness/registry.yaml` without PyYAML. PyYAML is not installed here (`ModuleNotFoundError: yaml`), and installing it is outside this task. The record was not inspected further. |
| 3 | `test_freshness_missing_manifest_is_fail`, `test_freshness_write_without_rebuild_is_refused`, `test_freshness_write_without_token_is_refused` | `freshness.sh` needs WSL, which is not installed on this host. |
| 1 | `test_artifact_identity_stamping` | No built bzImage sidecar (kernel build absent). |
| 4 | `test_golden_integrity_build_mismatch`, `test_golden_integrity_corrupt`, `test_golden_integrity_volume_mismatch`, `test_golden_resolution_without_symlink` | `FileNotFoundError` on `golden/26200.9168/*.json`, or the checker fails before the asserted check (`golden resolution` fails with `host build 26200.9457`, pinned `26200.9168`). |

### 5.3 What this corpus does and does not establish

- It establishes that the 16 probes run on this host and produce valid JSON,
  and it gives an oracle for host `26200.9457`.
- It does not satisfy the pinned-build gate. Relabelling it as `26200.9168`
  would be false, and the checker's build guard would reject it anyway
  (`version.json windows_build` must equal the host).
- It does not satisfy the readers' `latest` or `windows-*` lookup (section 2).

### 5.4 Next decision

Owner decision on section 4 (options A, B or C). Under option C the checker
needs a change, and that change needs review. Until then, lint stays at 2
violations and selftest does not start.

## 6. Opt-in dev mode for a host-captured golden (`NTPERS_GOLDEN_BUILD`)

Added 2026-10-12 to `tests/differential/check_golden_integrity.py`. This is an
explicit developer opt-in. It is **not** a policy change: the pin stays
`26200.9168`, `dualrun_policy.json` is unchanged, and the default is strict.

- **Unset or equal to the pin** (default): unchanged. Only the pinned build is
  accepted.
- **Set to another build**: the checker accepts `golden/windows-<build>/` only
  when all of these hold. The build equals the running host build. The
  directory is named `windows-<build>`. `version.json` has
  `"oracle": "host-captured"` and `windows_build` equal to the build.
  `manifest.json` has the `ntpers-golden-manifest/` schema and `host.build`
  equal to the build. Any other value is a FAIL with exit 1.
- The opt-in changes only which build the per-probe and `version.json` build
  checks compare against. It does not relax the completeness sentinels, the
  policy volume gate, the staleness check, or the per-probe build check.

Tests: `tests/unit/test_golden_integrity_override.py` (14 cases: strict default
rejects, opt-in accepts, and each rejection path).

Effect on the committed corpus: with `NTPERS_GOLDEN_BUILD=26200.9457` the
provenance check passes and the checker still exits 1. Its completeness gate
needs `probe_path.json` and `probe_path_direct.json`, which section 3 says were
skipped, and 8 of the 22 policy probes have no golden in
`golden/windows-26200.9457/`. Closing that needs an owner decision on the
policy (section 4), not a wider opt-in.

## 7. Confined-probe pass (2026-10-13 UTC)

Scope: the 8 probes skipped in section 3. Each source was read in full before
any decision. The rule for a capture is that the probe must be provably
confined: it writes or deletes only inside `%TEMP%` (or a key under
`HKCU\SOFTWARE\ntpers_probe_*` that it deletes), with no network, no services,
no process spawning outside itself, no system-wide state and a bounded runtime.
A probe that does not meet this stays skipped, with a patch proposal below. No
probe source was changed.

### 7.1 Decisions

| Probe | Decision | Evidence (Windows build path, `#else` branch) |
|-------|----------|-----------------------------------------------|
| `probe_section_direct` | **Ran**, rc `0x00000000` | Raw stubs are under `#ifdef VM_BUILD`, which the Windows build does not define. The skip reason "no NTPERS_VM guard" was wrong. The Windows build resolves ntdll by name, creates one `GetTempFileNameA` file in `%TEMP%` and deletes it. Its only other file access is a read-only open of `C:\Windows\System32\ntdll.dll`. |
| `probe_registry` | **Ran**, rc `0x00000000` | Creates and deletes one key, `HKCU\SOFTWARE\ntpers_probe_<pid>_<tick>`. It opens `HKLM\SOFTWARE` with `KEY_WRITE` as an access check only, and writes nothing there. It makes no file, network or process calls. The `HKCU\SOFTWARE\ntpers_probe_*` list was empty before and after the run. |
| `probe_direct_simple` | Skipped | The whole file is raw `syscall` stubs with no Win32 path and no guard. The hardcoded SSNs (`0x08`, `0x2c`) are build-specific, so on 26200.9457 they may reach a different service, and the calls bypass user-mode API hooks. No small change makes it a Windows probe. |
| `probe_handles_direct` | Skipped | Stubs are under `#ifdef NTPERS_VM` (the guard is present, so the old reason was wrong). The Windows build creates `\??\C:\tmp\probe_hdl_direct.tmp`, falls back to `C:\Windows\Temp`, and never deletes the file. Unguarded `write_file_nt` to `\??\mnt\share\...` runs in both builds. |
| `probe_ntstatus_direct` | Skipped | Stubs are under `NTPERS_VM` (guard present). The Windows build creates or overwrites `\??\C:\tmp\probe_ntstatus.tmp` (never deleted) and writes `\??\C:\tmp\probe_ntstatus_direct.json`. |
| `probe_path` | Skipped | The fuzz calls `NtCreateFile` with `\\server\share\...` and `\\?\UNC\server\share\...` (prefixes 6 and 7). Those can start SMB connections to a host named `server`, and so can send
credentials to it. Not run, so this is a reading of the source, not an observed
network event. It also opens `C:\tmp\...` and `\??\C:\tmp\...` outside `%TEMP%`, read-only. |
| `probe_path_direct` | Skipped | Same UNC fuzz as `probe_path`. The Windows build creates the `\??\C:\tmp\probe_path_direct` tree and two files, never deleted, then makes `write_file_nt` attempts to `\??\mnt\share` and `C:\tmp`. |
| `probe_sharemode_direct` | Skipped | Deletes and creates fixed `C:\tmp\probe_cross_{ready,done,child_status}.tmp` names, outside `%TEMP%`. Its fallback is `C:\tmp\probe_shr_direct.tmp`. It spawns 64 helper copies of itself, each with up to 5 s and 5 s waits, so the runtime was not measured and may exceed the 60 s limit. |

Both captured probes ran in fresh sandboxes (`golden-out/sandbox-<name>`, with
`TEMP`/`TMP` pointed at a fresh directory inside it). The sandbox held only an
empty `tmp` directory after each run, and both were removed. Output was scrubbed
with the rules in the manifest and checked for the username and machine name.
Neither output contains either. The raw sha256, the published sha256 and the
binary sha256 are in the manifest.

### 7.2 Patch proposals for the six skipped probes (not applied)

Each proposal is the smallest change that would make the probe confined. Each
one changes output, so the golden has to be captured again after the change.
Each proposal keeps the Windows build, takes a `--root <dir>` argument that
defaults to `GetTempPathA()`, and deletes everything it creates before exit.

- **`probe_handles_direct`**: build the file path from `<root>`, drop the
  `C:\Windows\Temp` fallback, and call `DeleteFileA` before exit. Put the
  `\??\mnt\share` writes under `#ifdef NTPERS_VM`.
- **`probe_ntstatus_direct`**: the same, for `<root>\probe_ntstatus.tmp`. Drop
  the write of `probe_ntstatus_direct.json` to `C:\tmp`, and put the
  `\??\mnt\share` writes under `#ifdef NTPERS_VM`.
- **`probe_path`**: replace the `C:\tmp\probe_path_direct\` prefixes (0, 1, 3)
  with `<root>\`. Remove the UNC prefixes (6, 7). The generated set changes from
  10000 cases, so this is a new golden, not comparable to the existing one.
- **`probe_path_direct`**: as `probe_path`, and create the directory tree under
  `<root>`, deleting it before exit. Put the `\??\mnt\share` and `C:\tmp` writes
  under `#ifdef NTPERS_VM`.
- **`probe_sharemode_direct`**: put the signal files under `<root>` (the
  `C:\tmp\probe_cross_*` and `probe_shr_direct.tmp` paths). Measure the runtime
  first. If 64 spawns do not fit in 60 s, add a `--cross-only` flag and capture the
  cross arm separately.
- **`probe_direct_simple`**: there is no safe Windows form. Keep it VM-only and
  remove it from the Windows capture list, which it already is not on
  (`capture_golden.py` never lists it in `RUN`).

### 7.3 Integrity lint after this pass

Command: `NTPERS_GOLDEN_BUILD=26200.9457 python tests/harness/lint_integrity.py`.
Result: exit 1, one violation. The checker resolves
`golden/windows-26200.9457/` and rejects it as incomplete (missing expected probe
golden files). The provenance check passes, and the checker stops at this gate, so
later gates (the volume check and the per-probe build check) were not reached.

Completeness gaps that remain:

- Sentinels: `probe_path.json` (needs more than 100 KB) and
  `probe_path_direct.json` are absent. Both are skipped (7.2).
- Policy probes without a golden file: 7 of 22. Five are skipped:
  `probe_handles_direct`, `probe_ntstatus_direct`, `probe_path`,
  `probe_path_direct` and `probe_sharemode_direct`. Two were captured but are
  kept out of the repo for size, and listed by sha256 in the manifest:
  `probe_virtualquery_direct` (1.1 MB) and `probe_sharemode` (4.9 MB). This
  supersedes the count of 8 in section 6, because `probe_registry` is now
  captured.

Closing the sentinels and the five skips needs the patches in 7.2, a re-capture,
and the policy decision in section 4. `probe_direct_simple` is not
Windows-capturable at all (7.2), so it is not counted in the policy gap.
