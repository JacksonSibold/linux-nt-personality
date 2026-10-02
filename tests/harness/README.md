# Definition-of-Done Conformance Harness

This directory contains the source of truth for whether project `ntpers` is
done. The harness is deliberately conservative: it is structurally harder to
report a pass than to report a failure.

## Files

| File | Purpose |
|------|---------|
| `dodrun.py` | Loads `tests/conformance/criteria.json`, maps each criterion to a test command via `registry.yaml`, executes with timeout, and writes a signed-evidence-style JSON report plus a human summary. |
| `registry.yaml` | Maps criterion IDs to test commands. Missing or null entries are **UNIMPLEMENTED**, never PASS. |
| `selftest.py` | Anti-gaming guard. Verifies that unregistered, gated, crashing, hanging, and regression cases behave correctly. |

## How to run

From WSL (Ubuntu 26.04) with `python3` available:

```bash
# Full conformance run against the real 423-criteria set:
python3 "/path/to/ntpers-research/tests/harness/dodrun.py"

# Run the harness self-tests:
python3 "/path/to/ntpers-research/tests/harness/selftest.py"

# Or via the top-level Makefile:
make -C "/path/to/ntpers-research/build" conformance
make -C "/path/to/ntpers-research/build" test
```

Results are written to `reports/results-<UTC>.json` and `reports/summary-<UTC>.txt`.

## How to add a test

1. Write a real test that exercises the criterion (never a placeholder that
   trivially passes).
2. Add an entry to `tests/harness/registry.yaml`:

```yaml
tests:
  DOD-PE-A001:
    command: ["tests/pe/test_pe_a001.py"]
    timeout: 120
```

3. Run `make -C build test` to ensure the harness still passes its self-tests.
4. Run `make -C build conformance` and inspect the new result file.

## Gated criteria (EXT-GATED)

Some criteria depend on artifacts controlled by third parties (e.g. production
WHQL-signed drivers, kernel anti-cheat attestation, Store licensing). These can
**never** report PASS unless an evidence file is present and hash-verified.

Create an evidence file at:

```
evidence/<CRITERION-ID>.evidence.json
```

with this schema:

```json
{
  "artifact_path": "/path/to/controlling/artifact",
  "artifact_sha256": "<sha-256 of the artifact>",
  "obtained_date": "2026-01-01T00:00:00Z",
  "controlling_party": "Microsoft / Valve / ...",
  "description": "..."
}
```

The harness verifies that the artifact exists and that its SHA-256 matches. If
verification fails, the criterion is reported as `GATED`.

## Governance rules enforced in code

* **UNIMPLEMENTED is a failure state.** A criterion with no registered test, or
  with a registered command of `null`/`unimplemented`, reports `UNIMPLEMENTED`
  and counts as a failure for tier roll-ups.
* **GATED is terminal without evidence.** Any `EXT-GATED` criterion missing a
  verified evidence file reports `GATED`, regardless of any registered test.
* **A test that crashes** (signal, command not found, etc.) reports `ERROR`.
* **A test that exceeds its timeout** reports `TIMEOUT`.
* **Regressions are fatal.** If any criterion moves from a better state to a
  worse state compared with the previous run, `dodrun.py` exits non-zero.
* **Tier gates decide completion:**
  * P0: 100% pass required
  * P1: >= 98% pass required
  * P2: >= 85% pass required
  * P3: best effort, reported but not gating

## Expectations for the current baseline

At this stage of the project the real conformance run will be overwhelmingly
`UNIMPLEMENTED` and `GATED`. That is the correct, honest baseline. The harness
is designed to report that truth without inflation.
