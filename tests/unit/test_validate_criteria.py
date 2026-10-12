"""Unit tests for build/validate_criteria.py.

The script has no functions and no __main__ guard, and it hard-codes an
absolute path to criteria.json at module level. Tests therefore:

  1. write a small criteria.json fixture to a temp directory,
  2. copy the script into the same temp directory with ONLY that hard-coded
     path rewritten to the fixture, and
  3. run the copy as a subprocess and check its printed summary.

The validation logic itself is not modified by this process.
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "validate_criteria.py"

PATH_PATTERN = re.compile(r'^P = \(\s*"[^"]*"\s*"[^"]*"\s*\)', re.MULTILINE)


def entry(**overrides):
    base = {
        "id": "DOD-NET-001",
        "domain": "NET",
        "tier": "P0",
        "dependency_class": "SELF",
        "criterion": "socket opens",
        "verification": "run probe",
        "threshold": "exit 0",
        "source": ["A"],
        "status": "not_implemented",
    }
    base.update(overrides)
    return base


def run_validator(entries):
    """Run the validator on a fixture. Returns (returncode, stdout, stderr)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        criteria = tmp_path / "criteria.json"
        criteria.write_text(json.dumps(entries), encoding="utf-8")

        src = SCRIPT.read_text(encoding="utf-8")
        patched, n = PATH_PATTERN.subn(lambda m: "P = " + json.dumps(str(criteria)), src)
        if n != 1:
            raise AssertionError("could not locate the hard-coded P path in the script")
        copy = tmp_path / "validate_criteria_copy.py"
        copy.write_text(patched, encoding="utf-8")

        proc = subprocess.run(
            [sys.executable, str(copy)], capture_output=True, text=True, encoding="utf-8",
        )
        return proc.returncode, proc.stdout, proc.stderr


class ValidateCriteriaTests(unittest.TestCase):

    def test_clean_fixture_reports_no_problems(self):
        rc, stdout, _ = run_validator([
            entry(id="DOD-NET-001"),
            entry(id="DOD-NET-002", tier="P1"),
        ])
        self.assertEqual(rc, 0)
        self.assertIn("valid JSON. entries: 2", stdout)
        self.assertIn("entries missing required fields: 0", stdout)
        self.assertIn("duplicate ids: 0", stdout)
        self.assertIn("empty criterion text: 0", stdout)

    def test_counts_entries_missing_required_fields(self):
        broken = entry(id="DOD-NET-003")
        del broken["threshold"]
        _, stdout, _ = run_validator([entry(), broken])
        self.assertIn("entries missing required fields: 1", stdout)

    def test_counts_duplicate_ids(self):
        _, stdout, _ = run_validator([
            entry(id="DOD-NET-001"),
            entry(id="DOD-NET-001"),
            entry(id="DOD-NET-002"),
        ])
        self.assertIn("duplicate ids: 1", stdout)

    def test_reports_blank_criterion_ids(self):
        _, stdout, _ = run_validator([
            entry(id="DOD-NET-001", criterion="   "),
            entry(id="DOD-NET-002"),
        ])
        self.assertIn("empty criterion text: 1 ['DOD-NET-001']", stdout)

    def test_tier_and_dependency_summaries(self):
        _, stdout, _ = run_validator([
            entry(id="DOD-NET-001", tier="P0", dependency_class="SELF"),
            entry(id="DOD-NET-002", tier="P0", dependency_class="EXT-DOC"),
            entry(id="DOD-NET-003", tier="P2", dependency_class="SELF"),
        ])
        self.assertIn("tiers: {'P0': 2, 'P2': 1}", stdout)
        self.assertIn("dep:   {'EXT-DOC': 1, 'SELF': 2}", stdout)

    def test_source_summary_uses_first_letter_of_source(self):
        _, stdout, _ = run_validator([
            entry(id="DOD-NET-001", source=["A"]),
            entry(id="DOD-NET-002", source=["B"]),
            entry(id="DOD-NET-003", source=["B"]),
        ])
        self.assertIn("source: {'A': 1, 'B': 2}", stdout)

    def test_lists_ext_gated_entries(self):
        _, stdout, _ = run_validator([
            entry(id="DOD-NET-001", dependency_class="SELF"),
            entry(id="DOD-NET-002", dependency_class="EXT-GATED",
                  criterion="needs MS signing key"),
        ])
        self.assertIn("=== EXT-GATED (1) ===", stdout)
        self.assertIn("DOD-NET-002", stdout)
        self.assertIn("needs MS signing key", stdout)
        self.assertNotIn("DOD-NET-001 [", stdout.split("=== EXT-GATED")[1])

    def test_p0_by_domain_only_counts_p0(self):
        _, stdout, _ = run_validator([
            entry(id="DOD-NET-001", domain="NET", tier="P0"),
            entry(id="DOD-MM-001", domain="MM", tier="P0"),
            entry(id="DOD-MM-002", domain="MM", tier="P1"),
        ])
        self.assertIn("=== P0 BY DOMAIN ===", stdout)
        self.assertTrue(stdout.rstrip().endswith("{'MM': 1, 'NET': 1}"))


if __name__ == "__main__":
    unittest.main()
