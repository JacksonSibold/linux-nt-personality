"""Unit tests for src/kernel/nt/gen_ntstatus_table.py.

The script parses arguments and does its work at module level with no
__main__ guard, so every test runs it as a subprocess against temporary
userspace-table fixtures and inspects the generated .inc file.
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = (pathlib.Path(__file__).resolve().parents[2]
          / "src" / "kernel" / "nt" / "gen_ntstatus_table.py")


def table_source(entry_lines):
    """Build a minimal ntstatus_to_dos.c containing the given body lines."""
    body = "\n".join(entry_lines)
    return (
        "static const NTSTATUS_TO_DOS NtStatusToDosErrorTable[] = {\n"
        f"{body}\n"
        "};\n"
    )


def run_script(src_text, *, extra_args=(), with_output=True):
    """Run the generator. Returns (returncode, stdout, stderr, inc_text or None)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        src = tmp_path / "ntstatus_to_dos.c"
        dst = tmp_path / "ntstatus_table.inc"
        src.write_text(src_text, encoding="utf-8")
        args = [sys.executable, str(SCRIPT), "--input", str(src)]
        if with_output:
            args += ["--output", str(dst)]
        args += list(extra_args)
        proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
        inc = dst.read_text(encoding="utf-8") if dst.exists() else None
        return proc.returncode, proc.stdout, proc.stderr, inc


def generated_entries(inc_text):
    """Entry lines of the .inc, i.e. everything after the header comment."""
    return [ln for ln in inc_text.splitlines()
            if ln and not ln.startswith(("/*", " *", " */"))]


class GenNtstatusTableTests(unittest.TestCase):

    def test_copies_entries_and_reports_count(self):
        rc, stdout, _, inc = run_script(table_source([
            "    { STATUS_OK, 0L },",
            "    { STATUS_BAD, 5L },",
        ]))
        self.assertEqual(rc, 0)
        self.assertIn("Found 2 entries in userspace table", stdout)
        self.assertIn("Generated", stdout)
        self.assertIn(" * Entries: 2\n", inc)
        self.assertEqual(generated_entries(inc),
                         ["{ STATUS_OK, 0L },", "{ STATUS_BAD, 5L },"])

    def test_header_identifies_generated_file_and_source(self):
        _, _, _, inc = run_script(table_source(["    { STATUS_OK, 0L },"]))
        self.assertTrue(inc.startswith("/* AUTO-GENERATED - DO NOT EDIT\n"))
        self.assertIn("Source of truth: src/ntdll/ntstatus_to_dos.c", inc)

    def test_header_source_path_uses_forward_slashes(self):
        # The header is committed, so it must not depend on the host's
        # path separator (Windows would otherwise write backslashes).
        _, _, _, inc = run_script(table_source(["    { STATUS_OK, 0L },"]))
        generated_from = [ln for ln in inc.splitlines() if "Generated from" in ln]
        self.assertEqual(len(generated_from), 1)
        self.assertNotIn("\\", generated_from[0])

    def test_output_uses_lf_line_endings(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = pathlib.Path(tmp)
            src = tmp_path / "ntstatus_to_dos.c"
            dst = tmp_path / "ntstatus_table.inc"
            src.write_text(table_source(["    { STATUS_OK, 0L },"]), encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--input", str(src), "--output", str(dst)],
                capture_output=True)
            self.assertEqual(proc.returncode, 0)
            raw = dst.read_bytes()
            self.assertNotIn(b"\r", raw)

    def test_comments_and_blank_lines_are_dropped(self):
        rc, _, _, inc = run_script(table_source([
            "    /* a block comment */",
            "    // a line comment",
            "",
            "    { STATUS_OK, 0L },",
        ]))
        self.assertEqual(rc, 0)
        self.assertEqual(generated_entries(inc), ["{ STATUS_OK, 0L },"])

    def test_lines_without_status_or_hex_are_dropped(self):
        _, _, _, inc = run_script(table_source([
            "    { STATUS_OK, 0L },",
            "    some_other_text;",
        ]))
        self.assertEqual(generated_entries(inc), ["{ STATUS_OK, 0L },"])

    def test_no_count_check_without_expect(self):
        # The old hard-coded 527 check is gone: there is no evidence for that
        # number in the source. Without --expect, any count is accepted silently.
        rc, _, stderr, inc = run_script(table_source(["    { STATUS_OK, 0L },"]))
        self.assertEqual(rc, 0)
        self.assertNotIn("WARN", stderr)
        self.assertIn(" * Entries: 1\n", inc)

    def test_expect_match_succeeds(self):
        rc, _, _, inc = run_script(
            table_source(["    { STATUS_OK, 0L },", "    { STATUS_BAD, 5L },"]),
            extra_args=["--expect", "2"])
        self.assertEqual(rc, 0)
        self.assertIn(" * Entries: 2\n", inc)

    def test_expect_mismatch_fails_and_writes_nothing(self):
        rc, _, stderr, inc = run_script(
            table_source(["    { STATUS_OK, 0L },"]),
            extra_args=["--expect", "527"])
        self.assertEqual(rc, 1)
        self.assertIn("expected 527 entries, got 1", stderr)
        self.assertIsNone(inc)

    def test_missing_table_is_an_error_and_writes_nothing(self):
        rc, _, stderr, inc = run_script("int unrelated;\n")
        self.assertEqual(rc, 1)
        self.assertIn("Could not locate NtStatusToDosErrorTable", stderr)
        self.assertIsNone(inc)

    def test_entries_without_l_suffix_are_counted(self):
        rc, _, _, inc = run_script(table_source([
            "    { STATUS_OK, 0 },",
        ]))
        self.assertEqual(rc, 0)
        self.assertIn(" * Entries: 1\n", inc)
        self.assertEqual(generated_entries(inc), ["{ STATUS_OK, 0 },"])

    def test_double_l_suffix_is_counted(self):
        # The userspace source has `{ (NTSTATUS)0xc0000225L, 1168LL }`. The
        # old regex required a single L, so this entry was dropped from the count.
        rc, _, _, inc = run_script(table_source([
            "    { STATUS_OK, 0L },",
            "    { (NTSTATUS)0xc0000225L, 1168LL },",
        ]))
        self.assertEqual(rc, 0)
        self.assertIn(" * Entries: 2\n", inc)
        self.assertIn("{ (NTSTATUS)0xc0000225L, 1168LL },", generated_entries(inc))

    def test_cast_hex_status_entries_are_counted(self):
        rc, _, _, inc = run_script(table_source([
            "    { (NTSTATUS)0x80070005L, 5L },",
            "    { STATUS_OK, 0L },",
        ]))
        self.assertEqual(rc, 0)
        self.assertIn(" * Entries: 2\n", inc)

    def test_unparseable_entry_fails_and_writes_nothing(self):
        # A table line that starts with "{" but does not parse must not be
        # silently dropped, or the count and the output go out of step.
        rc, _, stderr, inc = run_script(table_source([
            "    { STATUS_OK, 0L },",
            "    { STATUS_BAD, SOME_MACRO },",
        ]))
        self.assertEqual(rc, 1)
        self.assertIn("cannot parse table entry", stderr)
        self.assertIn("STATUS_BAD, SOME_MACRO", stderr)
        self.assertIsNone(inc)

    def test_missing_required_arguments_fail(self):
        rc, _, _, _ = run_script(table_source(["    { STATUS_OK, 0L },"]),
                                 with_output=False)
        self.assertNotEqual(rc, 0)



if __name__ == "__main__":
    unittest.main()
