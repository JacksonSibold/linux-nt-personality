# Golden corpus status (Windows oracle)

Status date: 2026-10-12. Scope: `tests/differential/golden/`, the readers that
consume it, and the build-pin policy. Owner decision resolved: option 3, accepted builds (section 8).

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

## 6. Opt-in dev mode for a host-captured golden (`NTPERS_GOLDEN_BUILD`) - superseded by section 8

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

### 7.2 Patch proposals for the six skipped probes (applied in section 8)

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

## 8. Parameterised probes (haiku/golden3, 2026-10-13 UTC)

The 7.2 proposals were applied to five probes. Each change is in the Windows
branch only (`#ifndef NTPERS_VM`), or the VM-only code is wrapped in
`#ifdef NTPERS_VM`. For every changed probe, `gcc -E -P -DNTPERS_VM` gives the
same output before and after the change. Each probe has its own commit, and the
capture driver (`tests/differential/capture_golden.py`) runs the five in a
sandbox with `--root`.

| Probe | Change | Host result |
|-------|--------|-------------|
| `probe_handles_direct` | `--root` (else `NTPERS_PROBE_ROOT`, else `%TEMP%`). The `C:\Windows\Temp` fallback is gone on Windows. The file is deleted before exit. The probe refuses to start if the file exists. | Ran, rc `0x00000000`. Published (2,919 bytes). |
| `probe_ntstatus_direct` | `--root`. The Windows build no longer writes `C:\tmp\probe_ntstatus_direct.json`. The scratch file is deleted. | Ran, rc `0x00000000`. 95,165 bytes, kept out (64 KiB policy). |
| `probe_path` | `--root` replaces `C:\tmp` in the fuzz prefixes, and the printed path is `<root>`. UNC prefixes need `NTPERS_PROBE_ALLOW_UNC=1`; otherwise each UNC index is `{"skipped":"unc"}` (2,500 of 10,000). The test tree was already deleted on exit. | Ran, rc `0x00000000`. 2.9 MB, kept out. |
| `probe_path_direct` | `--root`. The tree (including the 255-character file) is deleted before exit. UNC opt-in as above (8,191 of 32,767 skipped). | Ran, rc `0x00000000`. 9.2 MB, kept out. |
| `probe_sharemode_direct` | `--root` for the signal, scratch and fallback files, which are deleted. Helpers are children of the probe (its own image), capped by `--max-helpers` (1..64, default 64) and bounded by `--helper-timeout-ms` (default 5000). | Blocked: Application Control refused the launch (`WinError 4551`) before any helper started. Not retried with another binary or location. No golden. |
| `probe_direct_simple` | None. Skipped as "not a portable Windows probe": the file is raw SSN stubs with no Win32 path. | Skipped. |

### 8.1 Defect found while capturing (outside 7.2)

`do_probe` in four probes began with `__asm__ volatile("sub $128, %rsp")`. That
is frame setup for the freestanding VM `_start`, but the Windows build compiled
it too, and it moved `rsp` under the locals of the calling function. On the
first Windows run:

- `probe_handles_direct`: the first `NtCreateFile` returned `0xC0000005`.
- `probe_ntstatus_direct`: the one real `NtCreateFile` returned
  `STATUS_OBJECT_PATH_SYNTAX_BAD`, so every real-syscall case took the failure branch.
- `probe_path_direct`: the open of the subdirectory returned `0xC000003B`, so the
  relative cases ran with a null root.

The asm is now under `#ifdef NTPERS_VM` in all four (`probe_sharemode_direct` has
the same asm before its matrix opens). After the change the same probes give
valid results: handles creates its handles, ntstatus runs 438 real cases with 15
distinct statuses, and every `probe_path_direct` setup call returns success. This
is evidence for the cause, not a separate experiment: a diagnostic binary built
with the asm was refused by Application Control, so it could not be run.

### 8.2 Capture and scrub

- Sandbox: `golden-out/sandbox-<name>` holds the binary, `root/` and `tmp/`. Each
  is removed after its run. Nothing was left under any root (`leftover_under_root`
  is empty for all four runs).
- Scrub: the user name and machine name are replaced (none appeared in the output).
  The printed nt paths use `<root>`.
- Manifest: `source_capture` records the commit (`f544ec1`) and the sha256 of the
  probe-source diff from `354a912`. Each new entry records its binary sha256, which
  differs between builds of the same source (PE timestamps).
- The `windows_build` field of the new probes is the hardcoded `26200.9168`, as in
  the existing label defect. It is recorded in `known_label_defects`.

### 8.3 Lint and remaining gaps

