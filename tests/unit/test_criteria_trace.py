"""Unit tests for tools/criteria_trace.py.

The script is importable (main() sits behind a __main__ guard), so it is loaded
by path. Pure helpers are tested with small synthetic inputs. Filesystem tests
use a temporary directory. The real-repository tests only read repository files.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "criteria_trace.py"

_spec = importlib.util.spec_from_file_location("criteria_trace", SCRIPT)
criteria_trace = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(criteria_trace)

HOST = criteria_trace.HOST
HARNESS = criteria_trace.HARNESS
DOCS = criteria_trace.DOCS
NONE = criteria_trace.NONE


def quiet(fn, *args, **kwargs):
    """Call fn while swallowing its stdout. Returns fn's result."""
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kwargs)


def crit(cid, status="not_implemented", tier="P1", dep="SELF", criterion="Text"):
    return {"id": cid, "domain": cid.split("-")[1], "tier": tier,
            "dependency_class": dep, "criterion": criterion, "status": status}


REGISTRY = (
    "# header mentions DOD-ZZ-A009 in a comment\n"
    "tests:\n"
    "  # DOD-AA-B001 is intentionally UNIMPLEMENTED.\n"
    "  DOD-AA-A001:\n"
    "    command: [\"python3\", \"tests/kernel/a.py\"]  # runnable\n"
    "    timeout: 60\n"
    "  DOD-AA-A002:\n"
    "    command: unimplemented\n"
    "  DOD-AA-A003:\n"
    "    command: [\"python3\", \"tests/kernel/c.py\"]\n"
    "    skip: true\n"
    "    skip_reason: \"needs hardware\"\n"
    "  DOD-AA-B001:\n"
    "    command: null\n"
)


class ExtractIdsTests(unittest.TestCase):

    def test_plain_id(self):
        self.assertEqual(criteria_trace.extract_ids("see DOD-PE-A010 here"), {"DOD-PE-A010"})

    def test_shorthand_range_expands(self):
        self.assertEqual(criteria_trace.extract_ids("DOD-PE-A010/B010 need"),
                         {"DOD-PE-A010", "DOD-PE-B010"})

    def test_spaced_shorthand_expands(self):
        self.assertEqual(criteria_trace.extract_ids("DOD-NT-A003 / B004"),
                         {"DOD-NT-A003", "DOD-NT-B004"})

    def test_partial_number_rejected(self):
        self.assertEqual(criteria_trace.extract_ids("DOD-PE-A0101"), set())

    def test_non_canonical_form_ignored(self):
        self.assertEqual(criteria_trace.extract_ids("DOD-SOCK-001 DOD-VM-002"), set())

    def test_several_ids_in_one_line(self):
        self.assertEqual(criteria_trace.extract_ids("DOD-AA-A001, DOD-BB-B002."),
                         {"DOD-AA-A001", "DOD-BB-B002"})


class RegistryParseTests(unittest.TestCase):

    def setUp(self):
        self.entries = criteria_trace.parse_registry(REGISTRY)

    def test_keys_found_and_comments_ignored(self):
        self.assertEqual(set(self.entries), {"DOD-AA-A001", "DOD-AA-A002", "DOD-AA-A003", "DOD-AA-B001"})

    def test_list_command_is_active(self):
        self.assertTrue(criteria_trace.is_active(self.entries["DOD-AA-A001"]))

    def test_unimplemented_is_inactive(self):
        self.assertFalse(criteria_trace.is_active(self.entries["DOD-AA-A002"]))

    def test_skip_true_is_inactive(self):
        self.assertTrue(self.entries["DOD-AA-A003"]["skip"])
        self.assertFalse(criteria_trace.is_active(self.entries["DOD-AA-A003"]))

    def test_null_is_inactive(self):
        self.assertFalse(criteria_trace.is_active(self.entries["DOD-AA-B001"]))

    def test_inline_comment_stripped(self):
        self.assertEqual(self.entries["DOD-AA-A001"]["command"], '["python3", "tests/kernel/a.py"]')

    def test_empty_text(self):
        self.assertEqual(criteria_trace.parse_registry(""), {})


class StatusTests(unittest.TestCase):

    def test_placeholder_is_unknown(self):
        self.assertEqual(criteria_trace.status_of(crit("DOD-AA-A001")), "unknown")

    def test_empty_is_unknown(self):
        self.assertEqual(criteria_trace.status_of(crit("DOD-AA-A001", status="")), "unknown")

    def test_recorded_status_is_kept(self):
        self.assertEqual(criteria_trace.status_of(crit("DOD-AA-A001", status="pass")), "pass")


