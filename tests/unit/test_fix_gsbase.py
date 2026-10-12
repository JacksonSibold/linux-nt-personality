"""Unit tests for build/fix_gsbase.py.

The script is importable (work happens in main(), behind a __main__ guard), so
it is loaded by path. main() is run against a temporary copy of the target by
overriding the module-level PATH.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "fix_gsbase.py"

_spec = importlib.util.spec_from_file_location("fix_gsbase", SCRIPT)
fix_gsbase = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fix_gsbase)


def run_main_on(source_text):
    """Run main() on a temp copy of source_text. Returns (rc, stdout, stderr, new_text)."""
    with tempfile.TemporaryDirectory() as tmp:
        target = pathlib.Path(tmp) / "binfmt_pe.c"
        target.write_text(source_text, encoding="utf-8")
        original = fix_gsbase.PATH
        fix_gsbase.PATH = str(target)
        out, err = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = fix_gsbase.main()
        finally:
            fix_gsbase.PATH = original
        return rc, out.getvalue(), err.getvalue(), target.read_text(encoding="utf-8")


class ExactMatchTests(unittest.TestCase):

    SOURCE = "static int load(void)\n{\n" + fix_gsbase.OLD + "\n\treturn 0;\n}\n"

    def test_exact_match_is_replaced_with_new_block(self):
        rc, stdout, _, new = run_main_on(self.SOURCE)
        self.assertEqual(rc, 0)
        self.assertIn("patched via exact match", stdout)
        self.assertIn(fix_gsbase.NEW, new)
        self.assertNotIn("set_user_gsbase", new)

    def test_new_block_installs_base_via_msr_write(self):
        _, _, _, new = run_main_on(self.SOURCE)
        self.assertIn("current->thread.gsindex = 0;", new)
        self.assertIn("x86_gsbase_write_cpu_inactive((unsigned long)state.teb);", new)

    def test_new_block_validates_teb_against_task_size(self):
        _, _, _, new = run_main_on(self.SOURCE)
        self.assertIn("if ((unsigned long)state.teb < TASK_SIZE_MAX) {", new)
        self.assertIn("ret = -EINVAL;", new)
        self.assertIn("goto out;", new)

    def test_surrounding_code_is_preserved(self):
        _, _, _, new = run_main_on(self.SOURCE)
        self.assertTrue(new.startswith("static int load(void)\n{\n"))
        self.assertTrue(new.rstrip().endswith("}"))
        self.assertIn("\treturn 0;\n}\n", new)


class FallbackTests(unittest.TestCase):

    def test_fallback_regex_replaces_drifted_call_site(self):
        src = "void f(void)\n{\n\tset_user_gsbase(current,   (unsigned long)state.teb);\n}\n"
        rc, stdout, _, new = run_main_on(src)
        self.assertEqual(rc, 0)
        self.assertIn("patched via fallback regex", stdout)
        self.assertNotIn("set_user_gsbase", new)
        self.assertIn("x86_gsbase_write_cpu_inactive((unsigned long)state.teb);", new)

    @unittest.expectedFailure
    def test_fallback_keeps_teb_bounds_check(self):
        # Known defect: the exact-match replacement refuses a TEB at or above
        # TASK_SIZE_MAX, but the fallback regex installs the base unchecked.
        # The script's own comments call an unchecked GS base a privilege
        # escalation primitive. Marked expectedFailure until the fallback is
        # given the same guard.
        src = "void f(void)\n{\n\tset_user_gsbase(current, (unsigned long)state.teb);\n}\n"
        _, _, _, new = run_main_on(src)
        self.assertIn("TASK_SIZE_MAX", new)


class NoOpAndErrorTests(unittest.TestCase):

    def test_already_patched_source_is_left_alone(self):
        src = "void f(void)\n{\n\tx86_gsbase_write_cpu_inactive(0);\n}\n"
        rc, stdout, _, new = run_main_on(src)
        self.assertEqual(rc, 0)
        self.assertIn("set_user_gsbase already gone; nothing to do", stdout)
        self.assertEqual(new, src)

    def test_unlocatable_call_site_fails_without_writing(self):
        # The name appears, but not as a call the fallback pattern can match
        # (no parenthesis), so the script must fail and leave the file alone.
        src = "/* set_user_gsbase is mentioned here */\nint x;\n"
        rc, _, stderr, new = run_main_on(src)
        self.assertEqual(rc, 1)
        self.assertIn("could not locate the call site", stderr)
        self.assertEqual(new, src)


if __name__ == "__main__":
    unittest.main()