`NTPERS_GOLDEN_BUILD=26200.9457 python tests/harness/lint_integrity.py` exits 1 at
the completeness gate, as in 7.3. The gate stops there, so later gates did not run.
Gaps that remain:

- Sentinels: `probe_path.json` (needs more than 100 KB) and `probe_path_direct.json`
  are absent. Both were captured (2.9 MB and 9.2 MB) and kept out by the 64 KiB
  policy in section 4. Closing this needs that policy decision.
- Captured but not published (64 KiB policy, sha256 in the manifest):
  `probe_sharemode` (4.9 MB), `probe_virtualquery_direct` (1.1 MB),
  `probe_ntstatus_direct` (95 KB), `probe_path` and `probe_path_direct`.
- No output: `probe_sharemode_direct` (blocked by Application Control, see the
  table) and `probe_direct_simple` (not portable).

Unit suite: 567 tests pass (one WSL test skipped). `tools/check_doc_links.py`:
0 broken links. `tools/update_doc_counts.py --write` applied the tracked-file count.

## 8. Accepted-builds policy (option 3), applied 2026-10-13

Owner decision: option 3, accept a list of builds. The owner delegated the
details. Record: `docs/decisions/0073-golden-accepted-builds.md`.

### 8.1 What changed

- `tests/differential/golden/ACCEPTED_BUILDS.json` (schema
  `ntpers-accepted-golden-builds/1`) is the only way a golden build is
  accepted. Each entry has `build`, `directory`, `provenance`
  (`host-captured` or `pinned-reference`), `captured_on`, `accepted_on`,
  `version_json_sha256`, `manifest_sha256`, `rationale` and `known_gaps`.
- Accepted: `26200.9457` (`host-captured`, `windows-26200.9457/`). Its
  `manifest.json` sha256 is `7b96cda9...fd25c400`, and its `version.json`
  sha256 is `d3fb4d19...6907`.
- A directory is accepted only if its build is listed, both file hashes match,
  `version.json` oracle equals the entry's provenance, and the manifest host
  build equals the build. The golden build must also equal the running host
  build. That build-drift guard is unchanged, so a listed build is usable only
  on a host of that build.
- Removed: the `NTPERS_GOLDEN_BUILD` environment opt-in, and the fallback to the
  pinned, `latest` or newest version directory. `dualrun_policy.json` now
  points to the list (`accepted_builds_file`) instead of `windows_golden_build`.
  `dualrun.py`, `dualrun_wrapper.py` and `selftest.py` resolve through the
  list. The old override test file was replaced by
  `tests/unit/test_golden_accepted_builds.py`.

### 8.2 Known gaps (completeness)

A probe listed in `known_gaps` does not fail completeness when its golden is
absent. A gap whose golden is present fails. A criterion whose registry entry
declares `requires_golden` on a gap probe is reported **BLOCKED-ON-GOLDEN** by
`dodrun.py` and `lint_integrity.py`, and it is never passed.

| Probe | Reason code | Why |
|-------|-------------|-----|
| `probe_ntstatus_direct` | `too-large-kept-out` | 95165-byte output. Raw output not on local disk. |
| `probe_path` | `too-large-kept-out` | 2888920-byte output. Raw output not on local disk. Sentinel waived through this gap. |
| `probe_path_direct` | `too-large-kept-out` | 9207661-byte output. Raw output not on local disk. Sentinel waived through this gap. |
| `probe_sharemode_direct` | `unsafe-on-host` | Writes and deletes `C:\tmp\probe_cross_*`. First launch refused by Application Control (WinError 4551). No golden. |

Policy probes without a golden: 4 of 22 (the four above). No other golden is
missing.

### 8.3 Size policy (owner decision, 2026-10-13)

- Gzip when the compressed size is under 512 KiB. `probe_sharemode.json.gz`
  (188600 bytes) and `probe_virtualquery_direct.json.gz` (37159 bytes) are now
  published. Each was scrubbed (no username or machine name in the raw output)
  and checked against its manifest sha256 before and after compression. The
  decompressed sha256 is checked on every lint run.
- Summary sentinel (`ntpers-golden-summary/1`) for outputs that stay out of the
  repo. The checker validates the fields, the manifest `stdout_sha256_raw` and
  size, and, when the full file is local, its sha256. Unit tests cover valid,
  wrong-hash, missing-field and empty-placeholder cases. **No summary has been
  generated yet.** The raw outputs for `probe_ntstatus_direct`, `probe_path` and
  `probe_path_direct` are not in `golden-out/run`. Only their sha256 values are
  recorded in the manifest, so a summary cannot be produced from local data
  without recapturing those probes. They stay as known gaps.

### 8.4 Harness results (2026-10-13)

