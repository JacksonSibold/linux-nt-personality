"""Unit tests for tools/criteria_roadmap.py.

The script is loaded by path (main() sits behind a __main__ guard). Pure helpers
are tested with small synthetic inputs. The real-repository tests only read
repository files; output files go to a temporary directory.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "criteria_roadmap.py"

_spec = importlib.util.spec_from_file_location("criteria_roadmap", SCRIPT)
criteria_roadmap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(criteria_roadmap)

MILESTONES = criteria_roadmap.MILESTONES
NEVER = criteria_roadmap.NEVER
WORK_ITEMS = criteria_roadmap.WORK_ITEMS


def quiet(fn, *args, **kwargs):
    """Call fn while swallowing its stdout and stderr. Returns fn's result."""
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return fn(*args, **kwargs)


def crit(cid, domain="PE", tier="P1", dep="SELF", criterion="Text", verification="", status="not_implemented"):
    return {"id": cid, "domain": domain, "tier": tier, "dependency_class": dep,
            "criterion": criterion, "verification": verification, "status": status}


def info_for(entry, deps=(), level=0, never=False, closure_points=1, effort="S"):
    return {"entry": entry, "deps": set(deps), "level": level, "closure_points": closure_points,
            "own_effort": effort, "never": never, "effective_level": None}


_REAL = {}


def real_model():
    """The model for the real repository, built once per test run."""
    if not _REAL:
        _REAL["model"] = criteria_roadmap.build_real_model()
    return _REAL["model"]


class WorkItemGraphTests(unittest.TestCase):
    def test_every_prerequisite_exists(self):
        for wid, item in WORK_ITEMS.items():
            for p in item["prereqs"]:
                self.assertIn(p, WORK_ITEMS, wid)

    def test_graph_is_acyclic(self):
        prereq = {w: WORK_ITEMS[w]["prereqs"] for w in WORK_ITEMS}
        self.assertFalse(criteria_roadmap._has_cycle(prereq))

    def test_known_levels(self):
        self.assertEqual(criteria_roadmap.work_item_level("WI-HARNESS"), 0)
        self.assertEqual(criteria_roadmap.work_item_level("WI-KHARNESS"), 1)
        self.assertEqual(criteria_roadmap.work_item_level("WI-PROC-CREATE"), 3)

    def test_sentinels_have_no_level(self):
        self.assertIsNone(criteria_roadmap.work_item_level("WI-EXT-GATED"))
        self.assertIsNone(criteria_roadmap.work_item_level("WI-OUT-OF-SCOPE"))

    def test_efforts_are_valid(self):
        for wid, item in WORK_ITEMS.items():
            self.assertIn(item["effort"], criteria_roadmap.EFFORTS, wid)

    def test_cycle_detector_finds_a_cycle(self):
        self.assertTrue(criteria_roadmap._has_cycle({"A": ("B",), "B": ("A",)}))
        self.assertFalse(criteria_roadmap._has_cycle({"A": ("B",), "B": ()}))

    def test_closure_includes_transitive_prerequisites(self):
        closure = criteria_roadmap.closure_of(["WI-PROC-CREATE"])
        self.assertIn("WI-KBOOT", closure)
        self.assertIn("WI-OD-PROC", closure)
        self.assertIn("WI-MM-VAD", closure)