class BuildMatrixTests(unittest.TestCase):

    def build(self, sources, criteria=None, registry=REGISTRY):
        if criteria is None:
            criteria = [crit("DOD-AA-A001"), crit("DOD-AA-A002"), crit("DOD-AA-A003"),
                        crit("DOD-AA-A004"), crit("DOD-AA-B001", tier="P2")]
        rows, summary = criteria_trace.build_matrix(criteria, registry, sources)
        return {r["id"]: r for r in rows}, summary

    def test_harness_entry_wins_over_host_test(self):
        by_id, _ = self.build([("tests/kernel/a.py", "DOD-AA-A001")])
        self.assertEqual(by_id["DOD-AA-A001"]["classification"], HARNESS)
        self.assertEqual(by_id["DOD-AA-A001"]["host_tests"], ["tests/kernel/a.py"])

    def test_host_test_without_harness_entry(self):
        by_id, _ = self.build([("tests/host/t.c", "/* DOD-AA-A004 */")])
        self.assertEqual(by_id["DOD-AA-A004"]["classification"], HOST)

    def test_unimplemented_registry_entry_is_docs(self):
        by_id, _ = self.build([])
        self.assertEqual(by_id["DOD-AA-A002"]["classification"], DOCS)
        self.assertEqual(by_id["DOD-AA-A002"]["registry"], "placeholder")

    def test_skipped_registry_entry_is_docs(self):
        by_id, _ = self.build([])
        self.assertEqual(by_id["DOD-AA-A003"]["classification"], DOCS)

    def test_registry_comment_reference_is_docs(self):
        by_id, _ = self.build([])
        self.assertEqual(by_id["DOD-AA-B001"]["classification"], DOCS)
        self.assertEqual(by_id["DOD-AA-B001"]["doc_refs"], ["tests/harness/registry.yaml"])

    def test_registry_absent_state(self):
        by_id, _ = self.build([])
        self.assertEqual(by_id["DOD-AA-A004"]["registry"], "absent")

    def test_unit_test_reference_is_docs_not_host(self):
        by_id, _ = self.build([("tests/unit/test_x.py", "DOD-AA-A004")])
        self.assertEqual(by_id["DOD-AA-A004"]["classification"], DOCS)

    def test_markdown_reference_is_docs(self):
        by_id, _ = self.build([("tests/host/README.md", "covers DOD-AA-A004")])
        self.assertEqual(by_id["DOD-AA-A004"]["classification"], DOCS)

    def test_spec_files_excluded(self):
        by_id, _ = self.build([("tests/conformance/criteria.json", "DOD-AA-A004"),
                               ("tests/conformance/thresholds.json", "DOD-AA-A004")])
        self.assertEqual(by_id["DOD-AA-A004"]["classification"], NONE)

    def test_unknown_ids_ignored(self):
        by_id, summary = self.build([("tests/host/t.c", "DOD-XX-A999")])
        self.assertNotIn("DOD-XX-A999", by_id)
        self.assertEqual(summary["total"], 5)

    def test_result_is_unknown_without_record(self):
        by_id, summary = self.build([("tests/host/t.c", "DOD-AA-A004")])
        self.assertEqual(by_id["DOD-AA-A004"]["result"], "unknown")
        self.assertEqual(summary["result_counts"], {"unknown": 5})

    def test_recorded_status_is_reported_not_forced(self):
        criteria = [crit("DOD-AA-A001", status="pass")]
        by_id, _ = self.build([], criteria=criteria, registry="tests:\n")
        self.assertEqual(by_id["DOD-AA-A001"]["result"], "pass")
        self.assertEqual(by_id["DOD-AA-A001"]["classification"], NONE)

    def test_counts_by_tier_and_class(self):
        _, summary = self.build([("tests/host/t.c", "DOD-AA-A004")])
        self.assertEqual(summary["classification_counts"],
                         {HOST: 1, HARNESS: 1, DOCS: 3, NONE: 0})
        self.assertEqual(summary["by_tier"]["P2"], {HOST: 0, HARNESS: 0, DOCS: 1, NONE: 0})
        total = sum(sum(c.values()) for c in summary["by_tier"].values())
        self.assertEqual(total, summary["total"])

    def test_description_whitespace_collapsed(self):
        criteria = [crit("DOD-AA-A001", criterion="a\n  b\tc")]
        by_id, _ = self.build([], criteria=criteria, registry="tests:\n")
        self.assertEqual(by_id["DOD-AA-A001"]["description"], "a b c")


