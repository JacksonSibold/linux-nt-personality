"""Unit tests for build/fix_iosb.py.

The script is importable (its work happens in main(), behind a __main__
guard), so the module is loaded by path. Its rewrite tables are applied
directly to small C snippets, and main() is run against a temporary copy of
the target file by temporarily overriding the module-level PATH.
"""
import contextlib
import importlib.util
import io
import pathlib
import re
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "fix_iosb.py"

_spec = importlib.util.spec_from_file_location("fix_iosb", SCRIPT)
fix_iosb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fix_iosb)


def apply_table(table, text):
    for pattern, replacement in table:
        text, _ = re.subn(pattern, replacement, text)
    return text


def run_main_on(source_text):
    """Run fix_iosb.main() on a temp copy of source_text. Returns (rc, stdout, new_text)."""
    with tempfile.TemporaryDirectory() as tmp:
        target = pathlib.Path(tmp) / "nt_io.c"
        target.write_text(source_text, encoding="utf-8")
        original = fix_iosb.PATH
        fix_iosb.PATH = str(target)
        try:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = fix_iosb.main()
        finally:
            fix_iosb.PATH = original
        return rc, buf.getvalue(), target.read_text(encoding="utf-8")


class ReadRewriteTests(unittest.TestCase):

    def test_read_of_information_uses_getter(self):
        out = apply_table(fix_iosb.READS, "x = iosb->Information;")
        self.assertEqual(out, "x = ntio_get_iosb_info(iosb);")

    def test_read_through_ioStatusBlock_name(self):
        out = apply_table(fix_iosb.READS, "y = IoStatusBlock->Information + 1;")
        self.assertEqual(out, "y = ntio_get_iosb_info(IoStatusBlock) + 1;")

    def test_assignment_is_not_treated_as_read(self):
        text = "iosb->Information = 5;"
        self.assertEqual(apply_table(fix_iosb.READS, text), text)

    def test_unrelated_struct_field_is_not_touched(self):
        text = "k.Information = info; other->Information;"
        self.assertEqual(apply_table(fix_iosb.READS, text), text)


class PairedWriteTests(unittest.TestCase):

    def test_information_then_status_collapses(self):
        src = "\tiosb->Information = len;\n\tiosb->DUMMYUNIONNAME.Status = st;"
        out = apply_table(fix_iosb.PAIRS, src)
        self.assertEqual(out, "\tntio_put_iosb(iosb, st, (ULONG_PTR)(len));")

    def test_status_then_information_collapses(self):
        src = "\tIoStatusBlock->DUMMYUNIONNAME.Status = st;\n\tIoStatusBlock->Information = n;"
        out = apply_table(fix_iosb.PAIRS, src)
        self.assertEqual(out, "\tntio_put_iosb(IoStatusBlock, st, (ULONG_PTR)(n));")

    def test_unpaired_writes_are_left_for_singles(self):
        src = "\tiosb->Information = len;\n\tfoo();"
        self.assertEqual(apply_table(fix_iosb.PAIRS, src), src)


class SingleWriteTests(unittest.TestCase):

    def test_lone_status_write_keeps_existing_information(self):
        out = apply_table(fix_iosb.SINGLES, "\tiosb->DUMMYUNIONNAME.Status = st;")
        self.assertEqual(out, "\tntio_put_iosb(iosb, st, ntio_get_iosb_info(iosb));")

    def test_lone_information_write_uses_success_status(self):
        out = apply_table(fix_iosb.SINGLES, "\tiosb->Information = n;")
        self.assertEqual(out, "\tntio_put_iosb(iosb, STATUS_SUCCESS, (ULONG_PTR)(n));")


class MainTests(unittest.TestCase):

    SOURCE = (
        "#include <linux/uaccess.h>\n"
        "\n"
        "static NTSTATUS f(PIO_STATUS_BLOCK iosb, ULONG len)\n"
        "{\n"
        "\tiosb->Information = len;\n"
        "\tiosb->DUMMYUNIONNAME.Status = STATUS_SUCCESS;\n"
        "\treturn STATUS_SUCCESS;\n"
        "}\n"
    )

    def test_main_returns_zero_and_reports_counts(self):
        rc, stdout, _ = run_main_on(self.SOURCE)
        self.assertEqual(rc, 0)
        self.assertIn("direct iosb accesses before: 2", stdout)
        self.assertIn("written", stdout)

    def test_helpers_inserted_after_last_include(self):
        _, _, new = run_main_on(self.SOURCE)
        include_end = new.index("#include <linux/uaccess.h>\n") + len("#include <linux/uaccess.h>\n")
        helper_pos = new.index("static inline NTSTATUS ntio_put_iosb")
        self.assertGreaterEqual(helper_pos, include_end)
        self.assertLess(helper_pos, new.index("static NTSTATUS f("))

    def test_function_body_uses_accessors(self):
        _, _, new = run_main_on(self.SOURCE)
        body = new[new.index("static NTSTATUS f("):]
        self.assertIn("\tntio_put_iosb(iosb, STATUS_SUCCESS, (ULONG_PTR)(len));", body)
        self.assertNotIn("iosb->", body)

    def test_no_direct_iosb_access_outside_helpers_after_run(self):
        _, _, new = run_main_on(self.SOURCE)
        body = new[new.index("static NTSTATUS f("):]
        self.assertEqual(re.findall(r"(iosb|IoStatusBlock)->", body), [])

    def test_second_run_does_not_reinsert_helpers(self):
        _, _, once = run_main_on(self.SOURCE)
        _, stdout, twice = run_main_on(once)
        self.assertEqual(twice.count("static inline NTSTATUS ntio_put_iosb"), 1)
        self.assertNotIn("inserted accessors", stdout)

    def test_helper_non_kernel_branch_is_not_self_recursive(self):
        _, _, new = run_main_on(self.SOURCE)
        helper = new[new.index("static inline NTSTATUS ntio_put_iosb"):
                     new.index("static NTSTATUS f(")]
        start = helper.index("#else")
        else_branch = helper[start:helper.index("#endif", start)]
        self.assertNotIn("ntio_put_iosb(", else_branch)


if __name__ == "__main__":
    unittest.main()