class ClassificationTests(unittest.TestCase):
    def test_doc_only_detection(self):
        self.assertTrue(criteria_roadmap.is_doc_only({"verification": "`dod docs-lint --policy`"}))
        self.assertFalse(criteria_roadmap.is_doc_only({"verification": "docs-lint and canary sweep"}))
        self.assertFalse(criteria_roadmap.is_doc_only({"verification": "`dod run DOD-PE-001`"}))

    def test_labels_follow_environment_and_area(self):
        entry = crit("DOD-ZZ-A001", domain="PE")
        self.assertEqual(criteria_roadmap.labels_for(entry, "kernel"), ["PE-LOADER", "NT-KERNEL"])
        self.assertEqual(criteria_roadmap.labels_for(entry, "userspace"), ["PE-LOADER"])
        bare = crit("DOD-ZZ-A001", domain="ARCH")
        self.assertEqual(criteria_roadmap.labels_for(bare, "userspace"), ["USERSPACE"])

    def test_gated_criterion_gets_gated_label(self):
        entry = crit("DOD-ZZ-A001", domain="AC", dep="EXT-GATED")
        self.assertIn("EXT-GATED", criteria_roadmap.labels_for(entry, "EXT-GATED"))

    def test_work_items_for_gated_out_of_scope_and_doc_only(self):
        gated = crit("DOD-ZZ-A001", domain="AC", dep="EXT-GATED")
        self.assertEqual(criteria_roadmap.work_items_for(gated, "EXT-GATED")[0], "WI-EXT-GATED")
        oos = crit("DOD-ZZ-A001", domain="GDK")
        self.assertEqual(criteria_roadmap.work_items_for(oos, "userspace"), ["WI-OUT-OF-SCOPE"])
        docs = crit("DOD-ZZ-A008", domain="SEC", verification="`dod docs-lint --policy`")
        self.assertEqual(criteria_roadmap.work_items_for(docs, "userspace"), ["WI-POLICY-DOCS"])

    def test_harness_and_environment_items_are_added(self):
        entry = crit("DOD-ZZ-A001", domain="PE")
        wis = criteria_roadmap.work_items_for(entry, "kernel")
        self.assertEqual(wis[0], "WI-HARNESS")
        self.assertIn("WI-KBOOT", wis)
        self.assertIn("WI-KHARNESS", wis)
        self.assertIn("WI-LDR-3", wis)

    def test_keyword_refinement_narrows_nt_criteria(self):
        ntstatus = crit("DOD-ZZ-A001", domain="NT", criterion="NTSTATUS to Win32 error mapping table")
        wis = criteria_roadmap.work_items_for(ntstatus, "windows-differential")
        self.assertIn("WI-NTSTATUS-FULL", wis)
        self.assertNotIn("WI-WIN32-CORE", wis)
        generic = crit("DOD-ZZ-B007", domain="NT", criterion="Time system")
        self.assertIn("WI-WIN32-CORE", criteria_roadmap.work_items_for(generic, "windows-differential"))

    def test_milestone_bounds(self):
        self.assertEqual(criteria_roadmap.milestone_of_level(0), "M-A")
        self.assertEqual(criteria_roadmap.milestone_of_level(5), "M-F")
        self.assertEqual(criteria_roadmap.milestone_of_level(9), "M-F")
        self.assertEqual(criteria_roadmap.milestone_of_level(None), NEVER)


class EffortTests(unittest.TestCase):
    def test_userspace_partial_area_is_medium(self):
        entry = crit("DOD-ZZ-A010", domain="PE")
        bucket, reason = criteria_roadmap.criterion_effort(entry, "userspace", 1, False, False, False)
        self.assertEqual(bucket, "M")
        self.assertIn("partial", reason)

    def test_kernel_env_and_large_volume_raise_the_bucket(self):
        entry = crit("DOD-ZZ-A003", domain="PE")
        self.assertEqual(criteria_roadmap.criterion_effort(entry, "kernel", 1, False, False, False)[0], "L")
        self.assertEqual(criteria_roadmap.criterion_effort(entry, "kernel", 1_000_000, False, False, False)[0], "XL")

    def test_missing_area_is_capped_at_xl(self):
        entry = crit("DOD-ZZ-A001", domain="NET")
        self.assertEqual(criteria_roadmap.criterion_effort(entry, "vm", 10**9, False, False, False)[0], "XL")

    def test_unknown_area_and_gated_and_out_of_scope_are_unknown(self):
        entry = crit("DOD-ZZ-A001", domain="XX")
        self.assertEqual(criteria_roadmap.criterion_effort(entry, "userspace", 1, False, False, False)[0], "unknown")
        gated = crit("DOD-ZZ-A001", domain="AC", dep="EXT-GATED")
        self.assertEqual(criteria_roadmap.criterion_effort(gated, "EXT-GATED", "x", True, False, False)[0], "unknown")
        self.assertEqual(criteria_roadmap.criterion_effort(entry, "userspace", 1, False, True, False)[0], "unknown")

    def test_doc_only_is_small(self):
        entry = crit("DOD-ZZ-A008", domain="SEC")
        self.assertEqual(criteria_roadmap.criterion_effort(entry, "userspace", 1, False, False, True)[0], "S")


