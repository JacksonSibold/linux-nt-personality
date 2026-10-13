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


BOUNDARY_OK = """# Claim boundary

## Boundary statement

No third-party NT `.sys` executes on the Personality.
"""


class DrvB003Tests(unittest.TestCase):
    def setUp(self):
        self.roots = []

    def tearDown(self):
        for r in self.roots:
            shutil.rmtree(r, ignore_errors=True)

    def tree(self, boundary=BOUNDARY_OK, readme="Plain readme.\n", status=None):
        root = make_tree()
        self.roots.append(root)
        if boundary is not None:
            (root / "docs" / "policy" / "CLAIM-BOUNDARY.md").write_text(boundary, encoding="utf-8")
        if status is not None:
            (root / "docs" / "status" / "CRITERIA.md").write_text(status, encoding="utf-8")
        (root / "README.md").write_text(readme, encoding="utf-8")
        return root

    def test_clean_tree_has_no_violations(self):
        self.assertEqual(docs_lint.lint_drv_b003(self.tree()), [])

    def test_missing_boundary_document_is_a_violation(self):
        found = docs_lint.lint_drv_b003(self.tree(boundary=None))
        self.assertTrue(any("claim-boundary document is missing" in v for v in found))

    def test_missing_boundary_statement_is_a_violation(self):
        found = docs_lint.lint_drv_b003(self.tree(boundary="# Claim boundary\n"))
        self.assertTrue(any("required statement" in v for v in found))

    def test_affirmed_third_party_driver_is_a_violation(self):
        found = docs_lint.lint_drv_b003(
            self.tree(readme="Third-party NT .sys drivers load and run on the Personality.\n"))
        self.assertTrue(any("third-party or gated driver claim" in v for v in found))

    def test_hardware_support_without_subset_and_class_is_a_violation(self):
        found = docs_lint.lint_drv_b003(self.tree(readme="The Personality has hardware support.\n"))
        self.assertTrue(any("does not name an API subset and device class" in v for v in found))

    def test_hardware_support_naming_subset_and_class_is_allowed(self):
        found = docs_lint.lint_drv_b003(self.tree(
            readme="Hardware support covers the API subset of HID input for the device class keyboard.\n"))
        self.assertEqual(found, [])

    def test_hardware_sentence_wrapped_across_lines_is_read_as_one(self):
        readme = ("Hardware support is limited to the API subset\n"
                  "covering the device class of virtual serial ports.\n")
        self.assertEqual(docs_lint.lint_drv_b003(self.tree(readme=readme)), [])

    def test_disclaimer_is_not_a_violation(self):
        found = docs_lint.lint_drv_b003(
            self.tree(readme="No third-party NT .sys runs on the Personality.\n"))
        self.assertEqual(found, [])

    def test_criteria_restatement_is_not_scanned(self):
        status = "| DOD-DRV-A001 | DRV | Load/run a third-party production-signed NT `.sys`. | P3 | EXT-GATED | unknown |\n"
        self.assertEqual(docs_lint.lint_drv_b003(self.tree(status=status)), [])


MATRIX_HEADER = ("| ID | Area | Description | Tier | Dependency | Status | Evidence | Owner |\n"
                 "|---|---|---|---|---|---|---|---|\n")


class EviB006Tests(unittest.TestCase):
    def setUp(self):
        self.roots = []

    def tearDown(self):
        for r in self.roots:
            shutil.rmtree(r, ignore_errors=True)

    def tree(self, matrix=MATRIX_HEADER, readme="Plain readme.\n"):
        root = make_tree()
        self.roots.append(root)
        if matrix is not None:
            (root / "docs" / "status" / "CRITERIA.md").write_text(matrix, encoding="utf-8")
        (root / "README.md").write_text(readme, encoding="utf-8")
        return root

    def row(self, cid="DOD-AA-A001", tier="P0", dep="SELF", status="unknown",
            evidence="none recorded", owner="-", desc="a plain criterion"):
        return f"| {cid} | AA | {desc} | {tier} | {dep} | {status} | {evidence} | {owner} |\n"

    def test_unknown_rows_with_no_evidence_are_clean(self):
        self.assertEqual(docs_lint.lint_evi_b006(self.tree(matrix=MATRIX_HEADER + self.row())), [])

    def test_missing_matrix_is_a_violation(self):
        found = docs_lint.lint_evi_b006(self.tree(matrix=None))
        self.assertTrue(any("capability matrix is missing" in v for v in found))

    def test_empty_matrix_is_a_violation(self):
        found = docs_lint.lint_evi_b006(self.tree(matrix=MATRIX_HEADER))
        self.assertTrue(any("no criterion rows" in v for v in found))

    def test_short_row_is_a_violation(self):
        found = docs_lint.lint_evi_b006(self.tree(matrix=MATRIX_HEADER + "| DOD-AA-A001 | AA | x | P0 |\n"))
        self.assertTrue(any("row lacks" in v for v in found))

    def test_pass_without_evidence_link_is_a_violation(self):
        found = docs_lint.lint_evi_b006(self.tree(matrix=MATRIX_HEADER + self.row(status="pass")))
        self.assertTrue(any("claims pass without an evidence link" in v for v in found))

    def test_pass_with_evidence_link_is_clean(self):
        matrix = MATRIX_HEADER + self.row(status="pass", evidence="evidence/DOD-AA-A001.json")
        self.assertEqual(docs_lint.lint_evi_b006(self.tree(matrix=matrix)), [])

    def test_gated_row_without_owner_is_a_violation(self):
        matrix = MATRIX_HEADER + self.row(dep="EXT-GATED", owner="")
        found = docs_lint.lint_evi_b006(self.tree(matrix=matrix))
        self.assertTrue(any("gated DOD-AA-A001 has no owner name" in v for v in found))

    def test_escaped_pipe_in_description_keeps_eight_cells(self):
        matrix = MATRIX_HEADER + self.row(desc="a \\| b")
        self.assertEqual(docs_lint.lint_evi_b006(self.tree(matrix=matrix)), [])

    def test_unqualified_pass_count_claim_is_a_violation(self):
        readme = "The project records **5 of 423** criteria passing.\n"
        found = docs_lint.lint_evi_b006(self.tree(matrix=MATRIX_HEADER, readme=readme))
        self.assertTrue(any("unqualified pass-count claim" in v for v in found))

    def test_pass_count_qualified_in_same_paragraph_is_clean(self):
        readme = ("The handoff records **5 of 423** criteria passing.\n"
                  "This count is historical and not reproduced here.\n")
        matrix = MATRIX_HEADER + self.row()
        self.assertEqual(docs_lint.lint_evi_b006(self.tree(matrix=matrix, readme=readme)), [])


if __name__ == "__main__":
    unittest.main()
