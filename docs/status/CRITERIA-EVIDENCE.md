# Criteria evidence map

Status as of 2026-10-12, branch `haiku/regmap`, based on `haiku/mc`.

This records, for 15 criteria from the M-C milestone and the M-A repository-only set, whether
existing tests or checks in this repository demonstrate what the criterion text requires. The
verdict is a reviewer's reading of the cited sources. It is not a harness result.

## Summary

| Verdict | Count |
|---|---|
| FULL | 0 |
| PARTIAL | 3 (DOD-SEC-A008, DOD-DRV-B003, DOD-MEM-A007) |
| NONE | 12 |

Genuinely passing (harness PASS): 0. The lint gate in `tests/harness/dodrun.py` refuses
every run on this host (see below), so no criterion can record PASS.

## Scope and selection

The criteria are the 12 M-C criteria whose roadmap needs (`docs/status/CRITERIA-ROADMAP.md`) do
not include GOLDEN-DIFF, NT-KERNEL, HW-BOOT, VM-HARNESS or ANTI-CHEAT-DRM, plus the three
repository-only criteria the roadmap places in M-A (DOD-SEC-A008, DOD-DRV-B003, DOD-EVI-B006).
The other 91 M-C criteria all need a golden corpus, a booted guest, hardware or an anti-cheat title, so none can be FULL
on a host test. They were not reasoned individually. The brief asked for about 60; the set that
can be judged without a golden corpus or a guest is 15, and that is what was assessed.

## Method

* `tools/criteria_evidence_map.py` lists candidate tests and checks by matching criterion IDs
  and keywords against `tests/host`, `tests/kernel-host`, `tests/ntdll-host`, `tests/unit`,
  `tools/*.py` (scripts with `--check`, `--policy` or `--criterion` modes) and
  `docs/policy/*.md`. `--selftest` checks the matching and the assessment rules.
* Assessments are in `tools/criteria_evidence_assessments.yaml`. The tool refuses a FULL or
  PARTIAL verdict that cites a path it did not discover, a PARTIAL with no statement of what
  is missing, and a FULL on a criterion whose needs include a golden, guest or hardware item.
* `--markdown` renders the table below from that file.

## Harness state

`python tests/harness/dodrun.py` was run on a subset criteria file containing the 15 criteria
(scratch input, reports in a scratch directory). It exited 1 at the integrity lint gate and
executed no criterion:

* `FAIL: golden integrity: cannot list golden root tests/differential/golden`
* `FAIL: golden integrity: unable to resolve golden build directory for pinned 26200.9168`

`NTPERS_GOLDEN_BUILD` was set to `26200.9457` for the run. No code in the repository reads that
variable (grep finds no use), so it had no effect. The golden policy pins `26200.9168`, and the
corpus is absent from the tree. The gate was not weakened.

No registry entry was added. None of the 15 criteria has a FULL verdict, and the rule is that
only FULL matches are mapped.

Two findings from the review:

* `DOD-SEC-A008` already has a registry entry (`docs_lint.py --policy --criterion DOD-SEC-A008`).
  The check verifies only part of the criterion (see the table). The owner should decide whether
  to keep the mapping until the Windows-update declaration and CI wiring are added.
* `tools/docs_lint.py` documents `--criterion DOD-DRV-B003` and `--criterion DOD-EVI-B006`, but
  `RULES` lists only DOD-SEC-A008, so both exit 2. Wiring them would produce FAIL results (see
  the table). That is left for the owner.

## Assessments

