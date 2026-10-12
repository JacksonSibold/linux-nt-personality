# Conformance harness overview

Status as of 2026-10-12, branch `haiku/harness`, Windows 11 host. This document
describes how a conformance run works, according to the code in
`tests/harness/`, and records what was actually executed on this host. Where a
statement is a limitation rather than a design intent, it says so.

## Components

| File | Role |
|------|------|
| `tests/harness/dodrun.py` | Conformance runner. Reads criteria and the registry, runs each mapped test, writes results and summary. |
| `tests/harness/registry.yaml` | Maps criterion IDs to test commands (13 entries). |
| `tests/conformance/criteria.json` | The criteria set (423 entries) that dodrun iterates over. |
| `tests/harness/lint_integrity.py` | Structural lint. Runs first in dodrun and in selftest; a failure refuses the run. |
| `tests/harness/selftest.py` | Anti-gaming self-tests (42 functions). Its `main()` also runs the lint first. |
| `tests/harness/freshness.py` | Freshness gate helper (`check_freshness`, `require_fresh`). Not imported by any other module. |
| `tests/harness/dualrun_wrapper.py` | Verifier for dual-run (Windows vs VM) summaries. Has its own copy of the freshness gate. |
| `tests/harness/vmtest.py` | Glue from dodrun to `vm/runvm.sh` for in-VM tests. Has its own freshness gate copy. |
| `tests/harness/README.md` | Governance rules in prose. Some of its claims are stale; this document supersedes it where they differ. |

Make targets (`build/Makefile`): `make -C build conformance` runs dodrun,
`make -C build test` runs selftest.

## Dependencies

* Python 3 standard library for everything except the registry.
* **PyYAML** is required to parse `registry.yaml` (`load_yaml` raises
  `RuntimeError` if it is missing). It is not declared anywhere in the repo, and
  the CI workflow does not install it. No minimal YAML parser exists in the repo.
  JSON registries (`.json`) load without PyYAML.
* Freshness checks need `bash` (Linux) or WSL with an `Ubuntu` distribution
  (Windows). The VM-based criteria additionally need the built kernel and
  initramfs under `vm/` and `$HOME/ntpers/linux-6.18`.

## Inputs

### criteria.json

A JSON list. dodrun reads these keys per entry:

| Key | Use |
|-----|-----|
| `id` | Criterion identifier, for example `DOD-ARCH-A002`. Required. |
| `tier` | `P0`..`P3`; defaults to `P3` when absent. |
| `dependency_class` | `EXT-GATED` triggers the evidence check. Defaults to `EXT-DOC`. |

Current distribution: EXT-DOC 227, SELF 131, EXT-RE 48, EXT-GATED 17. Tiers:
P0 143, P1 175, P2 89, P3 16. Other keys (`criterion`, `threshold`, `source`,
`verification`, `status`, `gated_note`, ...) are informational to dodrun.

### registry.yaml

A mapping under a top-level `tests:` key (a flat mapping also loads). Each key is
a criterion ID. Each value is one of:

* a dict with the keys below, or
* a bare string, which is shorthand for `{command: <string>}`.

| Key | Meaning |
|-----|---------|
| `command` | String or list of strings. `null`, `""` or `"unimplemented"` mean the criterion is UNIMPLEMENTED. Only `command[0]` is path-expanded: if `project_root()/command[0]` exists it is used. |
| `timeout` | Seconds for this entry. Default is the `--timeout` flag (120). |
| `cwd` | Working directory. Defaults to the repo root. A relative value is resolved against the process working directory, not the repo root (see Known gaps). |
| `skip` | `true` makes the criterion SKIP, with `skip_reason` as the note. |
| `evidence` | Override path for an EXT-GATED evidence file. |

The current registry has 13 entries, all `DOD-*`. Ten are `command: unimplemented`
(DOD-NT-A001, B001, DOD-PE-A010, B010, DOD-NT-A002, B006, A003, B004,
DOD-API-A006, B005). Three are real commands, each `["python3",
"tests/kernel/harness/<suite>.py"]`: DOD-ARCH-A001 (provenance), DOD-ARCH-A002
and DOD-ARCH-B001 (arch). Those three run the in-VM kernel harness, so they can
only pass on a machine where the VM build exists. The comment block also lists
dual-run wrapper entries, which are commented out and not active.

