"""Unit tests for tools/coverage_report.py.

The full run (unit tests under the tracer) is not exercised here, because it
would run this file again. The measurement itself is checked by --selftest and
by the helpers below.
"""
import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "coverage_report.py"

_spec = importlib.util.spec_from_file_location("coverage_report", SCRIPT)
coverage_report = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(coverage_report)


def quiet(fn, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return fn(*args, **kwargs)


class TargetFilesTests(unittest.TestCase):
    def test_covers_build_tools_and_kernel_generator(self):
        names = [p.relative_to(REPO).as_posix() for p in coverage_report.target_files()]
        self.assertIn("src/kernel/nt/gen_ntstatus_table.py", names)
        self.assertIn("tools/pe_info.py", names)
        self.assertIn("build/merge_dod.py", names)
        self.assertTrue(all(n.startswith(("build/", "tools/")) or n.endswith("gen_ntstatus_table.py")
                            for n in names))
        self.assertEqual(len(names), len(set(names)))

    def test_every_target_exists(self):
        self.assertTrue(all(p.is_file() for p in coverage_report.target_files()))


class ExecutableLinesTests(unittest.TestCase):
    def _lines(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "m.py"
            path.write_text(source, encoding="utf-8")
            return coverage_report.executable_lines(path)

    def test_docstrings_are_not_statements(self):
        lines = self._lines('"""doc\nmore"""\n\nX = 1\n')
        self.assertEqual(lines, {4})

    def test_bare_string_after_code_is_a_statement(self):
        # Only a leading string in a body is a docstring; later strings count.
        lines = self._lines("print('x')\n'not a docstring'\n")
        self.assertEqual(lines, {1, 2})

    def test_function_docstring_skipped(self):
        lines = self._lines('def f():\n    """doc"""\n    return 1\n')
        self.assertEqual(lines, {1, 3})

    def test_compound_statements_and_bodies_counted(self):
        lines = self._lines("def f():\n    if True:\n        return 1\n")
        self.assertEqual(lines, {1, 2, 3})


class RenderTests(unittest.TestCase):
    def test_zero_executable_lines_shows_na(self):
        text = coverage_report.render([("tools/x.py", 0, 0)], repo=REPO)
        self.assertIn("n/a", text)
        self.assertIn("TOTAL", text)

    def test_totals_and_percent(self):
        rows = [("tools/a.py", 4, 2), ("tools/b.py", 4, 4)]
        text = coverage_report.render(rows, repo=REPO)
        total = [ln for ln in text.splitlines() if ln.startswith("TOTAL")][0]
        self.assertIn(" 8 ", total)
        self.assertIn("75.0%", total)

    def test_path_outside_repo_keeps_full_name(self):
        name = coverage_report.display_name(pathlib.Path("/elsewhere/x.py"), repo=REPO)
        self.assertTrue(name.endswith("x.py"))


class MeasureTests(unittest.TestCase):
    def test_nested_measure_restores_outer_tracer(self):
        # The unit run is itself traced, and this file's selftest nests measure().
        # Measure must hand back whatever tracer was installed before it.
        previous = sys.gettrace()

        def outer(frame, event, arg):
            return None

        sys.settrace(outer)
        try:
            coverage_report.measure(lambda: None, [])
            self.assertIs(sys.gettrace(), outer)
        finally:
            sys.settrace(previous)


class CliTests(unittest.TestCase):
    def test_selftest_exits_zero(self):
        self.assertEqual(quiet(coverage_report.main, ["--selftest"]), 0)

    def test_unknown_argument_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet(coverage_report.main, ["--no-such-option"])
        self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
