# Harness environment (local Windows host)

Status date: 2026-10-12. Scope: the Python environment that runs
`tests/harness/` on the Windows development host. Not a policy document.

## 1. Interpreter

- Python: WinPython 3.13.12, `C:/Users/Jackson/AppData/Local/Programs/WinPython/WPy64-3.13.12.0/python/python.exe`
- pip: 26.2.1, from the same interpreter.

## 2. PyYAML (required by `dodrun.py` for `tests/harness/registry.yaml`)

| Item | Value |
|------|-------|
| Package | `pyyaml` |
| Version | `6.0.3` |
| Wheel | `pyyaml-6.0.3-cp313-cp313-win_amd64.whl` |
| Wheel sha256 | `79005a0d97d5ddabfeeea4cf676af11e647e41d81c9a7722a193022accdb6b7c` |
| Index | `https://pypi.org/simple` (official PyPI) |
| Install | `python -m pip install --user -r tests/harness/requirements.txt` |

The sha256 is of the wheel downloaded from PyPI with `pip download --no-deps`
on 2026-10-12. The installed package is the same version from the same index.
The pin lives in `tests/harness/requirements.txt`.

No administrator rights and no system or security setting were used. The
install is per-user (`%APPDATA%\Python\Python313\site-packages`).

## 3. CI

The CI workflow (`.github/workflows/ci.yml`) does not run `tests/harness/`.
It runs `tools/*.py --selftest` and `python -m unittest discover tests/unit`.
`requirements.txt` is therefore read only by local runs and does not change
CI.

## 4. Known effect of PyYAML on the harness

With PyYAML present, `test_load_registry_yaml_when_available` in
`tests/unit/test_harness_dodrun.py` runs instead of skipping. The unit suite
stays green (546 tests, 0 skipped on this host).

The selftest CRIT-* cases were still failing after this install, for a
different reason. `dodrun.py` runs `lint_integrity.py` first and refuses to run
when it fails, so it writes no record for the criterion. That gate is covered
in `docs/status/GOLDEN.md` (section 5).

## 5. Selftest status after the PyYAML install (step 2)

Run on 2026-10-12, commit `2080b86` (PyYAML installed, no harness change).

- `python tests/harness/lint_integrity.py`: exit 1, 2 violations. Both are
  `golden integrity`: the pinned build `26200.9168` is absent, there is no
  `latest`, and no valid version directory exists under `golden/`.
- `python tests/harness/selftest.py`: exit 1. `main()` stops at the lint gate
  and runs 0 tests.
- Each `test_*` function called directly (42 functions): **before**, 24 pass
  and 18 fail; **after the PyYAML install**, 24 pass and 18 fail. The same
  18 names fail before and after.

Which of the 18 failures disappeared with PyYAML: **none**.

| Group | Count | Status after PyYAML | Why |
|-------|-------|---------------------|-----|
| CRIT-* (`crashing`, `hanging`, `regression`, `missing_result`, `positive_control`, `unregistered`, `gated_*`, `timeout_ignores_stale`, `exception_not_swallowed`) | 10 | still fail (`KeyError 'CRIT-*'`) | `dodrun.py` loads PyYAML correctly, but it runs `lint_integrity.py` first. The lint fails on the golden gate, so `dodrun.py` refuses to run and writes an empty result record. The cause is the golden gate, not the missing package. |
| WSL (`freshness_missing_manifest`, `freshness_write_without_rebuild`, `freshness_write_without_token`) | 3 | still fail | `wsl.exe` is not installed, so `freshness.sh` cannot run. Step 4 addresses this. |
| Kernel sidecar (`artifact_identity_stamping`) | 1 | still fails | No built `bzImage` sha256 sidecar. The kernel build is not present on this host. |
| Golden path (`golden_integrity_build_mismatch`, `golden_integrity_corrupt`, `golden_integrity_volume_mismatch`, `golden_resolution_without_symlink`) | 4 | still fail | These tests write into `golden/26200.9168/`, which does not exist. The checker also fails on the pin. Step 3 addresses this. |

The earlier note in `docs/status/GOLDEN.md` (section 5.2) attributed the 10
CRIT-* failures to PyYAML. That attribution was wrong: PyYAML is now present
and they still fail, for the lint-gate reason above.

## 6. After steps 3 and 4 (2026-10-12)

Commits: step 3 `5f4d233` (opt-in golden build), step 4 (WSL skip, this commit).

**Selftest entry point:** `python tests/harness/selftest.py` still exits 1 with
0 tests run. The lint gate fails on the golden check (2 violations), and
`main()` stops there. The opt-in in step 3 does not clear it, for the reason
below.

**Direct run of the 42 test functions** (same method as section 5):

| Result | Before (section 5) | After steps 3 and 4 |
|--------|--------------------|---------------------|
| pass | 24 (including 4 that print SKIP and return, so they count as passes) | 24 |
| SKIP, with reason | 0 | 3 (WSL) |
| fail | 18 | 15 |

Which failures disappeared:

- **WSL (3):** now SKIP with reason `WSL required, not available: WSL distro
  Ubuntu unavailable (wsl exit 1; ...)`. Not hidden: a test still fails on
  any assertion once WSL is present.
- **Golden path (4): still fail.** `test_golden_integrity_build_mismatch` and
  `_corrupt` write into `golden/26200.9168/`, which does not exist. `_volume_mismatch`
  and `_resolution_without_symlink` run the checker, which exits 1 on the
  committed `golden/windows-26200.9457/`. With `NTPERS_GOLDEN_BUILD=26200.9457`
  the provenance check passes, but the checker still exits 1 on completeness
  (`probe_path.json` and `probe_path_direct.json` are absent, and 8 of 22
  policy probes have no golden). Those gaps are a policy decision (section 4 of
  GOLDEN.md), not a change this step should make.
- **Kernel sidecar (1): still fails.** `bzImage` sha256 sidecar missing, so
  the kernel build is absent. Not a WSL issue and out of scope for step 4.
- **CRIT-* (10): still fail.** Cause is the lint gate inside `dodrun.py`, not
  PyYAML (section 5).

The unit suite is unchanged in result: green, plus the new tests from steps 3
and 4. The new tests are `tests/unit/test_golden_integrity_override.py`
(14 cases, step 3) and `tests/unit/test_harness_selftest_skip.py` (7 cases, step 4).

Correction to section 5.2 of `docs/status/GOLDEN.md`: the 10 CRIT-* failures
were attributed there to PyYAML. The PyYAML install did not clear them.