Commands were run from the worktree with the WinPython interpreter.

- `python tests/harness/lint_integrity.py`: **FAIL**, 4 violations, exit 1. The
  checker accepts `26200.9457` and passes everything else. It rejects these four
  goldens because their `windows_build` is `26200.9168`, a value hard-coded in
  the probe source. The manifest already records this as a known label defect:
  `probe_handles_direct`, `probe_process_thread_direct`, `probe_sync_direct`,
  `probe_virtualquery_direct`. The label is not the host build, so the strict
  build check is correct. Clearing it needs a probe-source change that reports
  the host build, then a recapture. That is not done here and is a separate
  owner decision.
- BLOCKED-ON-GOLDEN report: **0 criteria**. No `tests/harness/registry.yaml`
  entry declares `requires_golden` yet. The known gaps are reported, and the
  mechanism is covered by unit tests.
- `python tests/harness/dodrun.py` narrowed to DOD-SEC-A008, DOD-DRV-B003 and
  DOD-EVI-B006: **refused to run** (integrity lint gate). PASS count: 0. The
  three criteria have no registry entry, so they would be UNIMPLEMENTED even
  with the gate open. See `docs/status/CRITERIA-PROGRESS.md`.
- `python tests/harness/selftest.py`: **refused**, runs 0 tests (lint gate).
- Unit suite: 578 tests, OK (1 skipped, WSL not available).
- `tools/check_doc_links.py`: 0 broken links.

## 9. Recapture on the 26200.9457 host (haiku/goldenfix, 2026-10-13 UTC)

- Probe fix (commit f1b34bf): the Windows build of seven direct probes now
  prints `windows_build` and `windows_version` from RtlGetVersion and the
  registry UBR, not the literal 26200.9168. The `NTPERS_VM` build keeps its
  literal. `probe_virtualquery_direct` no longer writes `\??\C:\tmp` on the
  Windows build (that write is now `NTPERS_VM`-only).
- Capture (commit 83e1d76): all 23 probes in the capture list ran in sandboxes
  with the 60 s timeout, rc `0x00000000`, none blocked, none left files under
  their root. `probe_sharemode_direct` ran (4,942,145 bytes) and is no longer a
  known gap.
- Size policy: gzip for outputs of 64 KiB or more when the gzip is under 512 KiB.
  `probe_path` and `probe_path_direct` compressed under that limit, so no summary
  sentinel was written. Raw outputs are kept in `golden-out/run`, outside the repo.
- `ACCEPTED_BUILDS.json`: the entry's hashes were updated and `known_gaps` is
  empty, because every policy probe now has a golden.
- Lint: `python tests/harness/lint_integrity.py` exit 0, "Integrity lint PASS -
  no violations", 0 blocked criteria. `check_golden_integrity.py` exit 0 with one
  warning: `probe_section_direct` has a golden but no policy entry (not enforced).
- Unit suite: 578 tests OK in the default environment (2 skipped, WSL). With
  MSYS2 forced onto PATH, `test_canaries_are_portable_and_pass` fails because the
  canary commands `true`, `false` and `exit 2` return ERROR. That is an
  environment effect of the PATH override, not a change in this branch.
- `tools/check_doc_links.py`: 0 broken links. `tools/update_doc_counts.py --write`
  updated the tracked-file count.
- Selftest `tests/harness/selftest.py`: FAILS (exit 1). The first test that reads
  dodrun results, `test_unregistered_is_unimplemented`, reports
  `KeyError: 'CRIT-UNREG'` (with MSYS2 on PATH). With the default PATH the first
  two tests pass and `test_gated_with_evidence_can_pass` reports
  `KeyError: 'CRIT-GATED-OK'`. A hand-built copy of the gated fixture run through
  `dodrun.py` gives PASS. The cause inside `selftest.py`'s `run_dodrun` is not
  established. `dodrun.py` also aborts with rc 125 and writes no results when
  `true`, `false` or `exit 2` cannot be run. This is recorded, not fixed.

### 9.1 Merge of haiku/docslint and the SELF criteria (2026-10-13 UTC)

- `haiku/docslint` merged (commit 5da8be2). Conflicts: count lines (regenerated by
  `update_doc_counts.py --write`), and `docs/status/CRITERIA-PROGRESS.md`. The
  goldenfix file keeps that path because the policy docs link to it. The docslint
  milestone file is `docs/status/CRITERIA-MILESTONES.md`.
