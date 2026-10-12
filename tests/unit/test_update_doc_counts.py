"""Tests for tools/update_doc_counts.py.

Every test builds a small tree in a temporary directory and works on that tree
only, so the real documentation is never judged or rewritten by these tests.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "update_doc_counts.py"

_spec = importlib.util.spec_from_file_location("update_doc_counts", SCRIPT)
udc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(udc)

REVIEW_INDEX = """# Review

## Reports

| Area | File | High | Medium | Low | Info |
|---|---|---|---|---|---|
| One | [a.md](a.md) | 1 | 2 | 3 | 0 |
| Two | [b.md](b.md) | 0 | 1 | 1 | 1 |
| **Total** | | **1** | **3** | **4** | **1** |

## Fix status

| Report | Findings | Fixed | Partly | Blocked | No change | Open |
|---|---|---|---|---|---|---|
| [a.md](a.md) | 6 | 2 | 1 | 0 | 0 | 3 |
| **Total** | **6** | **2** | **1** | **0** | **0** | **3** |
"""

DECISIONS_INDEX = """# Decisions

| Record | Title | Status | Finding | Commit |
|---|---|---|---|---|
| [0001](0001-a.md) | A | open | x | |
| [0002](0002-b.md) | B | fixed (option 1; option 2 still open) | y | abc1234 |
| [0003](0003-c.md) | C | blocked | z | |
| [0004](0004-d.md) | D | no-change | w | |
"""

TARGET_TEXT = (
    "Tests: <!-- counts:unit_tests:begin -->0<!-- counts:unit_tests:end -->.\n"
    "Modules: <!-- counts:unit_modules:begin -->9<!-- counts:unit_modules:end -->.\n"
)


def write(root, rel, text):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def build_tree(root):
    """A complete fixture: unit tests, review and decision indexes, and the target docs."""
    write(root, "tests/unit/test_one.py",
          "import unittest\n\n"
          "class T(unittest.TestCase):\n"
          "    def test_a(self):\n        pass\n\n"
          "    def test_b(self):\n        pass\n\n"
          "    def helper(self):\n        pass\n\n"
          "def test_c():\n    text = 'def test_fake(): pass'\n")
    write(root, "tests/unit/test_two.py", "def test_d():\n    pass\n")
    write(root, "tests/unit/helper.py", "def test_not_counted():\n    pass\n")
    write(root, "docs/review/INDEX.md", REVIEW_INDEX)
    write(root, "docs/decisions/INDEX.md", DECISIONS_INDEX)
    for rel in udc.TARGETS:
        write(root, rel, TARGET_TEXT)
    write(root, "docs/status/PROGRESS.md", "No blocks here.\n")


class UnitTestCount(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_counts_test_functions_in_test_modules_only(self):
        write(self.root, "tests/unit/test_one.py",
              "def test_a():\n    pass\n\nclass K:\n    def test_b(self):\n        pass\n"
              "    def helper(self):\n        pass\n\n"
              "text = 'def test_in_string(): pass'  # def test_in_comment\n")
        write(self.root, "tests/unit/test_two.py", "def test_c():\n    pass\n")
        write(self.root, "tests/unit/other.py", "def test_ignored():\n    pass\n")
        self.assertEqual(udc.count_unit_tests(self.root), (2, 3))

    def test_missing_unit_directory_is_an_error(self):
        with self.assertRaises(udc.CountError):
            udc.count_unit_tests(self.root)

    def test_syntax_error_names_the_module(self):
        write(self.root, "tests/unit/test_bad.py", "def test_(:\n")
        with self.assertRaisesRegex(udc.CountError, "test_bad.py"):
            udc.count_unit_tests(self.root)


class SourceTables(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_review_totals_and_fix_status(self):
        write(self.root, "docs/review/INDEX.md", REVIEW_INDEX)
        facts = udc.review_facts(self.root)
        self.assertEqual(facts["review_reports"], 2)
        self.assertEqual(facts["review_findings"], 9)
        self.assertEqual((facts["review_high"], facts["review_medium"],
                          facts["review_low"], facts["review_info"]), (1, 3, 4, 1))
        self.assertEqual(facts["fix_findings"], 6)
        self.assertEqual((facts["fix_fixed"], facts["fix_partly"], facts["fix_blocked"],
                          facts["fix_no_change"], facts["fix_open"]), (2, 1, 0, 0, 3))

    def test_review_total_row_must_match_the_rows(self):
        bad = REVIEW_INDEX.replace("| **Total** | | **1** | **3** | **4** | **1** |",
                                   "| **Total** | | **2** | **3** | **4** | **1** |")
        write(self.root, "docs/review/INDEX.md", bad)
        with self.assertRaisesRegex(udc.CountError, "high"):
            udc.review_facts(self.root)

    def test_fix_status_must_add_up(self):
        bad = REVIEW_INDEX.replace("| [a.md](a.md) | 6 | 2 | 1 | 0 | 0 | 3 |",
                                   "| [a.md](a.md) | 6 | 2 | 1 | 0 | 0 | 2 |").replace(
            "| **Total** | **6** | **2** | **1** | **0** | **0** | **3** |",
            "| **Total** | **6** | **2** | **1** | **0** | **0** | **2** |")
        write(self.root, "docs/review/INDEX.md", bad)
        with self.assertRaisesRegex(udc.CountError, "do not add up"):
            udc.review_facts(self.root)

    def test_decision_statuses(self):
        write(self.root, "docs/decisions/INDEX.md", DECISIONS_INDEX)
        facts = udc.adr_facts(self.root)
        self.assertEqual(facts, {"adr_total": 4, "adr_fixed": 1, "adr_open": 1,
                                 "adr_blocked": 1, "adr_no_change": 1})

    def test_unknown_decision_status_is_an_error(self):
        write(self.root, "docs/decisions/INDEX.md",
              DECISIONS_INDEX.replace("| blocked |", "| maybe |"))
        with self.assertRaisesRegex(udc.CountError, "unknown status"):
            udc.adr_facts(self.root)

    def test_duplicate_decision_is_an_error(self):
        write(self.root, "docs/decisions/INDEX.md",
              DECISIONS_INDEX + "| [0001](0001-a.md) | A | open | x | |\n")
        with self.assertRaisesRegex(udc.CountError, "listed twice"):
            udc.adr_facts(self.root)


class TrackedFilesFallback(unittest.TestCase):
    def test_walk_outside_git_skips_dot_git_and_uses_forward_slashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            write(root, "a.txt", "a")
            write(root, "sub/b.txt", "b")
            write(root, ".git/config", "x")
            names = sorted(udc.tracked_files(root))
            self.assertEqual(names, ["a.txt", "sub/b.txt"])


class SyncAndMain(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        build_tree(self.root)

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = udc.main(["--root", str(self.root), *args])
        return code, out.getvalue()

    def read(self, rel):
        return udc.read_text(self.root / rel)

    def test_check_reports_stale_blocks_and_changes_nothing(self):
        before = self.read("README.md")
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("block 'unit_tests' reads '0', repository gives '4'", out)
        self.assertEqual(self.read("README.md"), before)

    def test_write_then_check_passes(self):
        code, out = self.run_main("--write")
        self.assertEqual(code, 0)
        self.assertIn("updated README.md: block 'unit_tests'", out)
        self.assertEqual(self.read("README.md"),
                         "Tests: <!-- counts:unit_tests:begin -->4<!-- counts:unit_tests:end -->.\n"
                         "Modules: <!-- counts:unit_modules:begin -->2<!-- counts:unit_modules:end -->.\n")
        code, out = self.run_main("--check")
        self.assertEqual(code, 0)
        self.assertIn("doc counts are current", out)

    def test_write_keeps_crlf_line_endings(self):
        crlf = TARGET_TEXT.replace("\n", "\r\n")
        udc_path = self.root / "CONTRIBUTING.md"
        with open(udc_path, "w", encoding="utf-8", newline="") as handle:
            handle.write(crlf)
        self.run_main("--write")
        with open(udc_path, "rb") as handle:
            data = handle.read()
        self.assertIn(b"\r\n", data)
        self.assertNotIn(b"\r\r\n", data)
        self.assertNotIn(b"\n\r", data)

    def test_missing_end_marker_is_a_problem(self):
        write(self.root, "README.md", "<!-- counts:unit_tests:begin -->3\n")
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("README.md: a counts:begin marker has no matching counts:end marker", out)

    def test_unknown_block_name_is_a_problem(self):
        write(self.root, "README.md",
              "<!-- counts:nope:begin -->1<!-- counts:nope:end -->\n")
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("unknown counts block 'nope'", out)

    def test_missing_target_file_is_a_problem(self):
        (self.root / "docs/KNOWN-ISSUES.md").unlink()
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("docs/KNOWN-ISSUES.md: target file is missing", out)

    def test_missing_source_table_fails_before_any_write(self):
        (self.root / "docs/review/INDEX.md").unlink()
        before = self.read("README.md")
        code, out = self.run_main("--write")
        self.assertEqual(code, 1)
        self.assertEqual(self.read("README.md"), before)

    def test_mode_is_required(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                udc.main(["--root", str(self.root)])
        self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
