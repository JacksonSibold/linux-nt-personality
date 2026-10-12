"""Unit tests for build/merge_dod.py.

The script is importable (main() sits behind a __main__ guard), so it is loaded
by path. Pure helpers are tested directly with small inputs. parse() and main()
are tested against temporary Markdown fixtures, with the module-level DOCS and
OUT paths overridden so nothing outside the temp directory is touched.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "merge_dod.py"

_spec = importlib.util.spec_from_file_location("merge_dod", SCRIPT)
merge_dod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(merge_dod)


def quiet(fn, *args):
    """Call fn while swallowing its stdout. Returns fn's result."""
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args)


class SplitRowTests(unittest.TestCase):

    def test_splits_cells_and_strips_outer_pipes(self):
        self.assertEqual(merge_dod.split_row("| a | b | c |"), ["a", "b", "c"])

    def test_escaped_pipe_stays_inside_cell(self):
        self.assertEqual(merge_dod.split_row(r"| a | b \| c | d |"),
                         ["a", r"b \| c", "d"])

    def test_no_outer_pipes(self):
        self.assertEqual(merge_dod.split_row("a | b"), ["a", "b"])


class ClassificationTests(unittest.TestCase):

    def test_canon_domain_applies_alias(self):
        self.assertEqual(merge_dod.canon_domain("DOD-SOCK-001"), "NET")
        self.assertEqual(merge_dod.canon_domain("DOD-VM-002"), "BOOTVM")
        self.assertEqual(merge_dod.canon_domain("DOD-LNX-003"), "LINUXR")

    def test_canon_domain_passes_through_unknown(self):
        self.assertEqual(merge_dod.canon_domain("DOD-MM-010"), "MM")

    def test_find_dep_exact_token(self):
        self.assertEqual(merge_dod.find_dep(["SELF"]), "SELF")

    def test_find_dep_strips_bold_and_backticks(self):
        self.assertEqual(merge_dod.find_dep(["**`EXT-RE`**"]), "EXT-RE")

    def test_find_dep_is_case_insensitive(self):
        self.assertEqual(merge_dod.find_dep(["ext-doc"]), "EXT-DOC")

    def test_find_dep_finds_embedded_class(self):
        self.assertEqual(merge_dod.find_dep(["", "EXT-GATED (MS key)"]), "EXT-GATED")

    def test_find_dep_none_when_absent(self):
        self.assertIsNone(merge_dod.find_dep(["nothing", "here"]))

    def test_find_tier_plain_and_bold(self):
        self.assertEqual(merge_dod.find_tier(["x", "P2"]), "P2")
        self.assertEqual(merge_dod.find_tier(["**P1**"]), "P1")

    def test_find_tier_rejects_out_of_range_and_missing(self):
        self.assertIsNone(merge_dod.find_tier(["P4"]))
        self.assertIsNone(merge_dod.find_tier(["SPRINT"]))


class PrecedenceTests(unittest.TestCase):

    def test_worst_dep_takes_highest_ranked_class(self):
        self.assertEqual(merge_dod.worst_dep("SELF", "EXT-RE"), "EXT-RE")
        self.assertEqual(merge_dod.worst_dep("EXT-GATED", "EXT-DOC"), "EXT-GATED")

    def test_worst_dep_ignores_missing_side(self):
        self.assertEqual(merge_dod.worst_dep(None, "SELF"), "SELF")

    def test_worst_dep_defaults_to_ext_doc(self):
        self.assertEqual(merge_dod.worst_dep(None, None), "EXT-DOC")

    def test_higher_tier_takes_highest_priority(self):
        # P0 is the highest priority (TIER_ORDER ranks it last).
        self.assertEqual(merge_dod.higher_tier("P1", "P3"), "P1")
        self.assertEqual(merge_dod.higher_tier("P0", "P2"), "P0")
        self.assertEqual(merge_dod.higher_tier("P0", "P0"), "P0")

    def test_higher_tier_ignores_missing_side(self):
        self.assertEqual(merge_dod.higher_tier(None, "P2"), "P2")

    def test_higher_tier_defaults_to_p1(self):
        self.assertEqual(merge_dod.higher_tier(None, None), "P1")


