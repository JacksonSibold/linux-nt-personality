"""Unit tests for tools/criteria_evidence_map.py.

The script is loaded by path. The tests run its --selftest, which checks the matching
and assessment rules on synthetic data and the shipped assessments on the repository,
and they check the rendered Markdown for the assessed criteria.
"""
import importlib.util
import io
import pathlib
import unittest
from contextlib import redirect_stdout

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "criteria_evidence_map.py"

_spec = importlib.util.spec_from_file_location("criteria_evidence_map", SCRIPT)
criteria_evidence_map = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(criteria_evidence_map)


class SelftestTests(unittest.TestCase):
    def test_selftest_passes(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(criteria_evidence_map.selftest(), 0)

    def test_no_criterion_is_marked_full_without_host_evidence(self):
        assessments = criteria_evidence_map.load_yaml(criteria_evidence_map.ASSESSMENTS)
        roadmap = criteria_evidence_map.parse_roadmap(
            criteria_evidence_map.ROADMAP.read_text(encoding="utf-8"))
        full = [k for k, v in assessments.items() if v.get("verdict") == "FULL"]
        self.assertEqual(full, [])
        self.assertEqual(criteria_evidence_map.validate(assessments, {
            c["path"] for c in criteria_evidence_map.discover(REPO)}, roadmap), [])

    def test_markdown_lists_each_assessed_criterion(self):
        assessments = criteria_evidence_map.load_yaml(criteria_evidence_map.ASSESSMENTS)
        text = criteria_evidence_map.render_markdown(assessments, [], {})
        for cid in assessments:
            self.assertIn(f"`{cid}`", text)


if __name__ == "__main__":
    unittest.main()
