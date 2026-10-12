"""Branch, error-path and CLI tests for tools/criteria_report.py.

The main() paths are driven with CRITERIA_PATH and README_PATH pointed at
temporary files, so the tracked criteria set is never modified. Text output
is written as bytes to sys.stdout.buffer, so those tests swap in a wrapper.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "criteria_report.py"

_spec = importlib.util.spec_from_file_location("criteria_report", SCRIPT)
cr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cr)

ENTRY = {"id": "DOD-AA-A001", "domain": "AA", "tier": "P0", "dependency_class": "SELF",
         "criterion": "First  criterion\nwith  spaces", "status": "not_implemented"}


def quiet_call(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        result = fn(*args, **kwargs)
    return result, out.getvalue(), err.getvalue()


def bytes_stdout_call(fn, *args, **kwargs):
    """Call fn with a stdout that has a .buffer. Returns (result, decoded bytes)."""
    raw = io.BytesIO()
    wrapper = io.TextIOWrapper(raw, encoding="utf-8", write_through=True)
    with mock.patch.object(sys, "stdout", wrapper), contextlib.redirect_stderr(io.StringIO()):
        result = fn(*args, **kwargs)
    wrapper.flush()
    return result, raw.getvalue().decode("utf-8")


class FileCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name, text):
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return path

    def criteria_file(self, entries):
        return self.write("criteria.json", json.dumps(entries))

    def patch_paths(self, criteria=None, readme=None):
        patches = []
        if criteria is not None:
            patches.append(mock.patch.object(cr, "CRITERIA_PATH", criteria))
        if readme is not None:
            patches.append(mock.patch.object(cr, "README_PATH", readme))
        for p in patches:
            p.start()
            self.addCleanup(p.stop)


class LoadCriteriaTests(FileCase):
    def test_valid_file_loads(self):
        data = cr.load_criteria(self.criteria_file([ENTRY]))
        self.assertEqual(data[0]["id"], "DOD-AA-A001")

    def test_top_level_must_be_list(self):
        with self.assertRaisesRegex(ValueError, "must hold a JSON list"):
            cr.load_criteria(self.write("obj.json", "{}"))

    def test_entry_missing_field_is_named(self):
        bad = dict(ENTRY)
        del bad["tier"]
        with self.assertRaisesRegex(ValueError, r"lacks \['tier'\]"):
            cr.load_criteria(self.criteria_file([bad]))

    def test_duplicate_id_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate id DOD-AA-A001"):
            cr.load_criteria(self.criteria_file([ENTRY, ENTRY]))

    def test_invalid_json_raises_value_error(self):
        with self.assertRaises(ValueError):
            cr.load_criteria(self.write("broken.json", "[{"))


class ReadmeTests(FileCase):
    def test_missing_readme_gives_none(self):
        self.assertIsNone(cr.read_readme_count(self.dir / "absent.md"))

    def test_readme_without_count_gives_none(self):
        self.assertIsNone(cr.read_readme_count(self.write("R.md", "no numbers here\n")))

    def test_readme_count_is_parsed(self):
        path = self.write("R.md", "**7 of 9** full conformance criteria passing\n")
        self.assertEqual(cr.read_readme_count(path),
                         {"passing": 7, "total": 9, "source": "README.md"})


class RenderTests(unittest.TestCase):
    def rows(self):
        return cr.build_rows([ENTRY])

    def test_build_rows_collapses_whitespace_and_maps_status(self):
        row = self.rows()[0]
        self.assertEqual(row["description"], "First criterion with spaces")
        self.assertEqual(row["status"], "unknown")
        self.assertEqual(row["area"], "AA")

    def test_status_placeholders(self):
        self.assertEqual(cr.status_of({"status": "  "}), "unknown")
        self.assertEqual(cr.status_of({"status": None}), "unknown")
        self.assertEqual(cr.status_of({"status": " pass "}), "pass")

    def test_text_with_no_rows_still_has_header_and_totals(self):
        summary = cr.summarize([], None)
        text = cr.render_text([], summary)
        self.assertTrue(text.startswith("ID"))
        self.assertIn("total: 0", text)
        self.assertNotIn("README records", text)

    def test_text_mentions_readme_count_when_known(self):
        readme = {"passing": 1, "total": 2, "source": "README.md"}
        text = cr.render_text(self.rows(), cr.summarize(self.rows(), readme))
        self.assertIn("README records 1 of 2 passing (ids not recorded)", text)

    def test_markdown_without_readme_count(self):
        md = cr.render_markdown(self.rows(), cr.summarize(self.rows(), None))
        self.assertIn("README.md records no pass count.", md)
        self.assertIn("| DOD-AA-A001 | AA |", md)

    def test_json_document_shape(self):
        doc = json.loads(cr.render_json(self.rows(), cr.summarize(self.rows(), None)))
        self.assertEqual(doc["summary"]["status_counts"], {"unknown": 1})
        self.assertIsNone(doc["summary"]["readme_passing"])
        self.assertEqual(doc["criteria"][0]["id"], "DOD-AA-A001")


class MainTests(FileCase):
    def test_default_text_output_on_stdout(self):
        self.patch_paths(self.criteria_file([ENTRY]), self.write("R.md", ""))
        rc, out = bytes_stdout_call(cr.main, [])
        self.assertEqual(rc, 0)
        self.assertIn("DOD-AA-A001", out)

    def test_json_and_markdown_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(cr.main, ["--json", "--markdown"])
        self.assertEqual(cm.exception.code, 2)

    def test_unknown_option_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(cr.main, ["--xml"])
        self.assertEqual(cm.exception.code, 2)

    def test_selftest_exit_status(self):
        rc, out, _ = quiet_call(cr.main, ["--selftest"])
        self.assertEqual(rc, 0)
        self.assertIn("selftest: all checks passed", out)

    def test_selftest_failure_exit_status(self):
        with mock.patch.object(cr, "selftest", return_value=2):
            rc, _out, _ = quiet_call(cr.main, ["--selftest"])
        self.assertEqual(rc, 1)

    def test_out_writes_file_with_lf_endings(self):
        self.patch_paths(self.criteria_file([ENTRY]), self.write("R.md", ""))
        target = self.dir / "report.md"
        rc, out, _ = quiet_call(cr.main, ["--markdown", "--out", str(target)])
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        data = target.read_bytes()
        self.assertIn(b"| DOD-AA-A001 |", data)
        self.assertNotIn(b"\r\n", data)

    def test_invalid_criteria_is_reported_not_raised(self):
        self.patch_paths(self.criteria_file([ENTRY, ENTRY]), self.write("R.md", ""))
        rc, _out, err = quiet_call(cr.main, ["--json"])
        self.assertEqual(rc, 1)
        self.assertIn("criteria_report: error:", err)
        self.assertIn("duplicate id", err)

    def test_unreadable_criteria_is_reported_not_raised(self):
        self.patch_paths(self.dir / "absent.json", self.write("R.md", ""))
        rc, _out, err = quiet_call(cr.main, [])
        self.assertEqual(rc, 1)
        self.assertIn("criteria_report: error:", err)

    def test_unwritable_out_is_reported_not_raised(self):
        self.patch_paths(self.criteria_file([ENTRY]), self.write("R.md", ""))
        target = self.dir / "no-such-dir" / "report.md"
        rc, _out, err = quiet_call(cr.main, ["--out", str(target)])
        self.assertEqual(rc, 1)
        self.assertIn("criteria_report: error: cannot write", err)

    def test_real_criteria_set_in_json(self):
        rc, out = bytes_stdout_call(cr.main, ["--json"])
        self.assertEqual(rc, 0)
        doc = json.loads(out)
        self.assertEqual(doc["summary"]["total"], len(doc["criteria"]))
        self.assertGreater(doc["summary"]["total"], 0)

    def test_script_exit_status_through_interpreter(self):
        ok = subprocess.run([sys.executable, str(SCRIPT), "--selftest"],
                            capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        usage = subprocess.run([sys.executable, str(SCRIPT), "--json", "--markdown"],
                               capture_output=True, text=True)
        self.assertEqual(usage.returncode, 2)


if __name__ == "__main__":
    unittest.main()