class CoverageRankTests(unittest.TestCase):

    def test_ranks_by_executable_sources_then_docs_then_id(self):
        rows = [
            {"id": "DOD-AA-A002", "area": "AA", "tier": "P1", "classification": HOST,
             "host_tests": ["a", "b"], "harness_entries": [], "doc_refs": []},
            {"id": "DOD-AA-A001", "area": "AA", "tier": "P1", "classification": HARNESS,
             "host_tests": ["a"], "harness_entries": ["r"], "doc_refs": []},
            {"id": "DOD-AA-A003", "area": "AA", "tier": "P1", "classification": DOCS,
             "host_tests": [], "harness_entries": [], "doc_refs": ["x", "y", "z"]},
            {"id": "DOD-AA-A004", "area": "AA", "tier": "P1", "classification": NONE,
             "host_tests": [], "harness_entries": [], "doc_refs": []},
        ]
        ranked = [r["id"] for r in criteria_trace.coverage_rank(rows)]
        self.assertEqual(ranked, ["DOD-AA-A001", "DOD-AA-A002", "DOD-AA-A003"])

    def test_limit(self):
        rows = [{"id": "DOD-AA-A%03d" % i, "area": "AA", "tier": "P1", "classification": HOST,
                 "host_tests": ["a"], "harness_entries": [], "doc_refs": []} for i in range(1, 30)]
        self.assertEqual(len(criteria_trace.coverage_rank(rows)), criteria_trace.TOP_N)


class ReadSourceTests(unittest.TestCase):

    def test_text_file_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "a.c"
            path.write_bytes(b"DOD-AA-A001\n")
            self.assertEqual(criteria_trace.read_source(path), "DOD-AA-A001\n")

    def test_binary_file_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "a.bin"
            path.write_bytes(b"DOD-AA-A001\0\0")
            self.assertIsNone(criteria_trace.read_source(path))

    def test_missing_file_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(criteria_trace.read_source(pathlib.Path(tmp) / "none"))


