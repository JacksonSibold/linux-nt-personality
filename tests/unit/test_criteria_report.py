"""Unit tests for tools/criteria_report.py.

The script is importable (main() sits behind a __main__ guard), so it is loaded
by path. Pure helpers are tested with small synthetic inputs. Tests against the
real criteria set read the repository files without writing anything, except
that the temp-file tests use a temporary directory only.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "criteria_report.py"
CRITERIA = REPO / "tests" / "conformance" / "criteria.json"
DOC = REPO / "docs" / "status" / "CRITERIA.md"

_spec = importlib.util.spec_from_file_location("criteria_report", SCRIPT)
criteria_report = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(criteria_report)


def quiet(fn, *args, **kwargs):
    """Call fn while swallowing its stdout. Returns fn's result."""
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kwargs)


def entry(cid, status="not_implemented", tier="P1", dep="SELF", criterion="Text"):
    return {"id": cid, "domain": cid.split("-")[1], "tier": tier,
            "dependency_class": dep, "criterion": criterion, "status": status}


class StatusTests(unittest.TestCase):

    def test_placeholder_is_unknown(self):
        self.assertEqual(criteria_report.status_of(entry("DOD-AA-A001")), "unknown")

    def test_empty_is_unknown(self):
        self.assertEqual(criteria_report.status_of(entry("DOD-AA-A001", status="")), "unknown")

    def test_recorded_status_is_kept(self):
        self.assertEqual(criteria_report.status_of(entry("DOD-AA-A001", status="pass")), "pass")


class BuildRowsTests(unittest.TestCase):

    def test_row_fields_and_order(self):
        rows = criteria_report.build_rows([entry("DOD-AA-A001", criterion="One")])
        self.assertEqual(list(rows[0].keys()),
                         ["id", "area", "description", "tier", "dependency_class", "status",
                          "verdict", "evidence", "owner"])
        self.assertEqual(rows[0]["area"], "AA")

    def test_description_whitespace_collapsed(self):
        rows = criteria_report.build_rows([entry("DOD-AA-A001", criterion="a\n  b\tc")])
        self.assertEqual(rows[0]["description"], "a b c")


class SummaryTests(unittest.TestCase):

    def test_counts(self):
        rows = criteria_report.build_rows([
            entry("DOD-AA-A001", tier="P0", dep="SELF"),
            entry("DOD-AA-A002", status="pass", tier="P0", dep="EXT-RE"),
            entry("DOD-BB-A001", tier="P3", dep="EXT-GATED"),
        ])
        s = criteria_report.summarize(rows, None)
        self.assertEqual(s["total"], 3)
        self.assertEqual(s["status_counts"], {"pass": 1, "unknown": 2})
        self.assertEqual(s["tier_counts"], {"P0": 2, "P3": 1})
        self.assertEqual(s["dependency_counts"], {"EXT-GATED": 1, "EXT-RE": 1, "SELF": 1})


class ReadmeTests(unittest.TestCase):

    def test_regex_parses_note(self):
        m = criteria_report.README_RE.search("only **3 of 423** full conformance criteria passing.")
        self.assertEqual(m.groups(), ("3", "423"))

    def test_regex_ignores_unrelated_text(self):
        self.assertIsNone(criteria_report.README_RE.search("**2 of 5** other things passing"))

    def test_missing_readme_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(criteria_report.read_readme_count(pathlib.Path(tmp) / "nope.md"))

    def test_readme_count_read_from_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / "README.md"
            p.write_text("only **3 of 423** full conformance criteria passing.\n", encoding="utf-8")
            self.assertEqual(criteria_report.read_readme_count(p),
                             {"passing": 3, "total": 423, "source": "README.md"})


