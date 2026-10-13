# Criteria progress: milestones M-A, M-B and M-C

This file records each criterion attempted from `docs/status/CRITERIA-ROADMAP.md`
(milestones M-A and M-B). For each one it gives what the criterion needs, what this
repository produced, the real result of the project's harness, and why. A criterion
is PASS here only when `python tests/harness/dodrun.py` recorded PASS for it. Direct
runs of a check are shown separately and are not harness results.

## Harness state on this worktree

Checked with `python tests/harness/dodrun.py` on branch `haiku/ma`.

- dodrun exits 1 before it runs any criterion. The integrity lint (`tests/harness/lint_integrity.py`) reports two golden-corpus violations, because `tests/differential/golden/` does not exist. dodrun refuses to run while the lint fails.
- The pinned golden build is `26200.9168` (`tests/differential/dualrun_policy.json`). The host reports `Microsoft Windows [Version 10.0.26200.9457]`. The golden check also requires the host build to match the pinned build, so the golden corpus cannot be captured on this host, even with a toolchain.
- The host has no MinGW toolchain (`gcc`, `x86_64-w64-mingw32-gcc` and `make` are all absent), so the probes cannot be built here.
- dodrun has no per-criterion selector. The narrowest run is the full runner, and it refuses before it selects anything. A selector was not added, because a partial run would misreport the rest of the set.
- PyYAML 6.0.3 is installed in the WinPython interpreter, so the YAML import is not the blocker.

Consequence: no criterion can record PASS in this worktree today. Every row below is
BLOCKED on the harness gate. A row also shows the direct result of its check where
one was run.

Blocker owner: the owner must either re-pin the golden build to the host build or
provide a host at `26200.9168` with MinGW, so that the golden corpus (WI-GOLDEN) can
be captured. That is an owner decision, not something this worktree can settle.

## Attempted criteria

| ID | Needs | Produced | Harness result | Why |
|---|---|---|---|---|
| `DOD-SEC-A008` (P0, SELF) | Public text declares PatchGuard, Secure Kernel and Windows-update-signed kernel claims out of scope, and affirms none of them. Verification names `dod docs-lint --policy`. | `docs/policy/INTEGRITY-POLICY.md` (sections Scope, Claims, Out-of-scope mechanisms, Rules for public text, Cross-references); `tools/docs_lint.py` (the check, rule `DOD-SEC-A008`); `tests/unit/test_docs_lint.py` (12 tests); registry entry for `DOD-SEC-A008` in `tests/harness/registry.yaml`. | BLOCKED. dodrun: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, then `Total violations: 2` (golden integrity). Direct check: `DOD-SEC-A008: 0 violation(s)`, exit 0. | The direct check passes on the real tree and is tested against bad synthetic trees. The criterion still cannot be PASS until dodrun records it. The lint is a SELF check, so its strength is its rules, which are listed in the policy document. |

## Policy docs-lint criteria (branch haiku/docslint)

Harness state on this branch: `python tests/harness/dodrun.py` was run again and refused at the integrity lint gate with the same two golden-corpus violations. No criterion ran, so nothing in this section is a harness result.

Two results are kept apart. The direct result is the criterion's own check, `python tools/docs_lint.py --policy --criterion <ID>`. PASS-DIRECT means that check exits 0. FAIL-DIRECT means it exits 1. Only a PASS-DIRECT that dodrun also records is cleared. Nothing is cleared on this branch, because dodrun records no criterion while the golden gate fails.

| ID | Criterion needs | Direct result | Harness result | Cleared |
|---|---|---|---|---|
| `DOD-SEC-A008` (P0) | Public text declares PatchGuard, Secure Kernel and Windows-update-signed kernels out of scope, and affirms none of them | PASS-DIRECT, exit 0, 0 violations, the rule now also asserts the Windows-update-signed declaration that `INTEGRITY-POLICY.md` already states | BLOCKED at the golden gate, no registry record | not cleared |
| `DOD-DRV-B003` (P1) | Claim boundary: the statement that no third-party driver executes appears in public docs and in the capability matrix, device-support wording names an API subset and device class, and an affirmed gated driver claim needs a section 11 waiver | FAIL-DIRECT, exit 1, 3 violations, README and the capability matrix do not state the boundary, and one device-support sentence in the evidence notes names no subset or class | BLOCKED at the golden gate, no registry record | not cleared |
| `DOD-EVI-B006` (P1) | Capability matrix shows ID, tier, verdict and evidence link on every row, gated rows name owners, and a CI text-lint rejects claims of a row without a green signed bundle | FAIL-DIRECT, exit 1, 6 violations, the generated matrix has no Verdict, Evidence or Owner column, and three pass claims have no linked bundle | BLOCKED at the golden gate, no registry record | not cleared |

Owner decisions that the direct results depend on:

- `DOD-DRV-B003`: the statement must also appear in `README.md` and in the capability matrix. The matrix is generated by `tools/criteria_report.py`, so that generator must change, which is an owner call. The device-support sentence in the evidence notes quotes the criterion wording. The owner can reword it or accept the finding.
- `DOD-EVI-B006`: `tools/criteria_report.py` must emit Verdict, Evidence and Owner columns and link every row to a bundle. Once the columns exist, each of the 423 rows with no bundle will be reported, because the matrix links no bundle today.
- Three pass claims have no linked bundle: the count sentence in `README.md` (line 32), the count in the generated matrix (`CRITERIA.md`, line 6), and the `DOD-API-B004` note in the M-C section of this file. The owner can link a bundle, or qualify each sentence as historical or not yet run in that same sentence.