class OrderingTests(unittest.TestCase):
    def test_dependency_comes_before_a_cheaper_dependent(self):
        cheap = info_for(crit("DOD-ZZ-A001"), closure_points=9)
        needy = info_for(crit("DOD-ZZ-A002", tier="P0"), deps={"DOD-ZZ-A001"}, closure_points=1)
        order = criteria_roadmap._topo_order({"DOD-ZZ-A002": needy, "DOD-ZZ-A001": cheap})
        self.assertEqual(order, ["DOD-ZZ-A001", "DOD-ZZ-A002"])

    def test_cheapest_first_within_a_milestone(self):
        a = info_for(crit("DOD-ZZ-A001", tier="P1"), closure_points=8)
        b = info_for(crit("DOD-ZZ-A002", tier="P1"), closure_points=2)
        order = criteria_roadmap._topo_order({"DOD-ZZ-A001": a, "DOD-ZZ-A002": b})
        self.assertEqual(order, ["DOD-ZZ-A002", "DOD-ZZ-A001"])

    def test_cycle_raises(self):
        a = info_for(crit("DOD-ZZ-A001"), deps={"DOD-ZZ-A002"})
        b = info_for(crit("DOD-ZZ-A002"), deps={"DOD-ZZ-A001"})
        with self.assertRaises(ValueError):
            criteria_roadmap._topo_order({"DOD-ZZ-A001": a, "DOD-ZZ-A002": b})

    def test_never_criteria_are_not_ordered(self):
        a = info_for(crit("DOD-ZZ-A001"), never=True)
        self.assertEqual(criteria_roadmap._topo_order({"DOD-ZZ-A001": a}), [])