class LoadTests(unittest.TestCase):

    def _write(self, tmp, data):
        p = pathlib.Path(tmp) / "criteria.json"
        p.write_text(json.dumps(data), encoding="utf-8")
        return p

    def test_rejects_duplicate_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = self._write(tmp, [entry("DOD-AA-A001"), entry("DOD-AA-A001")])
            with self.assertRaises(ValueError):
                criteria_report.load_criteria(p)

    def test_rejects_missing_field(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = entry("DOD-AA-A001")
            del bad["tier"]
            with self.assertRaises(ValueError):
                criteria_report.load_criteria(self._write(tmp, [bad]))

    def test_rejects_non_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                criteria_report.load_criteria(self._write(tmp, {"a": 1}))


class RenderTests(unittest.TestCase):

    def setUp(self):
        self.rows = criteria_report.build_rows([
            entry("DOD-AA-A001", criterion="has | pipe"),
            entry("DOD-AA-A002", status="pass", criterion="plain"),
        ])
        self.readme = {"passing": 3, "total": 423, "source": "README.md"}
        self.summary = criteria_report.summarize(self.rows, self.readme)

    def test_markdown_escapes_pipe(self):
        md = criteria_report.render_markdown(self.rows, self.summary)
        self.assertIn("has \\| pipe", md)

    def test_markdown_has_table_header_and_readme_note(self):
        md = criteria_report.render_markdown(self.rows, self.summary)
        self.assertIn("| ID | Area | Description | Tier | Dependency | Status | Verdict | Evidence | Owner |", md)
        self.assertIn("**3 of 423**", md)

    def test_markdown_states_claim_boundary(self):
        md = criteria_report.render_markdown(self.rows, self.summary)
        self.assertIn("No third-party NT `.sys` executes in the Personality.", md)
        self.assertIn("(../policy/CLAIM-BOUNDARY.md)", md)

    def test_markdown_row_has_verdict_evidence_owner(self):
        md = criteria_report.render_markdown(self.rows, self.summary)
        row = next(ln for ln in md.splitlines() if ln.startswith("| DOD-AA-A001 |"))
        self.assertIn("| not assessed |", row)
        self.assertIn("(../../tests/harness/registry.yaml)", row)
        self.assertTrue(row.endswith("| project owner |"))


    def test_json_round_trips(self):
        doc = json.loads(criteria_report.render_json(self.rows, self.summary))
        self.assertEqual(doc["summary"]["total"], 2)
        self.assertEqual(doc["criteria"][1]["status"], "pass")

    def test_text_lists_every_id(self):
        txt = criteria_report.render_text(self.rows, self.summary)
        for r in self.rows:
            self.assertIn(r["id"], txt)


class MatrixColumnTests(unittest.TestCase):

    def test_assessed_verdict_is_used(self):
        rows = criteria_report.build_rows(
            [entry("DOD-AA-A001"), entry("DOD-AA-A002")],
            assessed={"DOD-AA-A001": "partial"})
        self.assertEqual(rows[0]["verdict"], "partial")
        self.assertEqual(rows[1]["verdict"], "not assessed")

    def test_assessed_row_links_assessment(self):
        self.assertEqual(criteria_report.evidence_of("DOD-AA-A001", {"DOD-AA-A001": "partial"}, set()),
                         "[assessment](CRITERIA-EVIDENCE.md); no bundle")

    def test_registered_row_links_registry(self):
        self.assertEqual(criteria_report.evidence_of("DOD-AA-A001", {}, {"DOD-AA-A001"}),
                         "[registry](../../tests/harness/registry.yaml); no bundle")

    def test_unregistered_row_says_no_entry(self):
        self.assertIn("has no entry", criteria_report.evidence_of("DOD-AA-A001", {}, set()))

    def test_gated_row_has_gated_verdict_and_third_party_owner(self):
        rows = criteria_report.build_rows([entry("DOD-AA-A001", dep="EXT-GATED")])
        self.assertEqual(rows[0]["verdict"], "gated")
        self.assertIn("third party", rows[0]["owner"])

    def test_load_assessments_maps_verdict_words(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / "a.yaml"
            p.write_text("DOD-AA-A001:\n  verdict: PARTIAL\n  candidates:\n    - x\n"
                         "DOD-AA-A002:\n  verdict: NONE\n", encoding="utf-8")
            self.assertEqual(criteria_report.load_assessments(p),
                             {"DOD-AA-A001": "partial", "DOD-AA-A002": "not demonstrated"})

    def test_load_assessments_missing_file_is_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(criteria_report.load_assessments(pathlib.Path(tmp) / "no.yaml"), {})

    def test_load_registered_ignores_commented_entries(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / "r.yaml"
            p.write_text("tests:\n  DOD-AA-A001:\n    command: [x]\n  # DOD-AA-A002:\n"
                         "  #   command: [y]\n", encoding="utf-8")
            self.assertEqual(criteria_report.load_registered(p), {"DOD-AA-A001"})


class SelftestTests(unittest.TestCase):

    def test_selftest_passes(self):
        self.assertEqual(quiet(criteria_report.selftest), 0)


class RealDataTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.criteria = criteria_report.load_criteria(CRITERIA)
        cls.rows = criteria_report.build_rows(
            cls.criteria, criteria_report.load_assessments(),
            criteria_report.load_registered())

    def test_every_evidence_link_resolves(self):
        import re
        for r in self.rows:
            for target in re.findall(r"\]\(([^)]+)\)", r["evidence"]):
                self.assertTrue((DOC.parent / target).is_file(), target)

    def test_gated_rows_name_an_owner(self):
        gated = [r for r in self.rows if r["dependency_class"] == "EXT-GATED"]
        self.assertEqual(len(gated), 17)
        self.assertTrue(all(r["owner"] for r in gated))

    def test_total_is_423(self):
        self.assertEqual(len(self.rows), 423)

    def test_tiers_match_definition_of_done(self):
        s = criteria_report.summarize(self.rows, None)
        self.assertEqual(s["tier_counts"], {"P0": 143, "P1": 175, "P2": 89, "P3": 16})

    def test_dependency_classes_match_definition_of_done(self):
        s = criteria_report.summarize(self.rows, None)
        self.assertEqual(s["dependency_counts"],
                         {"EXT-DOC": 227, "SELF": 131, "EXT-RE": 48, "EXT-GATED": 17})

    def test_no_criterion_reports_passing_without_record(self):
        self.assertTrue(all(r["status"] == "unknown" for r in self.rows))

    def test_readme_records_three_of_423(self):
        readme = criteria_report.read_readme_count(REPO / "README.md")
        self.assertEqual((readme["passing"], readme["total"]), (3, 423))

    def test_generated_doc_is_current(self):
        readme = criteria_report.read_readme_count(REPO / "README.md")
        summary = criteria_report.summarize(self.rows, readme)
        expected = criteria_report.render_markdown(self.rows, summary)
        self.assertTrue(DOC.exists(), "docs/status/CRITERIA.md missing; regenerate it")
        self.assertEqual(DOC.read_text(encoding="utf-8"), expected)


if __name__ == "__main__":
    unittest.main()