Criteria with no entry at all are UNIMPLEMENTED, never PASS.

## How a run works

`dodrun.py main()`, in order:

1. **Lint gate.** Runs `lint_integrity.py` as a subprocess. On non-zero exit it
   writes `results-<UTC>.json` with `results: []` and a `harness_integrity`
   block containing the lint output, then returns **1**. No criteria run.
2. **Load.** Reads `--criteria` (default `tests/conformance/criteria.json`) and
   `--registry` (default `tests/harness/registry.yaml`).
3. **Positive controls.** Runs `true` (expect PASS), `false` (expect FAIL) and
   `sh -c "exit 2"` (expect UNIMPLEMENTED) through the same `run_command` path.
   Any mismatch writes a record and returns **125**. These commands are POSIX
   binaries; they are found on Windows only when Git for Windows is on PATH.
4. **Evaluate each criterion** (`evaluate_criterion`), in this order:
   1. If `dependency_class == EXT-GATED`, verify the evidence file (see below).
      Failure gives **GATED** and the test is never executed.
   2. No registry entry gives **UNIMPLEMENTED** (`no test registered`).
   3. `skip: true` gives **SKIP**.
   4. A null/`unimplemented` command gives **UNIMPLEMENTED**.
   5. `python3` or `python` is replaced by `sys.executable` when that name is not
      on PATH (`resolve_command`). Relative `command[0]` is expanded to the repo.
   6. The command runs with the entry's timeout. Exit codes map to states:

   | Exit | State |
   |------|-------|
   | 0 | PASS |
   | 2 | UNIMPLEMENTED |
   | 3 | SKIP |
   | 124, 137 | TIMEOUT |
   | 125 | ERROR |
   | negative (signal) | ERROR |
   | any other non-zero | FAIL |
   | subprocess timeout | TIMEOUT |
   | executable not found | ERROR |

   A verified EXT-GATED criterion still has to PASS its registered test.
5. **Roll up.** Per tier, `pass_rate = PASS / total`. Gates: P0 >= 100%, P1 >= 98%,
   P2 >= 85%, P3 best effort (no gate).
6. **Regressions.** Compares with `--previous-run`, or the newest
   `results-*.json` in the reports directory (lexicographic order of the UTC
   timestamp). The state ranking is PASS < FAIL < ERROR < TIMEOUT < SKIP <
   UNIMPLEMENTED < GATED; any move to a higher rank is a regression, any move to
   a lower rank an improvement.
7. **Write outputs** and print the summary.
8. **Exit code.** 1 if any regression; 1 if a tier gate is missed and
   `--no-gate-exit` is not given; otherwise 0.

Note on step 6: an integrity-failure record (`results: []`) becomes the baseline
for the next run, so that run reports no regressions.

### EXT-GATED evidence

`verify_evidence` accepts `<root>/evidence/<ID>.evidence.json` (or the entry's
`evidence:` path). The file must be JSON with keys `artifact_path` and
`artifact_sha256`. A relative `artifact_path` is resolved against the repo root.
The check passes only if the artifact exists and its SHA-256 equals the recorded
value (case-insensitive). Any failure gives GATED with the reason in `note`.
Other keys in the README example (`obtained_date`, `controlling_party`,
`description`) are not checked by the code.

## Outputs

Written under `--reports-dir` (default `<root>/reports/`, gitignored), named
with a UTC timestamp `%Y%m%dT%H%M%SZ`:

* `results-<ts>.json`: `run_id`, `project_root`, `criteria_file`,
  `registry_file`, `previous_run`, `summary` (counts per state, tier roll-up),
  `regressions`, `improvements`, `gates_met`, `harness_integrity`
  (`positive_control` plus reason, or the lint output on lint failure), and
  `results[]` (each with `id`, `tier`, `dependency_class`, `state`, `exit_code`,
  `stdout`, `stderr`, `duration_seconds`, `note`).
* `summary-<ts>.txt`: human-readable version: state counts, tier roll-up,
  REGRESSIONS, IMPROVEMENTS and `GATES_MET`.

## What lint_integrity.py checks

It is a set of text-pattern checks over the repo. Output goes to stderr; exit 0
is clean, 1 means violations. Each check was run on this host (see results).