class ParseTests(unittest.TestCase):

    A_TABLE = (
        "# Agent A\n"
        "\n"
        "| ID | Tier | Criterion | Verification | Threshold | Dep |\n"
        "|---|---|---|---|---|---|\n"
        "| DOD-NET-001 | P0 | socket opens | run probe | exit 0 | SELF |\n"
        "| **DOD-NET-002** | **P1** | socket closes | run probe | exit 0 | EXT-RE |\n"
        "| DOD-NET-003 | P2 | prose only | not a row | - |\n"
    )

    def _parse(self, text, source="A"):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "dod.md"
            path.write_text(text, encoding="utf-8")
            return quiet(merge_dod.parse, str(path), source)

    def test_six_column_rows_are_parsed(self):
        rows = self._parse(self.A_TABLE)
        self.assertEqual([r["raw_id"] for r in rows],
                         ["DOD-NET-001", "DOD-NET-002", "DOD-NET-003"])
        first = rows[0]
        self.assertEqual(first["domain"], "NET")
        self.assertEqual(first["tier"], "P0")
        self.assertEqual(first["dependency_class"], "SELF")
        self.assertEqual(first["criterion"], "socket opens")
        self.assertEqual(first["verification"], "run probe")
        self.assertEqual(first["threshold"], "exit 0")
        self.assertEqual(first["source"], "A")
        self.assertFalse(first["offshape"])

    def test_bold_id_and_bold_tier_are_understood(self):
        rows = self._parse(self.A_TABLE)
        self.assertEqual(rows[1]["raw_id"], "DOD-NET-002")
        self.assertEqual(rows[1]["tier"], "P1")
        self.assertEqual(rows[1]["dependency_class"], "EXT-RE")

    def test_b_style_domain_column_gets_tier_from_criterion(self):
        text = (
            "| ID | Domain | Criterion | Verification | Threshold | Dep |\n"
            "| DOD-SOCK-001 | Socket | P0 path works | probe | ok | SELF |\n"
            "| DOD-SOCK-002 | Socket | no tier here | probe | ok | EXT-DOC |\n"
        )
        rows = self._parse(text, source="B")
        self.assertEqual(rows[0]["domain"], "NET")
        self.assertEqual(rows[0]["tier"], "P0")
        self.assertIsNone(rows[1]["tier"])

    def test_off_shape_row_is_kept_and_flagged(self):
        text = self.A_TABLE + "| DOD-NET-009 | P1 | registry note | ok |\n"
        rows = self._parse(text)
        extra = [r for r in rows if r["raw_id"] == "DOD-NET-009"][0]
        self.assertTrue(extra["offshape"])
        self.assertEqual(extra["tier"], "P1")

    def test_gated_registry_row_promotes_full_row_and_records_note(self):
        text = (
            "| DOD-NET-001 | P0 | socket opens | run probe | exit 0 | SELF |\n"
            "| DOD-NET-001 | P0 | EXT-GATED registry note |\n"
        )
        rows = self._parse(text)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["dependency_class"], "EXT-GATED")
        self.assertEqual(rows[0]["gated_note"], "EXT-GATED registry note")
        self.assertEqual(rows[0]["criterion"], "socket opens")

    def test_gated_promotion_works_when_registry_row_comes_first(self):
        text = (
            "| DOD-NET-001 | P0 | EXT-GATED registry note |\n"
            "| DOD-NET-001 | P0 | socket opens | run probe | exit 0 | SELF |\n"
        )
        rows = self._parse(text)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["criterion"], "socket opens")
        self.assertEqual(rows[0]["dependency_class"], "EXT-GATED")

    def test_off_shape_row_mentioning_gated_is_classified_gated(self):
        rows = self._parse("| DOD-NET-004 | the gated registry |\n")
        self.assertEqual(rows[0]["dependency_class"], "EXT-GATED")

    def test_non_table_lines_are_ignored(self):
        rows = self._parse("Some prose DOD-NET-001 here.\n")
        self.assertEqual(rows, [])