## Milestone M-C attempts

Selection: the first 15 criteria of milestone M-C in the roadmap order (section 3). Of the 103 M-C criteria,
55 need only `WIN32-SHIM`, `PE-LOADER` or `GOLDEN-DIFF` at effort S or M. No M-C criterion is classed REPO-ONLY,
and each of the 15 names the golden corpus, the personality kernel, or an input file the repository does not contain.
A host unit test would not stand in for them: the registry rules forbid mapping a test to a criterion it does not verify.

Status: `BLOCKED` means the first gate that fails on this host is the golden corpus, which is an owner decision on the
pinned build. `NEEDS-GUEST` means the evidence needs a run on the personality kernel and no golden gate is named.
Every row also needs a guest for its real run. Passing count: 0. Needs-guest count: 3.

Common harness result: `python tests/harness/dodrun.py` exits 1 with `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`
and `Total violations: 2` (golden corpus absent). The registry has no entry for any of the 15, so each is UNIMPLEMENTED.

| ID | Tier | Needs | Produced | Harness result | Status | Why |
|---|---|---|---|---|---|---|
| `DOD-SYNC-A002` | P0 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | 10^7 WaitOnAddress producer/consumer transitions with a lost-wakeup check. Golden gate fails first (corpus absent). The run itself needs the personality wait path on a guest. |
| `DOD-SYNC-B002` | P0 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Dual run of the same 10^7 wait/wake check against golden, with address-size arms 1/2/4/8 and unaligned. Golden gate fails first; also needs a guest. |
| `DOD-SYNC-B003` | P0 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | CriticalSection recursion, TryEnter timed arms and SRWLock semantics versus golden. Golden gate fails first; also needs a guest. |
| `DOD-SYNC-A003` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Timeout fidelity bands (p99.9 completion within 2 ms, 1,000 timers) need Windows reference timings. WI-PERF-BASE is not done; needs the reference Windows host. |
| `DOD-SYNC-A004` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Priority-inheritance window needs a kernel-trace cross-check on a guest, compared to a golden band of +/-20%. Golden gate fails first. |
| `DOD-SYNC-A005` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Interlocked suite including cmpxchg16b on all corpus CPUs (2,000 micro-test cases). Golden gate fails first; corpus CPUs are not on this host. |
| `DOD-SYNC-A006` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Global\ versus session namespace isolation and ERROR_ALREADY_EXISTS behaviour versus golden. Golden gate fails first. |
| `DOD-SYNC-B004` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Timer latency histograms compared to Windows measured p50/p99 within 1.5x (KS test). Needs WI-PERF-BASE baseline timings, which are absent. |
| `DOD-SYNC-B005` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | IOCP exactly-once completion ledger (CancelIoEx race, 0-byte Post, batch keys) versus golden. Golden gate fails first. |
| `DOD-SYNC-B006` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Interlocked family and IsProcessorFeaturePresent truthfulness, including a CPUID-masked VM arm. The masked arm needs WI-PORT-ARM and WI-KBOOT, which are absent. |
| `DOD-SYNC-B007` | P1 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | WAIT_ALL with an owned mutex, and 10^6 shuffles of 64 mixed handles, compared to golden. Golden gate fails first. |
| `DOD-SYNC-A007` | P2 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | WaitForMultipleObjects 64-handle race (10^6 shuffles) against the golden satisfied-set invariant. Golden gate fails first. |
| `DOD-SYNC-B008` | P2 | WIN32-SHIM, GOLDEN-DIFF | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | BLOCKED | Priority-inversion window with kernel trace and a golden band of +/-20%. Golden gate fails first; needs a guest trace. |
| `DOD-API-A003` | P1 | WIN32-SHIM | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | NEEDS-GUEST | Gap APIs fail safe in a smoke app. The check runs against gaps.json, which is generated from api-surface; that file is absent (WI-API-SURFACE, not in the tree), so the input cannot be produced here. |
| `DOD-API-B003` | P1 | WIN32-SHIM | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | NEEDS-GUEST | Same gap-fail-safe check through the dual harness. Needs gaps.json (absent, WI-API-SURFACE) and the personality runtime on a guest. |
| `DOD-API-B004` | P1 | WIN32-SHIM | none | dodrun exit 1: `INTEGRITY LINT FAILED - dodrun REFUSES TO RUN`, `Total violations: 2` (golden corpus absent); no registry entry (UNIMPLEMENTED) | NEEDS-GUEST | Executed coverage of core750 needs an instrumented build and a release run on the personality. The core750 list is absent (WI-API-SURFACE), so the denominator cannot be formed here. |

## Needs-guest list (M-C)

These criteria need a booted personality kernel or a generated input the repository lacks. Exact command for each once a registry entry exists; dodrun has no per-criterion selector, so the run is the full suite:

- `DOD-API-A003`: `python tests/harness/dodrun.py` (full run), after `gaps.json` is generated from api-surface (WI-API-SURFACE) and the personality smoke app runs on a guest.
- `DOD-API-B003`: `python tests/harness/dodrun.py` (full run), with the same `gaps.json` input and a guest.
- `DOD-API-B004`: `python tests/harness/dodrun.py` (full run), after an instrumented core750 build (core750 list from WI-API-SURFACE) runs its release check on a guest.

The other 12 rows above are BLOCKED on the golden corpus first. Each also needs a guest run after that gate clears.
