"""Unit tests for tools/check_orphan_tests.py.

Each test builds a small tree in a temporary directory, so the matching rules
are checked without depending on the current repository layout.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_orphan_tests.py"

_spec = importlib.util.spec_from_file_location("check_orphan_tests_under_test", TOOL)
orphans = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(orphans)


class _Tree:
    def __init__(self, files):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        for rel, text in files.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")

    def find(self):
        return orphans.find_orphans(self.root, "src")

    def run(self, *args):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = orphans.main(["--root", str(self.root), *args])
        return code, buf.getvalue()

    def close(self):
        self._tmp.cleanup()


def _with(files, fn):
    t = _Tree(files)
    try:
        return fn(t)
    finally:
        t.close()


class Discovery(unittest.TestCase):
    def test_test_like_names_are_recognised(self):
        self.assertTrue(orphans.is_test_source("port_test.c"))
        self.assertTrue(orphans.is_test_source("test_nt_types.c"))
        self.assertTrue(orphans.is_test_source("pe_tests.c"))
        self.assertFalse(orphans.is_test_source("testing_helpers.c"))
        self.assertFalse(orphans.is_test_source("port.c"))
        self.assertFalse(orphans.is_test_source("port_test.h"))

    def test_helper_sources_are_not_tests(self):
        def check(t):
            return t.find()
        found, total = _with({"src/a/helper.c": "int x;\n", "src/a/port.c": "int y;\n"}, check)
        self.assertEqual((found, total), ([], 0))

    def test_build_directories_are_skipped(self):
        def check(t):
            return t.find()
        found, total = _with({"src/build/x_test.c": "int x;\n"}, check)
        self.assertEqual((found, total), ([], 0))


class Matching(unittest.TestCase):
    def test_unreferenced_test_is_orphan(self):
        files = {"src/k/a_test.c": "int main(void){return 0;}\n", "Makefile": "all:\n\techo hi\n"}
        found, total = _with(files, lambda t: t.find())
        self.assertEqual(found, ["src/k/a_test.c"])
        self.assertEqual(total, 1)

    def test_makefile_reference_is_not_orphan(self):
        files = {
            "src/k/a_test.c": "int main(void){return 0;}\n",
            "tests/host/Makefile": "TESTS := a_test b_test\n",
        }
        found, total = _with(files, lambda t: t.find())
        self.assertEqual(found, [])
        self.assertEqual(total, 1)

    def test_comment_only_mention_does_not_count(self):
        files = {
            "src/k/a_test.c": "int main(void){return 0;}\n",
            "Makefile": "# a_test is not built yet\nall:\n\techo hi\n",
        }
        found, _ = _with(files, lambda t: t.find())
        self.assertEqual(found, ["src/k/a_test.c"])

    def test_trailing_comment_on_build_line_still_counts(self):
        files = {
            "src/k/a_test.c": "int main(void){return 0;}\n",
            "Makefile": "TESTS := a_test # built on Linux\n",
        }
        found, _ = _with(files, lambda t: t.find())
        self.assertEqual(found, [])

    def test_longer_name_does_not_match_shorter_test(self):
        files = {
            "src/k/port_test.c": "int main(void){return 0;}\n",
            "Makefile": "TESTS := port_test_extra\n",
        }
        found, _ = _with(files, lambda t: t.find())
        self.assertEqual(found, ["src/k/port_test.c"])

    def test_mk_fragment_counts_as_makefile(self):
        files = {
            "src/k/a_test.c": "int main(void){return 0;}\n",
            "mk/rules.mk": "TESTS += a_test\n",
        }
        found, _ = _with(files, lambda t: t.find())
        self.assertEqual(found, [])


class Report(unittest.TestCase):
    def test_orphan_listing_and_summary(self):
        files = {
            "src/k/a_test.c": "int main(void){return 0;}\n",
            "src/k/b_test.c": "int main(void){return 0;}\n",
            "tests/host/Makefile": "TESTS := a_test\n",
        }
        code, out = _with(files, lambda t: t.run())
        self.assertEqual(code, 1)
        self.assertIn("orphan src/k/b_test.c", out)
        self.assertNotIn("orphan src/k/a_test.c", out)
        self.assertIn("orphans: 1 of 2 test sources under src/", out)

    def test_no_orphans_exits_zero(self):
        files = {
            "src/k/a_test.c": "int main(void){return 0;}\n",
            "Makefile": "TESTS := a_test\n",
        }
        code, out = _with(files, lambda t: t.run())
        self.assertEqual(code, 0)
        self.assertIn("orphans: 0 of 1", out)

    def test_missing_scan_root_exits_two(self):
        files = {"Makefile": "all:\n"}
        buf = io.StringIO()
        def check(t):
            with contextlib.redirect_stderr(io.StringIO()):
                return t.run("--scan", "nope")
        code, _ = _with(files, check)
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