class MainTests(unittest.TestCase):

    A_DOC = (
        "| ID | Tier | Criterion | Verification | Threshold | Dep |\n"
        "|---|---|---|---|---|---|\n"
        "| DOD-NET-001 | P0 | socket opens | run probe | exit 0 | SELF |\n"
        "| DOD-NET-002 | P1 | socket closes | run probe | exit 0 | EXT-RE |\n"
        "| DOD-NET-003 | P2 | PlayReady playback | run drm | ok | EXT-GATED |\n"
    )
    B_DOC = (
        "| ID | Domain | Criterion | Verification | Threshold | Dep |\n"
        "|---|---|---|---|---|---|\n"
        "| DOD-SOCK-001 | Socket | Steam client login | steam run | ok | EXT-DOC |\n"
        "| DOD-SOCK-002 | Socket | sends packets | run probe | exit 0 | SELF |\n"
    )

    def _run_main(self, tmp_path):
        docs = tmp_path / "docs" / "phase0"
        docs.mkdir(parents=True)
        (docs / "dod-agent-a.md").write_text(self.A_DOC, encoding="utf-8")
        (docs / "dod-agent-b.md").write_text(self.B_DOC, encoding="utf-8")
        out = tmp_path / "tests" / "conformance" / "criteria.json"

        saved = merge_dod.DOCS, merge_dod.OUT
        merge_dod.DOCS, merge_dod.OUT = str(docs), str(out)
        try:
            quiet(merge_dod.main)
        finally:
            merge_dod.DOCS, merge_dod.OUT = saved
        return out

    def test_writes_union_of_both_documents(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self._run_main(pathlib.Path(tmp))
            merged = json.loads(out.read_text(encoding="utf-8"))
        # Nothing is dropped: 3 rows from A plus 2 from B, all in domain NET.
        self.assertEqual(len(merged), 5)
        self.assertEqual([m["id"] for m in merged],
                         ["DOD-NET-A001", "DOD-NET-A002", "DOD-NET-A003",
                          "DOD-NET-B001", "DOD-NET-B002"])

    def test_criteria_from_both_sources_stay_distinct(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self._run_main(pathlib.Path(tmp))
            merged = json.loads(out.read_text(encoding="utf-8"))
        texts = [m["criterion"] for m in merged]
        self.assertIn("socket opens", texts)
        self.assertIn("Steam client login", texts)
        self.assertEqual(len(set(texts)), len(texts))

    def test_record_fields_and_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self._run_main(pathlib.Path(tmp))
            merged = json.loads(out.read_text(encoding="utf-8"))
        by_id = {m["id"]: m for m in merged}

        a1 = by_id["DOD-NET-A001"]
        self.assertEqual(a1["tier"], "P0")
        self.assertEqual(a1["dependency_class"], "SELF")
        self.assertEqual(a1["source"], ["A"])
        self.assertEqual(a1["dup_group"], "NET")
        self.assertEqual(a1["origin_ids"], ["DOD-NET-001"])
        self.assertEqual(a1["status"], "not_implemented")

        # B rows have no tier column, so they default to P1 and keep their class.
        b1 = by_id["DOD-NET-B001"]
        self.assertEqual(b1["tier"], "P1")
        self.assertEqual(b1["dependency_class"], "EXT-DOC")
        self.assertEqual(b1["origin_ids"], ["DOD-SOCK-001"])
        self.assertEqual(b1["source"], ["B"])

    def test_summary_lists_ext_gated_register(self):
        with tempfile.TemporaryDirectory() as tmp:
            docs_tmp = pathlib.Path(tmp)
            docs = docs_tmp / "docs" / "phase0"
            docs.mkdir(parents=True)
            (docs / "dod-agent-a.md").write_text(self.A_DOC, encoding="utf-8")
            (docs / "dod-agent-b.md").write_text(self.B_DOC, encoding="utf-8")
            out = docs_tmp / "criteria.json"
            saved = merge_dod.DOCS, merge_dod.OUT
            merge_dod.DOCS, merge_dod.OUT = str(docs), str(out)
            try:
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    merge_dod.main()
            finally:
                merge_dod.DOCS, merge_dod.OUT = saved
        text = buf.getvalue()
        self.assertIn("TOTAL CRITERIA: 5", text)
        self.assertIn("=== EXT-GATED REGISTER ===", text)
        self.assertIn("DOD-NET-A003", text.split("=== EXT-GATED REGISTER ===")[1])


if __name__ == "__main__":
    unittest.main()