| Check | What it asserts | Limits |
|-------|-----------------|--------|
| `check_policy` | `tests/differential/dualrun_policy.json` exists, parses, and every probe has `s1_allowed` and `s2_allowed` equal to 0 (a missing key counts as 0). | Only those two keys. |
| `check_measured_assignment` | No guest script (`tests/kernel/guest/*.sh`) assigns NT_SYSCALLS, HOST_SYSCALLS, VOLUME, TRANSITIONS_VERIFIED or BINFMT_HITS from `$SUCCESS` or `$RC`. | Fixed regex list. |
| `check_hardcoded_status` | Guest scripts have no literal `"status":"PASS"`/`"FAIL"` outside `$STATUS`, no `VOLUME=4096`, no fixed non-empty `MISSING_SERVICE`, and not the old RC-to-PASS fallback block. | Grep-style. Lines with `grep` + `"status":"PASS"` are exempt. |
| `check_swallow` | No `|| true` on measurement lines in guest scripts (NT_SYSCALLS, SUCCESS, do_nt_syscall, binfmt), unless a whitelisted keyword is on the line. No bare `except:` followed by `pass` in harness, kernel-harness or differential Python (lint and selftest excluded). | Look-ahead of 3 lines. |
| `check_default_nonzero` | No measured variable defaulted to 1, and no `${X:-1}` or `${X:=1}` parameter default in guest scripts. | Only `1` is flagged. |
| `check_golden_integrity` | Runs `tests/differential/check_golden_integrity.py`; non-zero exit is a violation. | Requires the golden corpus (see Known gaps). |
| `check_vacuous_comparison` | `dualrun.py` contains `MANDATORY_KEYS`, a volume check and the word "vacuous". | Presence of strings, not behaviour. |
| `check_normalization_masks_real_diff` | If `dualrun.py` mentions dos_device, it also mentions `HarddiskVolume`; handle-count code has a band check; RegionSize code mentions 4096. | Presence of strings. |
| `check_volume_not_claim_based` | Guest scripts that take VOLUME from a claim must also name an external counter, but only `run_mem.sh` is actually enforced. `dualrun.py` must mention `actual_volume` or `MANDATORY_KEYS`. | Effectively one file. |
| `check_evidence_bundle_hash_covers_all_files` | For each `reports/evidence/<bundle>/` with `evidence.json`: `hash.txt` equals SHA-256 over manifest files (in manifest order) plus `run_id`; every manifest file exists; no untracked files except `hash.txt`, `evidence.json`, `run_id.txt`; `run_id.txt` matches. | `evidence.json` itself is not hash-covered (acknowledged in the code). Bundles without `evidence.json` are skipped. |
| `check_file_missing_is_error` | In `dualrun_wrapper.py`, an ERROR log about corrupt or missing files must not be followed by `return 2`. | Six-line window heuristic. Windows fallback in `arch.py` is not enforced (documented in the code). |

The selftest mutation tests confirm that these checks fire on synthetic bad
input, for example `NT_SYSCALLS=$SUCCESS`, a stowaway file in a bundle, or a
missing `HarddiskVolume` prefix check.

## What freshness verifies

`freshness.py` delegates to `vm/freshness.sh check`:

* With no `vm/freshness.sh`, it returns True.
* On Linux it runs `bash vm/freshness.sh check`. On Windows it runs
  `wsl -d Ubuntu -- bash <wsl path>/vm/freshness.sh check`, where the WSL path is
  derived from the repo root (`C:\x\y` becomes `/mnt/c/x/y`).
* Any non-zero exit, or any exception (for example WSL missing), returns False:
  fail closed. `require_fresh()` exits 125.

`freshness.sh check` compares a digest of the kernel sources, the sources
contributing to the bzImage, initramfs and DLL digests against the recorded
manifest in `$KDIR` (default `$HOME/ntpers/linux-6.18`) and the project. A
missing manifest is FAIL. It also flags kernel sources newer than the bzImage.
`freshness.sh write` requires `NTPERS_ALLOW_WRITE=1` and refuses on a dirty tree.

Nothing in dodrun calls `freshness.py`. The in-VM paths (`dualrun_wrapper.py`,
`vmtest.py`) run their own copies of the gate.

## What selftest.py checks