class ParsingTests(unittest.TestCase):
    def test_gated_register_stops_at_next_section(self):
        text = ("### §3.1 EXT-GATED register (governance)\n\n"
                "| Criteria | Artifact required | Controlling party |\n|---|---|---|\n"
                "| `DOD-ZZ-A001`, `DOD-ZZ-B003` | kernel attestation | Riot / Epic |\n\n## §4 next\n"
                "| `DOD-NOT-IN-SECTION` | x | y |\n")
        reg = criteria_roadmap.parse_gated_register(text)
        self.assertEqual(list(reg), ["DOD-ZZ-A001", "DOD-ZZ-B003"])
        self.assertEqual(reg["DOD-ZZ-A001"], "Riot / Epic")

    def test_decision_status_parsing(self):
        text = ("| [0029](0029-x.md) | Title | open | ntdll LDR-1 (High) | |\n"
                "| [0003](0003-y.md) | Title | fixed (option 1; option 2 open) | kernel H-2 | d7 |\n"
                "| not a row |\n")
        status = criteria_roadmap.parse_decision_status(text)
        self.assertEqual(status, {"0029": "open", "0003": "fixed"})

    def test_ref_text_reads_decision_status(self):
        self.assertEqual(criteria_roadmap.ref_text("ADR-0029"), "ADR-0029 (open)")
        self.assertEqual(criteria_roadmap.ref_text("OD-17"), "OD-17")
        self.assertEqual(criteria_roadmap.ref_text("DOC:docs/x.md"), "docs/x.md")

    def test_load_criteria_rejects_duplicates(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "c.json"
            entry = crit("DOD-ZZ-A001")
            path.write_text(json.dumps([entry, entry]), encoding="utf-8")
            with self.assertRaises(ValueError):
                criteria_roadmap.load_criteria(path)

    def test_load_criteria_rejects_missing_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "c.json"
            path.write_text(json.dumps([{"id": "DOD-ZZ-A001"}]), encoding="utf-8")
            with self.assertRaises(ValueError):
                criteria_roadmap.load_criteria(path)


class RepositoryTests(unittest.TestCase):
    def test_selftest_passes(self):
        self.assertEqual(quiet(criteria_roadmap.selftest), [])

    def test_main_selftest_exits_zero(self):
        self.assertEqual(quiet(criteria_roadmap.main, ["--selftest"]), 0)

    def test_reference_problems_are_empty(self):
        self.assertEqual(criteria_roadmap.repo_reference_problems(), [])

    def test_every_criterion_is_placed_once(self):
        model = real_model()
        ids = [r["id"] for r in model["rows"] + model["never"]]
        self.assertEqual(len(ids), 423)
        self.assertEqual(len(set(ids)), 423)

    def test_never_list_is_gated_plus_out_of_scope(self):
        model = real_model()
        never = {r["id"] for r in model["never"]}
        criteria = criteria_roadmap.load_criteria()
        gated = {c["id"] for c in criteria if c["dependency_class"] == "EXT-GATED"}
        out = {c["id"] for c in criteria if c["domain"] in ("TELEM", "GDK")}
        self.assertEqual(never, gated | out)
        self.assertEqual(len(gated), 17)

    def test_register_mismatch_is_only_drm_a004(self):
        model = real_model()
        mismatch = [r for r in model["rows"] + model["never"]
                    if r["gated_register"] and r["dependency_class"] != "EXT-GATED"]
        self.assertEqual([r["area"] for r in mismatch], ["DRM"])

    def test_order_respects_explicit_dependencies(self):
        model = real_model()
        position = {r["id"]: r["order"] for r in model["rows"]}
        for r in model["rows"]:
            for dep in r["depends_on"]:
                if dep in position:
                    self.assertLess(position[dep], r["order"], f"{r['id']} before {dep}")
        drv_rows = [r for r in model["rows"] if r["area"] == "DRV" and r["depends_on"]]
        dx_ids = {c["id"] for c in criteria_roadmap.load_criteria() if c["domain"] == "DX"}
        self.assertTrue(drv_rows)
        self.assertTrue(dx_ids <= set(drv_rows[0]["depends_on"]))

    def test_milestones_and_efforts_are_valid(self):
        model = real_model()
        for r in model["rows"]:
            self.assertIn(r["milestone"], MILESTONES)
        for r in model["rows"] + model["never"]:
            self.assertIn(r["effort"], criteria_roadmap.EFFORTS)

    def test_first_ten_are_repository_local_or_policy_first(self):
        model = real_model()
        entries = {c["id"]: c for c in criteria_roadmap.load_criteria()}
        first = model["rows"][:3]
        self.assertTrue(all(criteria_roadmap.is_doc_only(entries[r["id"]]) for r in first))
        self.assertTrue(all(r["milestone"] == "M-A" for r in first))

    def test_markdown_lists_every_criterion(self):
        model = real_model()
        summary = criteria_roadmap.summarize(model)
        text = criteria_roadmap.render_markdown(model, summary)
        for r in model["rows"] + model["never"]:
            self.assertIn(f"`{r['id']}`", text)
        self.assertIn("## 5. Criteria that cannot pass", text)
        self.assertNotIn("❤", text)

    def test_json_document_has_expected_keys(self):
        model = real_model()
        summary = criteria_roadmap.summarize(model)
        payload = json.loads(criteria_roadmap.render_json(model, summary))
        self.assertEqual(set(payload), {"summary", "milestones", "work_items", "criteria", "never"})
        self.assertEqual(payload["summary"]["criteria"], 423)
        self.assertEqual(len(payload["criteria"]) + len(payload["never"]), 423)

    def test_out_file_is_written_to_a_temporary_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "roadmap.md"
            code = quiet(criteria_roadmap.main, ["--markdown", "--out", str(out)])
            self.assertEqual(code, 0)
            self.assertIn("# Conformance criteria roadmap", out.read_text(encoding="utf-8"))

    def test_text_summary_reports_counts(self):
        model = real_model()
        text = criteria_roadmap.render_text(model, criteria_roadmap.summarize(model))
        self.assertIn("criteria 423", text)
        self.assertIn("M-A:", text)


if __name__ == "__main__":
    unittest.main()
