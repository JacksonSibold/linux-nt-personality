"""Unit tests for tools/docs_lint.py.

The lint is loaded by path. Each test builds a small synthetic tree in a
temporary directory and runs the rule set against it, so the tests never write
into the repository. One test runs the rule set on the real repository and
requires zero violations.
"""
import importlib.util
import io
import json
import pathlib
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "docs_lint.py"

_spec = importlib.util.spec_from_file_location("docs_lint", SCRIPT)
docs_lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(docs_lint)

POLICY_OK = """# Integrity policy

## Scope

Scope text.

## Claims

No claim is made.

## Out-of-scope mechanisms

- PatchGuard is out of scope. It is not implemented.
- Secure Kernel is out of scope. It is not implemented.
- Windows-update-signed kernel images are out of scope. They are not claimed.

## Rules for public text

Rules.

## Cross-references

- DOD-SEC-A008 is this policy.
"""


def make_tree(policy=POLICY_OK, readme="Plain readme.\n", criteria_ids=("DOD-SEC-A008",)):
    root = pathlib.Path(tempfile.mkdtemp(prefix="docslint-"))
    (root / "docs" / "policy").mkdir(parents=True)
    (root / "docs" / "status").mkdir(parents=True)
    (root / "tests" / "conformance").mkdir(parents=True)
    (root / "README.md").write_text(readme, encoding="utf-8")
    if policy is not None:
        (root / "docs" / "policy" / "INTEGRITY-POLICY.md").write_text(policy, encoding="utf-8")
    (root / "tests" / "conformance" / "criteria.json").write_text(
        json.dumps([{"id": i} for i in criteria_ids]), encoding="utf-8")
    return root


