"""Branch and CLI tests for tools/check_repo.py.

Each test builds a small tree in a temporary directory and runs the checks on
it with --root, so the real repository is never judged by these tests. The
file-listing test uses a throwaway git repository when git is available.
"""
import contextlib
import importlib.util
import io
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_repo.py"

_spec = importlib.util.spec_from_file_location("check_repo", SCRIPT)
cr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cr)

HAVE_GIT = shutil.which("git") is not None


def quiet_call(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        result = fn(*args, **kwargs)
    return result, out.getvalue(), err.getvalue()


class TreeCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, data):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            data = data.encode("utf-8")
        path.write_bytes(data)
        return path

    def check(self, *rels):
        """Run check_file on the given relative paths. Returns (failures, text_count)."""
        failures = []
        text = 0
        for rel in rels:
            if cr.check_file(str(self.root), rel, failures):
                text += 1
        return failures, text

    def run_checks(self, quiet=False):
        return quiet_call(cr.run, str(self.root), quiet)


class BinaryAndSizeTests(TreeCase):
    def test_small_binary_is_not_read_as_text(self):
        self.write("blob.bin", b"\x00\x01" * 100)
        failures, text = self.check("blob.bin")
        self.assertEqual(failures, [])
        self.assertEqual(text, 0)

    def test_binary_over_limit_is_reported(self):
        self.write("big.bin", b"\x00" + b"x" * cr.MAX_BINARY_BYTES)
        failures, _ = self.check("big.bin")
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0][0], "binary-size")
        self.assertIn(str(cr.MAX_BINARY_BYTES), failures[0][2])

    def test_binary_exactly_at_limit_passes(self):
        self.write("edge.bin", b"\x00" + b"x" * (cr.MAX_BINARY_BYTES - 1))
        failures, _ = self.check("edge.bin")
        self.assertEqual(failures, [])

    def test_is_binary_looks_for_nul(self):
        self.assertTrue(cr.is_binary(b"ab\x00cd"))
        self.assertFalse(cr.is_binary(b"abcd"))


class PerFileTests(TreeCase):
    def test_clean_python_file_passes(self):
        self.write("ok.py", "x = 1\n")
        failures, text = self.check("ok.py")
        self.assertEqual((failures, text), ([], 1))

    def test_python_syntax_error_names_line(self):
        self.write("bad.py", "x = 1\ndef broken(:\n")
        failures, _ = self.check("bad.py")
        self.assertEqual(failures[0][0], "py-compile")
        self.assertTrue(failures[0][2].startswith("line 2:"), failures)

    def test_nul_byte_past_probe_is_a_compile_failure_without_line_none(self):
        # A NUL after the 8 KiB binary probe makes the file look like text. It
        # must be reported as a compile failure with no "line None" in the message.
        self.write("late_nul.py", b"x = 1\n" + b"#" * (cr.BINARY_PROBE_BYTES + 100) + b"\x00\n")
        failures, _ = self.check("late_nul.py")
        self.assertEqual(len(failures), 1, failures)
        self.assertEqual(failures[0][0], "py-compile")
        self.assertNotIn("None", failures[0][2])
        self.assertIn("null bytes", failures[0][2])

    def test_crlf_mixed_with_bare_lf(self):
        self.write("mixed.txt", b"one\r\ntwo\nthree\r\n")
        failures, _ = self.check("mixed.txt")
        self.assertEqual(failures, [("crlf-mix", "mixed.txt", "2 CRLF lines and 1 bare-LF lines")])

    def test_pure_crlf_and_pure_lf_pass(self):
        self.write("crlf.txt", b"a\r\nb\r\n")
        self.write("lf.txt", b"a\nb\n")
        failures, text = self.check("crlf.txt", "lf.txt")
        self.assertEqual((failures, text), ([], 2))

    def test_empty_file_is_text_and_clean(self):
        self.write("empty.txt", b"")
        self.assertEqual(self.check("empty.txt"), ([], 1))