class CollectTests(unittest.TestCase):

    def make_tree(self, tmp):
        root = pathlib.Path(tmp)
        (root / "tests" / "conformance").mkdir(parents=True)
        (root / "tests" / "harness").mkdir(parents=True)
        (root / "tests" / "host").mkdir(parents=True)
        (root / "tests" / "unit").mkdir(parents=True)
        (root / "tests" / "conformance" / "criteria.json").write_text(json.dumps([
            crit("DOD-AA-A001"), crit("DOD-AA-A002")]), encoding="utf-8")
        (root / "tests" / "conformance" / "thresholds.json").write_text(
            '{"DOD-AA-A001": {}, "DOD-AA-A002": {}}', encoding="utf-8")
        (root / "tests" / "harness" / "registry.yaml").write_text(REGISTRY, encoding="utf-8")
        (root / "tests" / "host" / "t.c").write_text("/* DOD-AA-A002 */\n", encoding="utf-8")
        (root / "tests" / "unit" / "test_fx.py").write_text("DOD-AA-A001\n", encoding="utf-8")
        (root / "tests" / "host" / "blob.bin").write_bytes(b"DOD-AA-A001\0")
        return root

    def test_collect_and_classify_temp_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.make_tree(tmp)
            criteria, registry_text, sources = criteria_trace.collect(root)
            rels = [rel for rel, _ in sources]
            self.assertIn("tests/host/t.c", rels)
            self.assertIn("tests/unit/test_fx.py", rels)
            self.assertNotIn("tests/conformance/criteria.json", rels)
            self.assertNotIn("tests/conformance/thresholds.json", rels)
            self.assertNotIn("tests/harness/registry.yaml", rels)
            self.assertNotIn("tests/host/blob.bin", rels)
            rows, _ = criteria_trace.build_matrix(criteria, registry_text, sources)
            by_id = {r["id"]: r for r in rows}
            self.assertEqual(by_id["DOD-AA-A002"]["classification"], HOST)
            self.assertEqual(by_id["DOD-AA-A001"]["classification"], HARNESS)
            self.assertIn("tests/unit/test_fx.py", by_id["DOD-AA-A001"]["doc_refs"])

    def test_list_test_files_walk_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.make_tree(tmp)
            files = criteria_trace.list_test_files(root)
            self.assertIn("tests/host/t.c", files)
            self.assertTrue(all(f.startswith("tests/") for f in files))
            self.assertEqual(files, sorted(files))

    def test_list_test_files_missing_tests_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(criteria_trace.list_test_files(pathlib.Path(tmp)), [])

    def test_load_criteria_rejects_duplicate(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "c.json"
            path.write_text(json.dumps([crit("DOD-AA-A001"), crit("DOD-AA-A001")]), encoding="utf-8")
            with self.assertRaises(ValueError):
                criteria_trace.load_criteria(path)

    def test_load_criteria_rejects_missing_field(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "c.json"
            entry = crit("DOD-AA-A001")
            del entry["tier"]
            path.write_text(json.dumps([entry]), encoding="utf-8")
            with self.assertRaises(ValueError):
                criteria_trace.load_criteria(path)


class RenderTests(unittest.TestCase):

    def setUp(self):
        criteria = [crit("DOD-AA-A001", criterion="Host"), crit("DOD-AA-A002", criterion="Nothing | pipe")]
        self.rows, self.summary = criteria_trace.build_matrix(
            criteria, "tests:\n", [("tests/host/t.c", "DOD-AA-A001")])

    def test_markdown_is_deterministic(self):
        first = criteria_trace.render_markdown(self.rows, self.summary)
        second = criteria_trace.render_markdown(self.rows, self.summary)
        self.assertEqual(first, second)

    def test_markdown_escapes_pipes(self):
        md = criteria_trace.render_markdown(self.rows, self.summary)
        self.assertIn("Nothing \\| pipe", md)

    def test_markdown_lists_no_evidence_by_area(self):
        md = criteria_trace.render_markdown(self.rows, self.summary)
        self.assertIn("### AA (1)", md)
        self.assertIn("| DOD-AA-A002 |", md)
        self.assertNotIn("| DOD-AA-A001 | P1 |", md)

    def test_markdown_states_not_a_results_report(self):
        md = criteria_trace.render_markdown(self.rows, self.summary)
        self.assertIn("not a results report", md)
        self.assertIn("Results: unknown 2", md)

    def test_markdown_has_no_timestamp(self):
        md = criteria_trace.render_markdown(self.rows, self.summary)
        self.assertNotRegex(md, r"\d{4}-\d{2}-\d{2}")

    def test_json_round_trip(self):
        doc = json.loads(criteria_trace.render_json(self.rows, self.summary))
        self.assertEqual(doc["summary"]["total"], 2)
        self.assertEqual(doc["criteria"][0]["classification"], HOST)

    def test_text_summary(self):
        text = criteria_trace.render_text(self.rows, self.summary)
        self.assertIn("criteria: 2", text)
        self.assertIn("NO-EVIDENCE: 1", text)


class RealRepositoryTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        criteria, registry_text, sources = criteria_trace.collect(REPO)
        cls.rows, cls.summary = criteria_trace.build_matrix(criteria, registry_text, sources)

    def test_every_criterion_classified(self):
        self.assertEqual(len(self.rows), 423)
        self.assertEqual(sum(self.summary["classification_counts"].values()), 423)
        self.assertTrue(all(r["classification"] in criteria_trace.CLASSES for r in self.rows))

    def test_no_result_claims_a_pass(self):
        self.assertEqual(set(self.summary["result_counts"]), {"unknown"})

    def test_registry_active_entries_are_harness(self):
        by_id = {r["id"]: r for r in self.rows}
        self.assertEqual(by_id["DOD-ARCH-A001"]["classification"], HARNESS)
        self.assertEqual(by_id["DOD-ARCH-A001"]["registry"], "active")

    def test_unimplemented_entries_never_harness(self):
        by_id = {r["id"]: r for r in self.rows}
        self.assertNotEqual(by_id["DOD-NT-A001"]["classification"], HARNESS)
        self.assertEqual(by_id["DOD-NT-A001"]["registry"], "placeholder")


class MainTests(unittest.TestCase):

    def test_selftest_passes(self):
        self.assertEqual(quiet(criteria_trace.main, ["--selftest"]), 0)

    def test_json_out_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "trace.json"
            self.assertEqual(quiet(criteria_trace.main, ["--json", "--out", str(out)]), 0)
            doc = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(doc["summary"]["total"], 423)
            self.assertEqual(len(doc["criteria"]), 423)

    def test_markdown_out_file_is_lf(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "trace.md"
            self.assertEqual(quiet(criteria_trace.main, ["--markdown", "--out", str(out)]), 0)
            raw = out.read_bytes()
            self.assertNotIn(b"\r\n", raw)
            self.assertTrue(raw.startswith(b"# Criteria traceability matrix"))

    def test_unwritable_out_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "missing-dir" / "trace.md"
            self.assertEqual(quiet(criteria_trace.main, ["--markdown", "--out", str(out)]), 1)


if __name__ == "__main__":
    unittest.main()