| ID | Tier | Verdict | Candidates | Missing / reasoning | Harness result |
|---|---|---|---|---|---|
| `DOD-AC-A003` | P1 | NONE | none | Needs five real titles with user-mode-only integrity checks to run offline on the personality, with the AC SDK log showing the checks ran and passed. No title corpus or AC log exists in the repository. | Not registered. Subset run refused at the lint gate. |
| `DOD-AC-A005` | P0 | NONE | none | Needs a canary that calls the 28 version-bearing APIs and compares the results to an osidentity.json policy table. No such policy file exists in the repository (only vm/initramfs.identity.json, which is unrelated), and no canary exists. RtlGetVersion (src/ntdll/ntdll_main.c line 804) is implemented but no test asserts its output against any policy table. | Not registered. Subset run refused at the lint gate. |
| `DOD-AC-B001` | P0 | NONE | none | Needs an exact-equality canary over all identity-bearing sources against os-identity-policy.json. That file does not exist, and no canary runs. The same gap as DOD-AC-A005. | Not registered. Subset run refused at the lint gate. |
| `DOD-AC-B002` | P1 | NONE | none | The same requirement as DOD-AC-A003 (five titles, scripted gameplay, AC log evidence). Nothing in the repository runs a title. | Not registered. Subset run refused at the lint gate. |
| `DOD-API-A003` | P1 | NONE | none | Needs a gaps.json generated from api-surface and a smoke app that calls every gap in sequence and checks the error codes. gaps.json is not in the repository, and no test calls the unimplemented-API set. test_ntstatus_to_dos.c line 45 maps STATUS_NOT_IMPLEMENTED in a static table, which is not a fail-safe run. | Not registered. Subset run refused at the lint gate. |
| `DOD-API-B003` | P1 | NONE | none | The same gap as DOD-API-A003 (gaps.json absent, no harness calls every gap). The criterion also requires a published gap list, which does not exist. | Not registered. Subset run refused at the lint gate. |
| `DOD-API-B004` | P1 | NONE | none | Needs an instrumented core750 build and a release run on the personality, with at least 98% of the core750 functions executed. The core750 list is absent. Coverage tools in the repository (tools/c_coverage.sh, tools/coverage_report.py) measure host test runs, not a personality release run, and the denominator is not defined here. | Not registered. Subset run refused at the lint gate. |
| `DOD-DLL-A003` | P1 | NONE | none | Needs an application launched on the personality with app-local copies of api-ms-* and core DLLs and a payload check. No test in tests/ or tools/ does this. The word KnownDlls appears in src/kernel/nt/object.c and in docs only. Host-side unit tests of the parser cannot show loader behavior. | Not registered. Subset run refused at the lint gate. |
| `DOD-DLL-A006` | P1 | NONE | none | Needs LdrRegisterDllNotification to stream notifications during corpus launches and an external cross-check against mapped regions. LdrRegisterDllNotification appears in no source file under src/ or tests/ (only in criteria.json, thresholds.json and the differential golden probes). Nothing implemented or run here matches this. | Not registered. Subset run refused at the lint gate. |
| `DOD-DRV-B003` | P1 | PARTIAL | `tools/docs_lint.py` | Missing: (1) docs/policy/CLAIM-BOUNDARY.md does not exist, so the required statement "No third-party NT .sys executes" cannot be found (lint_drv_b003 reports "claim-boundary document is missing" on the real tree); (2) lint_drv_b003 is not in the RULES dict (tools/docs_lint.py line 193), so `--criterion DOD-DRV-B003` exits 2 with "no policy rule set", although the module docstring lists it; (3) no unit test calls lint_drv_b003; (4) the capability-matrix part of the criterion is owned by DOD-EVI-B006, not this rule.. The criterion asks for a claim-boundary statement in public docs, an API-subset and device-class rule for "hardware support" wording, and a doc-lint that fails on gated driver claims. lint_drv_b003 (tools/docs_lint.py line 144) implements the second and third parts with real logic (DRV_TOPIC scan and the hardware-support check). It has no test, is unwired, and fails on the first part because the boundary document is absent. Writing that document to clear the lint would not be evidence of the criterion, so it was not written here. | Not registered (no FULL verdict). Selected in the subset dodrun run: refused at the lint gate, no criterion executed. Direct call of lint_drv_b003 on the real tree: 1 violation (boundary document missing). |
| `DOD-EVI-B006` | P1 | NONE | `tools/docs_lint.py` | Missing: The criterion requires a public capability matrix auto-generated from criteria.json and the results database. No generator exists. lint_evi_b006 (line 163) is unwired (not in RULES) and rejects every row of docs/status/CRITERIA.md: its table has 7 cells per row, the rule requires 8 (line 174), giving 425 violations on the real tree. It also has no test.. The only evidence is a lint whose column layout does not match the matrix it is meant to check, so it cannot show that the matrix is generated or that unlinked claims are rejected. The check is not in CI. NONE is the verdict the evidence supports. | Not registered. Selected in the subset dodrun run: refused at the lint gate, no criterion executed. |
| `DOD-MEM-A007` | P1 | PARTIAL | `tests/kernel-host/stress_section_test.c` | Missing: (1) volume: the default is 4096 operations (main, line 333), not 10^8; (2) the cycles run on threads, not multiple processes; (3) no private RSS plateau or slope is measured, only allocation counts and handle counts; (4) no 12-hour nightly soak exists.. The test drives mixed alloc, map, view, unmap and close operations across threads and checks at the end that the allocation count returns to the baseline (line 416, STRESS_CHECK(end_allocs == base_allocs)) and that the handle table is empty (line 407). That is leak accounting for the same operation mix, so it is the part of the criterion it covers. It does not measure the RSS plateau, the slope threshold or the volume. | Not registered. Subset run refused at the lint gate. |
| `DOD-NT-A008` | P2 | NONE | none | Needs CreateFileMapping on a running EXE with write-sharing contention, a MoveFileEx over a running image that must fail with ERROR_ACCESS_DENIED, and page-fault accounting in /proc/pid/stat. None of these runs in the repository. tests/ntdll-host/test_ldr_pure.c checks image-section layout in pure user space, which is a different property. | Not registered. Subset run refused at the lint gate. |
| `DOD-PE-A011` | P2 | NONE | none | Needs an unhandled exception to produce a minidump read by cdb.exe or dotnet-dump, with a 20-symbol stack walk. No minidump writer exists in src/ or tests/. The stack walk in tests/ntdll-host/test_except_pure.c works on in-process frames, not on a dump. | Not registered. Subset run refused at the lint gate. |
| `DOD-SEC-A008` | P0 | PARTIAL | `tests/unit/test_docs_lint.py`, `tools/docs_lint.py` | Missing: (1) the Windows-update-signed kernel out-of-scope declaration is not asserted: check_policy_doc (tools/docs_lint.py lines 108-127) tests only PatchGuard (line 119) and Secure Kernel (line 122), so deleting the Windows-update bullet in docs/policy/INTEGRITY-POLICY.md line 26 would not fail any test; (2) "auto-checked by doc-lint in CI" is not met: .github/workflows/ci.yml runs update_doc_counts and check_doc_links, not docs_lint; (3) the affirmation scan (is_claim, tools/docs_lint.py) exempts any sentence that contains a qualifier word such as "not" or "no" anywhere, so a claim sentence with such a word is not caught.. The criterion needs the policy document to declare PatchGuard, Secure Kernel and Windows-update-signed kernels out of scope, and needs no public text to affirm them, with the check run in CI. test_docs_lint.py covers the first two declarations (test_patchguard_not_declared_out_of_scope_is_a_violation, line 87) and the affirmation rule (test_affirmed_signed_kernel_is_a_violation, line 102; test_affirmed_patchguard_support_is_a_violation). test_clean_tree_has_no_violations (line 75) and test_real_repository_has_zero_violations (line 130) run the lint on the real tree, which currently reports 0 violations. This is a real check, but it does not assert the third declaration and is not wired into CI. | Registered as command docs_lint.py --policy --criterion DOD-SEC-A008. The dodrun subset run refused at the lint gate (2 golden-corpus violations); no criterion executed, so no harness PASS. Direct run: "DOD-SEC-A008: 0 violation(s)", exit 0. |