class HeaderGuardTests(TreeCase):
    def guard_failures(self, files):
        failures = []
        cr.check_header_guards(str(self.root), sorted(files), failures)
        return failures

    def test_guard_pair_accepted(self):
        self.write("a.h", "#ifndef A_H\n#define A_H\n#endif\n")
        self.write("a.c", "int x;\n")
        self.assertEqual(self.guard_failures(["a.c", "a.h"]), [])

    def test_pragma_once_accepted(self):
        self.write("a.h", "  #  pragma once\n")
        self.write("a.c", "int x;\n")
        self.assertEqual(self.guard_failures(["a.c", "a.h"]), [])

    def test_missing_guard_reported_against_header(self):
        self.write("a.h", "int y;\n")
        self.write("a.c", "int x;\n")
        failures = self.guard_failures(["a.c", "a.h"])
        self.assertEqual(failures, [("header-guard", "a.h", "no include guard (needed by a.c)")])

    def test_mismatched_define_is_not_a_guard(self):
        self.write("a.h", "#ifndef A_H\n#define B_H\n")
        self.write("a.c", "int x;\n")
        self.assertEqual(len(self.guard_failures(["a.c", "a.h"])), 1)

    def test_c_file_without_header_is_not_checked(self):
        self.write("solo.c", "int x;\n")
        self.assertEqual(self.guard_failures(["solo.c"]), [])

    def test_non_c_files_are_ignored(self):
        self.write("a.h", "int y;\n")
        self.assertEqual(self.guard_failures(["a.h"]), [])


class ListFilesTests(TreeCase):
    def test_walk_skips_git_and_pycache(self):
        self.write("keep/x.txt", "x")
        self.write(".git/config", "x")
        self.write("__pycache__/m.pyc", "x")
        names = cr.list_files(str(self.root))
        self.assertEqual(names, ["keep/x.txt"])

    @unittest.skipUnless(HAVE_GIT, "git not installed")
    def test_git_work_tree_lists_tracked_files_only(self):
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        self.write("tracked.txt", "t")
        self.write("untracked.txt", "u")
        subprocess.run(["git", "-C", str(self.root), "add", "tracked.txt"], check=True)
        self.assertEqual(cr.list_files(str(self.root)), ["tracked.txt"])


class RunAndMainTests(TreeCase):
    def test_clean_tree_summary(self):
        self.write("ok.txt", "fine\n")
        rc, out, _ = self.run_checks()
        self.assertEqual(rc, 0)
        self.assertEqual(out, "checked 1 files (1 text), 0 failure(s)\n")

    def test_failures_are_listed_and_return_one(self):
        self.write("mixed.txt", b"a\r\nb\n")
        rc, out, _ = self.run_checks()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL [crlf-mix] mixed.txt:", out)
        self.assertIn("1 failure(s)", out)

    def test_quiet_prints_only_summary(self):
        self.write("mixed.txt", b"a\r\nb\n")
        rc, out, _ = self.run_checks(quiet=True)
        self.assertEqual(rc, 1)
        self.assertEqual(out.count("\n"), 1)
        self.assertNotIn("FAIL", out)

    def test_main_missing_root_is_usage_error(self):
        rc, _out, err = quiet_call(cr.main, ["--root", str(self.root / "absent")])
        self.assertEqual(rc, 2)
        self.assertIn("not a directory", err)

    def test_main_quiet_flag_and_root(self):
        self.write("ok.txt", "fine\n")
        rc, out, _ = quiet_call(cr.main, ["--root", str(self.root), "--quiet"])
        self.assertEqual(rc, 0)
        self.assertIn("checked 1 files", out)

    def test_main_unknown_option_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(cr.main, ["--no-such-flag"])
        self.assertEqual(cm.exception.code, 2)

    def test_script_exit_status_through_interpreter(self):
        self.write("ok.txt", "fine\n")
        ok = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), "--quiet"],
                            capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.write("bad.py", "def broken(:\n")
        bad = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root)],
                             capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)
        self.assertIn("FAIL [py-compile] bad.py", bad.stdout)
        missing = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root / "nope")],
                                 capture_output=True, text=True)
        self.assertEqual(missing.returncode, 2)


if __name__ == "__main__":
    unittest.main()
