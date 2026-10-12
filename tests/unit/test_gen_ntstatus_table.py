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

EXPECTED_ENTRY_COUNT = 527


def table_source(entry_lines):
    """Build a minimal ntstatus_to_dos.c containing the given body lines."""
    body = "\n".join(entry_lines)
    return (
        "static const NTSTATUS_TO_DOS NtStatusToDosErrorTable[] = {\n"
        f"{body}\n"
        "};\n"
    )


def run_script(src_text, *, with_output=True):
    """Run the generator. Returns (returncode, stdout, stderr, inc_text or None)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        src = tmp_path / "ntstatus_to_dos.c"
        dst = tmp_path / "ntstatus_table.inc"
        src.write_text(src_text, encoding="utf-8")
        args = [sys.executable, str(SCRIPT), "--input", str(src)]
        if with_output:
            args += ["--output", str(dst)]
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

    def test_warns_but_succeeds_when_count_is_not_527(self):
        rc, _, stderr, inc = run_script(table_source(["    { STATUS_OK, 0L },"]))
        self.assertEqual(rc, 0)
        self.assertIn("WARN: Expected 527 entries, got 1", stderr)
        self.assertIsNotNone(inc)

    def test_no_warning_at_expected_count(self):
        entries = [f"    {{ STATUS_X{i}, {i}L }}," for i in range(EXPECTED_ENTRY_COUNT)]
        rc, _, stderr, inc = run_script(table_source(entries))
        self.assertEqual(rc, 0)
        self.assertNotIn("WARN", stderr)
        self.assertIn(f" * Entries: {EXPECTED_ENTRY_COUNT}\n", inc)

    def test_missing_table_is_an_error_and_writes_nothing(self):
        rc, _, stderr, inc = run_script("int unrelated;\n")
        self.assertEqual(rc, 1)
        self.assertIn("Could not locate NtStatusToDosErrorTable", stderr)
        self.assertIsNone(inc)

    def test_entries_without_l_suffix_are_not_counted(self):
        # Known limitation: the entry pattern requires the "L" suffix on the
        # value, so an entry written without it is silently dropped from the
        # count and output. Recorded here so the behaviour is explicit.
        rc, _, stderr, inc = run_script(table_source([
            "    { STATUS_OK, 0 },",
        ]))
        self.assertEqual(rc, 0)
        self.assertIn(" * Entries: 0\n", inc)
        self.assertIn("WARN: Expected 527 entries, got 0", stderr)

    def test_missing_required_arguments_fail(self):
        rc, _, _, _ = run_script(table_source(["    { STATUS_OK, 0L },"]),
                                 with_output=False)
        self.assertNotEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