`main()` runs the lint first and returns 1 without running any test if the lint
fails. It then runs 42 test functions. Each one either runs dodrun against a
synthetic criteria set and registry in a temporary directory (unregistered,
gated, crashing, hanging, regression, timeout, wrong run ID, missing result,
canary), or checks a property of a real repo file (golden corpus rules, lint
mutations, freshness, evidence bundles, normalisation and volume guards).

Several tests change real repository files and restore them in `finally`
blocks: the golden corpus, `src/kernel/nt/sync.c` (mtime and content), the
freshness manifests at the repo root and under `$HOME`, and guest scripts. An interrupted run can leave
such changes behind. One test leaves an empty `reports/evidence/` tree
behind in the repo root; `reports/` is gitignored.

## Results on this host (2026-10-12)

Host: Windows 11 Pro 10.0.26200 (build 26200.9457), WinPython 3.13.12, Git for
Windows (bash on PATH). WSL is not installed. No golden corpus is present.

| Run | Result |
|-----|--------|
| `lint_integrity.py` | **FAIL**, 2 violations. Both are from `check_golden_integrity`: `cannot list golden root tests/differential/golden` (WinError 3). The other 10 checks report 0 violations. |
| `selftest.py` | **Does not run**. `main()` stops at the lint gate because of the golden violations above. |
| Selftest functions called directly (outside `main()`) | 24 of 42 pass. 18 fail: 10 dodrun-driven tests (`KeyError` on the criterion, because the lint gate wrote an empty record), 4 golden-corpus tests (golden files missing, or host-build mismatch), 3 freshness tests (WSL not installed), 1 `artifact_identity_stamping` (no built bzImage sidecar). |
| `dodrun.py` | Not completed. Would refuse at the lint gate. Separately, PyYAML is not installed, so `registry.yaml` cannot be parsed. |
| `tests/regression/test_smap_guard.py` | **PASS** (exit 0). `vm/runvm.sh` has no SMAP-disable patterns. |
| `python -m unittest discover tests/unit` | **OK**, 364 tests, 1 skipped (YAML registry, PyYAML absent). Includes the 62 new harness tests. |

Host build note: the golden policy pins `26200.9168`; this host reports
`26200.9457`. Golden checks and dual-run build guards would fail on this host
even with a corpus present.

## Known gaps and open items

1. **Golden corpus is not tracked.** `tests/differential/golden/` is absent from
   git and from the sibling `ntpers-research` checkout. Lint, selftest and dodrun
   therefore cannot pass on any clean checkout, including CI. This is an
   availability problem, not a lint defect; the gate was not weakened.
2. **PyYAML is undeclared.** dodrun needs it for `registry.yaml`; CI does not
   install it. Options: declare and install it in CI, or add a minimal parser for
   the registry subset actually used. The YAML registry test is skipped without it.
3. **WSL root (resolved).** The hardcoded `/path/to/ntpers-research` placeholder
   at about 30 sites is gone. Those sites now take the root from `NTPERS_ROOT` or
   the checkout, through `tests/harness/_paths.py`; see `docs/DEVELOPMENT.md`,
   "Harness root". The kernel harness still uses `~/winapi-native-os` inside WSL,
   an older repo name. The WSL paths are not yet verified on a host with WSL and a
   VM, so they need a run there.
4. **Relative `cwd` in the registry** resolves against the process working
   directory, not the repo root, which the README does not say.
5. **Pinned golden build** (`26200.9168`) differs from this host (`26200.9457`).
6. **Lint is text-based.** Several checks assert that a string exists, not that a
   behaviour holds. Treat a clean lint as necessary, not sufficient.
7. **CI step not added.** The selftest does not pass on a clean Ubuntu runner
   (golden corpus absent, PyYAML absent, and some tests need VM artifacts), so
   adding it would only produce a red build. The new unit tests run in the
   existing CI step `python -m unittest discover tests/unit`.

## Commits in this pass

* `fix(harness): map registry python3 to running interpreter on Windows`
  (`dodrun.py` `resolve_command`, plus `tests/unit/test_harness_dodrun.py`).
* `fix(harness): derive WSL freshness path from real root on Windows`
  (`freshness.py` `windows_to_wsl_path`, plus `tests/unit/test_harness_freshness.py`).
* `test(harness): unit tests for lint_integrity structural checks`
  (`tests/unit/test_harness_lint.py`).
* `docs(harness): conformance run overview` (this file).