- Direct docs-lint results on the merged tree (`tools/docs_lint.py --policy --criterion X`):
  - `DOD-SEC-A008`: 0 violations. Its registry entry (`python3 tools/docs_lint.py`) was already on docslint.
  - `DOD-DRV-B003`: 3 violations (required statement missing from README and
    CRITERIA.md; device-support claim without an API subset). Not registered.
  - `DOD-EVI-B006`: 6 violations (claims with no linked bundle). Not registered.
- `dodrun.py` narrowed to these three (a scratch criteria file taken from
  `tests/conformance/criteria.json`, the repo registry, default PATH):
  - `DOD-SEC-A008`: FAIL, exit 9009 ("command not found"). The registry command is
    `python3`, which on this host is the Windows Store alias with no interpreter.
    The registry command was not edited, so no PASS was written.
  - `DOD-DRV-B003`: UNIMPLEMENTED (no registry entry, because its direct check fails).
  - `DOD-EVI-B006`: UNIMPLEMENTED (no registry entry, because its direct check fails).
  - Result: 0 of 3 criteria recorded PASS.
- With MSYS2 forced onto PATH, `dodrun.py` aborts with rc 125 at its canary step.
  The canary `true` returns ERROR there, so the narrowed run used the default PATH.

### 9.2 Harness fixes for section 9 items 1-4 (haiku/dodrun, 2026-10-13 UTC)

Commits: 3e024d7, 232bf26, 75315fd, 2459a1e, 09d3c59.

- Item 1, selftest KeyError. Root cause: two defects in the test helper
  `run_dodrun`, not in dodrun's scoring. (a) Windows paths were written into YAML
  double-quoted scalars; `\U` in `C:\Users` is a YAML escape error, so dodrun
  aborted at registry load without writing a results file (rc 1). (b) `run_dodrun`
  read the newest `results-*.json` in the shared reports directory even when the
  current run wrote nothing, so the stale record of an earlier test produced the
  KeyError. Fixed: paths are emitted with `json.dumps`, and only results files not
  older than the invocation are read. The bash-shebang fixtures could not run on
  Windows (WinError 193, so ERROR instead of their intended state); they are now
  Python scripts run as `[sys.executable, script]`.
- Item 2, canary rc 125. The canaries used `true`, `false` and `sh -c`. Without
  Git's `usr/bin` on PATH they return ERROR and the run aborts with rc 125. They now
  run `sys.executable` with exit codes 0, 1 and 2, through the same `run_command`
  path. Unit tests: the canaries pass with no POSIX binaries on PATH, and fail when a
  known failure is mapped to PASS.
- Item 3, `python3` exit 9009. `resolve_command` mapped `python3` only when
  `shutil.which` returned None. On this host `which('python3')` finds the Store alias
  in `WindowsApps`, so the mapping was skipped and the alias exited 9009. The mapping
  is now unconditional for a first-argument `python3` or `python`. Unit tests cover
  the alias case, platform independence, and an end-to-end `python3` registry entry
  that reaches PASS.
- Also found while fixing item 1: a Windows NTSTATUS exit code (0xC0000005) is
  reported unsigned, so it was FAIL, not ERROR. dodrun now maps codes at or above
  0xC0000000 to ERROR, the same as a POSIX signal.

Verdicts:

- Unit suite, `python -m unittest discover tests/unit`: 672 tests OK (2 skipped, WSL).
- `tools/check_doc_links.py`: 161 files, 386 links, 0 broken.
- `tools/update_doc_counts.py --write`: unit count 664 -> 672 (commit 09d3c59).
- `python tests/harness/selftest.py`: FAILS, exit 1, at `test_golden_integrity_corrupt`
  with `FileNotFoundError: tests/differential/golden/26200.9168/probe_handles.json`.
  The golden tests still use the pre-recapture build directory `26200.9168`; the
  committed golden is `windows-26200.9457`. Those tests write into the real golden
  tree (with backup and restore), so they are not repointed here. Not run: the
  golden tests (`test_golden_integrity_*`, `test_golden_resolution_without_symlink`).
  Run one by one, the other 36 tests give 32 PASS, 3 SKIP (WSL distro Ubuntu absent),
  and 1 FAIL: `test_artifact_identity_stamping` ("bzImage sha256 sidecar missing"),
  a build product that is absent from this worktree. Not fixed.
- `python tests/harness/dodrun.py` narrowed to DOD-SEC-A008 (scratch criteria file,
  scratch reports): **DOD-SEC-A008 PASS** (stdout `DOD-SEC-A008: 0 violation(s)`).
  The process exit 1 comes from the tier gate (the narrowed run has no P1 or P2
  criteria), not from the criterion. Recorded in `docs/status/CRITERIA-PROGRESS.md`.
- DOD-DRV-B003 and DOD-EVI-B006 are not registered and remain open (section 9.1).