class SecA008Tests(unittest.TestCase):
    def setUp(self):
        self.roots = []

    def tearDown(self):
        for r in self.roots:
            shutil.rmtree(r, ignore_errors=True)

    def tree(self, **kw):
        root = make_tree(**kw)
        self.roots.append(root)
        return root

    def test_clean_tree_has_no_violations(self):
        self.assertEqual(docs_lint.lint_sec_a008(self.tree()), [])

    def test_missing_policy_document_is_a_violation(self):
        found = docs_lint.lint_sec_a008(self.tree(policy=None))
        self.assertTrue(any("policy document is missing" in v for v in found))

    def test_missing_required_section_is_a_violation(self):
        policy = POLICY_OK.replace("## Cross-references", "## Refs")
        found = docs_lint.lint_sec_a008(self.tree(policy=policy))
        self.assertTrue(any("missing section '## Cross-references'" in v for v in found))

    def test_patchguard_not_declared_out_of_scope_is_a_violation(self):
        policy = POLICY_OK.replace("PatchGuard is out of scope", "PatchGuard is discussed")
        found = docs_lint.lint_sec_a008(self.tree(policy=policy))
        self.assertTrue(any("PatchGuard is not declared out of scope" in v for v in found))

    def test_unknown_cross_reference_is_a_violation(self):
        policy = POLICY_OK + "\nSee DOD-ZZ-A999 for details.\n"
        found = docs_lint.lint_sec_a008(self.tree(policy=policy))
        self.assertTrue(any("DOD-ZZ-A999 is not in criteria.json" in v for v in found))

    def test_windows_update_signed_not_declared_out_of_scope_is_a_violation(self):
        policy = POLICY_OK.replace("Windows-update-signed kernel images are out of scope.",
                                   "Windows-update-signed kernel images are discussed.")
        found = docs_lint.lint_sec_a008(self.tree(policy=policy))
        self.assertTrue(any("Windows-update-signed kernel is not declared out of scope" in v for v in found))

    def test_affirmed_patchguard_support_is_a_violation(self):
        found = docs_lint.lint_sec_a008(
            self.tree(readme="The Personality supports PatchGuard on every boot.\n"))
        self.assertTrue(any("affirms an out-of-scope mechanism" in v for v in found))

    def test_affirmed_signed_kernel_is_a_violation(self):
        found = docs_lint.lint_sec_a008(
            self.tree(readme="The kernel is Microsoft-signed and passes Windows Update checks.\n"))
        self.assertTrue(any("affirms an out-of-scope mechanism" in v for v in found))

    def test_disclaimer_is_not_a_violation(self):
        found = docs_lint.lint_sec_a008(
            self.tree(readme="The Personality does not support PatchGuard.\n"))
        self.assertEqual(found, [])

    def test_claim_outside_surface_is_not_scanned(self):
        root = self.tree()
        research = root / "docs" / "phase1"
        research.mkdir(parents=True)
        (research / "note.md").write_text("PatchGuard is supported.\n", encoding="utf-8")
        self.assertEqual(docs_lint.lint_sec_a008(root), [])

    def test_main_exit_codes(self):
        clean = self.tree()
        bad_root = self.tree(readme="HVCI is enabled everywhere.\n")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(docs_lint.main(["--policy", "--criterion", "DOD-SEC-A008", "--root", str(clean)]), 0)
            self.assertEqual(docs_lint.main(["--policy", "--criterion", "DOD-SEC-A008", "--root", str(bad_root)]), 1)

    def test_main_rejects_unknown_criterion(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(docs_lint.main(["--policy", "--criterion", "DOD-XX-A001"]), 2)

    def test_real_repository_has_zero_violations(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(docs_lint.main(["--policy", "--criterion", "DOD-SEC-A008"]), 0)



def build_tree(files, criteria=None):
    """Write a synthetic tree from {relative path: text} and a criteria.json."""
    root = pathlib.Path(tempfile.mkdtemp(prefix="docslint-"))
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    if criteria is None:
        criteria = [{"id": "DOD-DRV-B003", "criterion": ""}, {"id": "DOD-EVI-B006", "criterion": ""}]
    cj = root / "tests" / "conformance" / "criteria.json"
    cj.parent.mkdir(parents=True, exist_ok=True)
    cj.write_text(json.dumps(criteria), encoding="utf-8")
    return root


STATEMENT = "No third-party NT `.sys` executes in the Personality.\n"
BOUNDARY_FILES = {
    "docs/policy/CLAIM-BOUNDARY.md": "# Claim boundary\n\n" + STATEMENT,
    "README.md": "Plain readme.\n\n" + STATEMENT,
    "docs/status/CRITERIA.md": "Matrix.\n\n" + STATEMENT,
}
DRV_B003_TEXT = ("`[P1]` Claim boundary: no third-party NT `.sys` executes; public docs + capability "
                 "matrix state this; any \"hardware support\" claim must name the **API subset + device "
                 "class** it covers; doc-lint fails release if a gated claim appears without §11 waiver.")
DRV_A001_TEXT = ("Load/run a third-party **production-signed NT `.sys`** (e.g. a real printer/GPU "
                 "filter driver).")


class DrvB003Tests(unittest.TestCase):
    def setUp(self):
        self.roots = []

    def tearDown(self):
        for r in self.roots:
            shutil.rmtree(r, ignore_errors=True)

    def tree(self, extra=None, drop=(), criteria=None):
        files = dict(BOUNDARY_FILES)
        files.update(extra or {})
        for rel in drop:
            files.pop(rel, None)
        root = build_tree(files, criteria)
        self.roots.append(root)
        return root

    def lint(self, **kw):
        return docs_lint.lint_drv_b003(self.tree(**kw))

    def test_clean_tree_has_no_violations(self):
        self.assertEqual(self.lint(), [])

    def test_missing_boundary_document_is_a_violation(self):
        found = self.lint(drop=["docs/policy/CLAIM-BOUNDARY.md"])
        self.assertEqual(len(found), 1)
        self.assertIn("document is missing", found[0])

    def test_boundary_without_statement_is_a_violation(self):
        found = self.lint(extra={"docs/policy/CLAIM-BOUNDARY.md": "# Claim boundary\n\nNothing here.\n"})
        self.assertTrue(any("CLAIM-BOUNDARY.md" in v and "required statement" in v for v in found))

    def test_readme_must_state_the_boundary(self):
        found = self.lint(extra={"README.md": "Plain readme.\n"})
        self.assertTrue(any(v.startswith("README.md") and "required statement" in v for v in found))

    def test_matrix_must_state_the_boundary_outside_the_criterion_text(self):
        restated = f"| DOD-DRV-B003 | DRV | {DRV_B003_TEXT} | P1 | SELF | unknown |\n"
        criteria = [{"id": "DOD-DRV-B003", "criterion": DRV_B003_TEXT}]
        found = self.lint(extra={"docs/status/CRITERIA.md": restated}, criteria=criteria)
        self.assertTrue(any("CRITERIA.md" in v and "required statement" in v for v in found))

    def test_hardware_support_without_subset_and_class_is_a_violation(self):
        found = self.lint(extra={"README.md": STATEMENT + "The Personality adds hardware support for USB.\n"})
        self.assertTrue(any("API subset and device class" in v for v in found))

    def test_hardware_support_naming_subset_and_class_passes(self):
        text = STATEMENT + ("The Personality adds hardware support for the WinUSB API subset "
                            "in the HID device class.\n")
        self.assertEqual(self.lint(extra={"README.md": text}), [])

    def test_negated_hardware_support_is_not_a_claim(self):
        text = STATEMENT + "No hardware support is claimed.\n"
        self.assertEqual(self.lint(extra={"README.md": text}), [])

    def test_affirmed_vendor_driver_without_waiver_is_a_violation(self):
        text = STATEMENT + "The Personality runs a production-signed vendor .sys driver.\n"
        found = self.lint(extra={"README.md": text})
        self.assertTrue(any("without a section 11 waiver" in v for v in found))

    def test_affirmed_vendor_driver_with_section_11_waiver_passes(self):
        text = STATEMENT + ("The Personality runs a production-signed vendor .sys driver "
                            "under the section 11 waiver.\n")
        self.assertEqual(self.lint(extra={"README.md": text}), [])

    def test_disclaimed_vendor_driver_passes(self):
        text = STATEMENT + "The Personality does not run a production-signed vendor .sys driver.\n"
        self.assertEqual(self.lint(extra={"README.md": text}), [])

    def test_criterion_restatement_row_is_not_a_claim(self):
        row = f"| DOD-DRV-A001 | DRV | {DRV_A001_TEXT} | P3 | EXT-GATED | unknown |\n"
        criteria = [{"id": "DOD-DRV-A001", "criterion": DRV_A001_TEXT}]
        self.assertEqual(self.lint(extra={"docs/status/CRITERIA-ROADMAP.md": row}, criteria=criteria), [])

    def test_same_words_outside_a_restatement_are_a_claim(self):
        row = f"| DOD-DRV-A001 | DRV | The project runs a production-signed vendor .sys. | P3 | EXT-GATED | unknown |\n"
        criteria = [{"id": "DOD-DRV-A001", "criterion": DRV_A001_TEXT}]
        found = self.lint(extra={"docs/status/CRITERIA-ROADMAP.md": row}, criteria=criteria)
        self.assertTrue(any("CRITERIA-ROADMAP.md" in v for v in found))


EVI_HEADER = "| ID | Area | Description | Tier | Dependency | Verdict | Evidence | Owner |\n"
EVI_SEP = "|---|---|---|---|---|---|---|---|\n"


def matrix_row(cid, tier="P1", dep="SELF", verdict="unknown", evidence="[matrix](CRITERIA.md)", owner=""):
    return f"| {cid} | AREA | desc | {tier} | {dep} | {verdict} | {evidence} | {owner} |\n"


class EviB006Tests(unittest.TestCase):
    def setUp(self):
        self.roots = []

    def tearDown(self):
        for r in self.roots:
            shutil.rmtree(r, ignore_errors=True)

    def tree(self, matrix=None, extra=None):
        files = {"docs/status/CRITERIA.md": (EVI_HEADER + EVI_SEP + (matrix or matrix_row("DOD-EVI-A001"))),
                 "README.md": "Plain readme.\n"}
        files.update(extra or {})
        root = build_tree(files)
        self.roots.append(root)
        return root

    def lint(self, **kw):
        return docs_lint.lint_evi_b006(self.tree(**kw))

    def test_clean_matrix_has_no_violations(self):
        matrix = matrix_row("DOD-EVI-A001") + matrix_row("DOD-EVI-A002", dep="EXT-GATED", owner="Microsoft")
        self.assertEqual(self.lint(matrix=matrix), [])

    def test_missing_verdict_column_is_a_violation(self):
        header = "| ID | Area | Description | Tier | Dependency | Evidence | Owner |\n"
        root = build_tree({"docs/status/CRITERIA.md": header + EVI_SEP + "| DOD-EVI-A001 | A | d | P1 | SELF | [m](CRITERIA.md) | |\n"})
        self.roots.append(root)
        found = docs_lint.lint_evi_b006(root)
        self.assertTrue(any("lacks required column 'Verdict'" in v for v in found))

    def test_missing_evidence_column_is_a_violation(self):
        header = "| ID | Area | Description | Tier | Dependency | Verdict | Owner |\n"
        root = build_tree({"docs/status/CRITERIA.md": header + EVI_SEP + "| DOD-EVI-A001 | A | d | P1 | SELF | x | |\n"})
        self.roots.append(root)
        found = docs_lint.lint_evi_b006(root)
        self.assertTrue(any("lacks required column 'Evidence'" in v for v in found))

    def test_row_without_evidence_link_is_a_violation(self):
        found = self.lint(matrix=matrix_row("DOD-EVI-A001", evidence="none recorded"))
        self.assertTrue(any("DOD-EVI-A001 shows no evidence link" in v for v in found))

    def test_evidence_link_to_missing_file_is_a_violation(self):
        found = self.lint(matrix=matrix_row("DOD-EVI-A001", evidence="[bundle](evidence/none.json)"))
        self.assertTrue(any("does not resolve" in v for v in found))

    def test_gated_row_without_owner_is_a_violation(self):
        found = self.lint(matrix=matrix_row("DOD-EVI-A001", dep="EXT-GATED"))
        self.assertTrue(any("gated or deviation row DOD-EVI-A001 has no owner name" in v for v in found))

    def test_deviation_row_without_owner_is_a_violation(self):
        found = self.lint(matrix=matrix_row("DOD-EVI-A001", verdict="deviation: skipped"))
        self.assertTrue(any("DOD-EVI-A001 has no owner name" in v for v in found))

    def test_row_without_tier_is_a_violation(self):
        found = self.lint(matrix=matrix_row("DOD-EVI-A001", tier=""))
        self.assertTrue(any("DOD-EVI-A001 has no tier" in v for v in found))

    def test_pass_claim_without_bundle_is_a_violation(self):
        found = self.lint(extra={"README.md": "DOD-EVI-A001 passes on every boot.\n"})
        self.assertTrue(any("pass claim without a linked green bundle" in v for v in found))

    def test_pass_claim_with_existing_bundle_passes(self):
        text = "DOD-EVI-A001 passes, see `evidence/DOD-EVI-A001.bundle.json`.\n"
        found = self.lint(extra={"README.md": text,
                                 "evidence/DOD-EVI-A001.bundle.json": "{}"})
        self.assertEqual(found, [])

    def test_pass_claim_with_missing_bundle_is_a_violation(self):
        text = "DOD-EVI-A001 passes, see `evidence/DOD-EVI-A001.bundle.json`.\n"
        found = self.lint(extra={"README.md": text})
        self.assertTrue(any("pass claim" in v for v in found))

    def test_disclaimed_pass_claim_passes(self):
        found = self.lint(extra={"README.md": "DOD-EVI-A001 does not pass.\n"})
        self.assertEqual(found, [])

    def test_pass_count_without_bundle_is_a_violation(self):
        found = self.lint(extra={"README.md": "The project records 3 of 423 criteria passing.\n"})
        self.assertTrue(any("pass claim" in v for v in found))

    def test_historical_pass_count_in_same_sentence_passes(self):
        text = "The handoff records 3 of 423 criteria passing (historical, not reproduced).\n"
        self.assertEqual(self.lint(extra={"README.md": text}), [])

    def test_hyphenated_identifier_is_not_a_pass_claim(self):
        self.assertIsNone(docs_lint.PASS_CLAIM.search("WI-EXC-SECOND-PASS"))
        self.assertIsNotNone(docs_lint.PASS_CLAIM.search("DOD-EVI-A001 PASS"))

    def test_rules_are_wired(self):
        self.assertEqual(set(docs_lint.RULES), {"DOD-SEC-A008", "DOD-DRV-B003", "DOD-EVI-B006"})


if __name__ == "__main__":
    unittest.main()
