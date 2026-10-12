"""Unit tests for build/validate_criteria.py.

The script has no functions and no __main__ guard. It reads
<root>/tests/conformance/criteria.json, where <root> is the NTPERS_ROOT
environment variable when set, and the parent of build/ otherwise. Tests write
a small criteria.json fixture into a temporary root, set NTPERS_ROOT to it, and
run the real script as a subprocess. The script file is never copied or edited.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "validate_criteria.py"


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
        root = pathlib.Path(tmp)
        conf = root / "tests" / "conformance"
        conf.mkdir(parents=True)
        (conf / "criteria.json").write_text(json.dumps(entries), encoding="utf-8")

        env = dict(os.environ, NTPERS_ROOT=str(root))
        proc = subprocess.run(
            [sys.executable, str(SCRIPT)], capture_output=True, text=True,
            encoding="utf-8", env=env, stdin=subprocess.DEVNULL,
        )
        return proc.returncode, proc.stdout, proc.stderr


class ValidateCriteriaTests(unittest.TestCase):

    def test_default_root_is_the_repository_containing_the_script(self):
        env = {k: v for k, v in os.environ.items() if k != "NTPERS_ROOT"}
        env["PYTHONIOENCODING"] = "utf-8"  # real criteria text contains non-ASCII
        proc = subprocess.run(
            [sys.executable, str(SCRIPT)], capture_output=True, text=True,
            encoding="utf-8", env=env, stdin=subprocess.DEVNULL,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("valid JSON. entries:", proc.stdout)

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
