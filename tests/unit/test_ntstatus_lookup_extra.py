"""Branch, parsing and CLI tests for tools/ntstatus_lookup.py.

Most tests build a small synthetic repository (ntstatus.h and ntstatus_to_dos.c)
in a temporary directory and pass it with --repo. A few tests read the real
repository tables, which are the source of truth for the kernel copy.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "ntstatus_lookup.py"

_spec = importlib.util.spec_from_file_location("ntstatus_lookup", SCRIPT)
nl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nl)

HEADER = """\
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_WAIT_0 ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005L)
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#define STATUS_CUSTOM ((NTSTATUS)0xE0000001L)
#define STATUS_CUSTOMER_WIN32 ((NTSTATUS)0xD0070005L)
#define STATUS_WIN32_FAC ((NTSTATUS)0xC0070005L)
#define STATUS_OTHER_FAC ((NTSTATUS)0xC0010002L)
#define STATUS_PLAIN ((NTSTATUS)0xC0000001L)
#define STATUS_SEVERITY_ERROR 0x3
"""

TABLE = """\
static const NTSTATUS_TO_DOS NtStatusToDosErrorTable[] = {
    { STATUS_SUCCESS, 0L },
    { STATUS_WAIT_0, 9L },
    { STATUS_ACCESS_VIOLATION, 998L },
    { STATUS_BUFFER_OVERFLOW, 234L },
    { STATUS_NOT_DEFINED_HERE, 7L },
    { 0xC0000034L, 2L },
    { STATUS_NOT_FOUND, 1168LL },
};
"""


def quiet_call(fn, *args, **kwargs):
    """Call fn with stdout and stderr captured. Returns (result, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        result = fn(*args, **kwargs)
    return result, out.getvalue(), err.getvalue()


class SyntheticRepoCase(unittest.TestCase):
    header = HEADER
    table = TABLE

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        ntdll = self.root / "src" / "ntdll"
        ntdll.mkdir(parents=True)
        (ntdll / "ntstatus.h").write_text(self.header, encoding="utf-8")
        (ntdll / "ntstatus_to_dos.c").write_text(self.table, encoding="utf-8")
        self.header_path = str(ntdll / "ntstatus.h")
        self.source_path = str(ntdll / "ntstatus_to_dos.c")

    def tearDown(self):
        self._tmp.cleanup()

    def load(self):
        by_name, by_value = nl.load_defines(self.header_path)
        table = nl.load_table(self.source_path, by_name)
        return by_name, by_value, table

    def run_main(self, *args):
        return quiet_call(nl.main, [*args, "--repo", str(self.root)])


class ParsingTests(SyntheticRepoCase):
    def test_defines_map_names_and_values(self):
        by_name, by_value = nl.load_defines(self.header_path)
        self.assertEqual(by_name["STATUS_BUFFER_OVERFLOW"], 0x80000005)
        self.assertEqual(by_value[0], ["STATUS_SUCCESS", "STATUS_WAIT_0"])
        self.assertNotIn("STATUS_SEVERITY_ERROR", by_name)

    def test_first_definition_of_a_name_wins(self):
        self.header += "#define STATUS_SUCCESS ((NTSTATUS)0x12345678L)\n"
        pathlib.Path(self.header_path).write_text(self.header, encoding="utf-8")
        by_name, _ = nl.load_defines(self.header_path)
        self.assertEqual(by_name["STATUS_SUCCESS"], 0)

    def test_table_first_entry_for_a_value_wins(self):
        _, _, table = self.load()
        self.assertEqual(table[0], 0)          # STATUS_SUCCESS listed before STATUS_WAIT_0
        self.assertEqual(table[0xC0000005], 998)

    def test_table_skips_unknown_symbols(self):
        _, _, table = self.load()
        self.assertNotIn(7, table.values())

    def test_table_accepts_hex_literal_entries(self):
        _, _, table = self.load()
        self.assertEqual(table[0xC0000034], 2)

    def test_table_accepts_cast_and_long_long_entries(self):
        # The kernel generator accepts "(NTSTATUS)0x...L" names and "1168LL" values;
        # the userspace lookup must parse the same spellings or the DOS code is wrong.
        self.table = TABLE.replace(
            "};",
            "    { (NTSTATUS)0xC0000225L, 1168LL },\n"
            "    { (NTSTATUS)0x80070005L, 5L },\n"
            "};")
        (self.root / "src" / "ntdll" / "ntstatus_to_dos.c").write_text(self.table, encoding="utf-8")
        _, _, table = self.load()
        self.assertEqual(table[0xC0000225], 1168)
        self.assertEqual(table[0x80070005], 5)

    def test_missing_table_is_an_error(self):
        (self.root / "src" / "ntdll" / "ntstatus_to_dos.c").write_text("int x;\n", encoding="utf-8")
        with self.assertRaisesRegex(nl.LookupFailure, "NtStatusToDosErrorTable not found"):
            self.load()

    def test_table_with_no_parseable_entries_is_an_error(self):
        table = "static const NTSTATUS_TO_DOS NtStatusToDosErrorTable[] = {\n    { STATUS_NOPE, 1L },\n};\n"
        (self.root / "src" / "ntdll" / "ntstatus_to_dos.c").write_text(table, encoding="utf-8")
        with self.assertRaisesRegex(nl.LookupFailure, "no entries parsed"):
            self.load()

    def test_header_without_status_defines_is_an_error(self):
        (self.root / "src" / "ntdll" / "ntstatus.h").write_text("/* empty */\n", encoding="utf-8")
        with self.assertRaisesRegex(nl.LookupFailure, "no STATUS_ defines"):
            self.load()

    def test_unreadable_source_is_an_error(self):
        with self.assertRaisesRegex(nl.LookupFailure, "cannot read"):
            nl.read_text(str(self.root / "absent.h"))

    def test_parse_literal_hex_decimal_and_suffixes(self):
        self.assertEqual(nl.parse_literal("0xC0000005"), 0xC0000005)
        self.assertEqual(nl.parse_literal("0XFF"), 255)
        self.assertEqual(nl.parse_literal("42"), 42)
        with self.assertRaises(ValueError):
            nl.parse_literal("0x")


class FallbackTests(unittest.TestCase):
    """Mirror of RtlNtStatusToDosErrorFallback in src/ntdll/ntstatus_to_dos.c."""

    def test_zero_maps_to_zero(self):
        self.assertEqual(nl.fallback_dos(0), 0)

    def test_customer_bit_returns_status_itself(self):
        self.assertEqual(nl.fallback_dos(0xE0000001), 0xE0000001)

    def test_customer_alias_is_stripped_before_facility_check(self):
        self.assertEqual(nl.fallback_dos(0xD0070005), 5)

    def test_win32_facility_codes_return_low_word(self):
        self.assertEqual(nl.fallback_dos(0xC0070005), 5)
        self.assertEqual(nl.fallback_dos(0x80070005), 5)
        self.assertEqual(nl.fallback_dos(0xC0010002), 2)

    def test_other_codes_map_to_mr_mid_not_found(self):
        self.assertEqual(nl.fallback_dos(0xC0000225), nl.ERROR_MR_MID_NOT_FOUND)
        self.assertEqual(nl.fallback_dos(0x80000005), nl.ERROR_MR_MID_NOT_FOUND)

    def test_value_is_masked_to_32_bits(self):
        self.assertEqual(nl.fallback_dos(0x1_0000_0000 | 0xC0070005), 5)


class QueryTests(SyntheticRepoCase):
    def test_query_by_full_name_short_name_and_case(self):
        by_name, _, _ = self.load()
        self.assertEqual(nl.parse_query("STATUS_NOT_FOUND", by_name), (0xC0000225, "STATUS_NOT_FOUND"))
        self.assertEqual(nl.parse_query("not_found", by_name), (0xC0000225, "STATUS_NOT_FOUND"))
        self.assertEqual(nl.parse_query("  Status_Plain ", by_name), (0xC0000001, "STATUS_PLAIN"))

    def test_query_by_hex_and_decimal(self):
        by_name, _, _ = self.load()
        self.assertEqual(nl.parse_query("0xc0000005", by_name), (0xC0000005, None))
        self.assertEqual(nl.parse_query("3221225477", by_name), (0xC0000005, None))

    def test_invalid_query_raises(self):
        by_name, _, _ = self.load()
        with self.assertRaisesRegex(ValueError, "not a STATUS_ name or number"):
            nl.parse_query("NO_SUCH_THING", by_name)

    def test_describe_with_table_entry_and_name(self):
        _, by_value, table = self.load()
        text = nl.describe(0xC0000005, by_value, table)
        self.assertIn("0xC0000005  (3221225477)", text)
        self.assertIn("name:     STATUS_ACCESS_VIOLATION", text)
        self.assertIn("severity: ERROR  customer-code: no", text)
        self.assertIn("DOS:      998  (table)", text)

    def test_describe_without_name_or_table_uses_fallback(self):
        _, by_value, table = self.load()
        text = nl.describe(0xC0070009, by_value, table)     # no name in the fixture header
        self.assertIn("(no symbol in ntstatus.h)", text)
        self.assertIn("DOS:      9  (fallback rule)", text)

    def test_describe_severity_and_customer_bit(self):
        _, by_value, table = self.load()
        self.assertIn("severity: SUCCESS", nl.describe(0, by_value, table))
        self.assertIn("severity: INFORMATIONAL", nl.describe(0x40000001, by_value, table))
        self.assertIn("severity: WARNING", nl.describe(0x80000005, by_value, table))
        self.assertIn("customer-code: yes", nl.describe(0xE0000001, by_value, table))

    def test_search_is_case_insensitive_and_sorted(self):
        by_name, _, _ = self.load()
        hits = nl.search("buffer", by_name)
        self.assertEqual(hits, [("STATUS_BUFFER_OVERFLOW", 0x80000005)])
        names = [n for n, _ in nl.search("status_", by_name)]
        self.assertEqual(names, sorted(names))


class CliTests(SyntheticRepoCase):
    def test_no_arguments_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main()
        self.assertEqual(cm.exception.code, 2)

    def test_unknown_option_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(nl.main, ["--bogus"])
        self.assertEqual(cm.exception.code, 2)

    def test_query_success_exits_zero(self):
        rc, out, err = self.run_main("STATUS_NOT_FOUND")
        self.assertEqual(rc, 0)
        self.assertIn("DOS:      1168  (table)", out)
        self.assertEqual(err, "")

    def test_search_hits_exit_zero_and_print_names(self):
        rc, out, _ = self.run_main("--search", "OVERFLOW")
        self.assertEqual(rc, 0)
        self.assertIn("0x80000005  STATUS_BUFFER_OVERFLOW", out)

    def test_search_without_hits_exits_one(self):
        rc, out, err = self.run_main("--search", "NO_SUCH_NAME")
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("no names contain 'NO_SUCH_NAME'", err)

    def test_bad_query_reports_error_and_continues(self):
        rc, out, err = self.run_main("NOPE", "STATUS_PLAIN")
        self.assertEqual(rc, 1)
        self.assertIn("ntstatus_lookup: error: not a STATUS_ name or number", err)
        self.assertIn("0xC0000001", out)   # the valid query is still answered

    def test_several_queries_are_separated_by_blank_lines(self):
        rc, out, _ = self.run_main("STATUS_PLAIN", "STATUS_SUCCESS")
        self.assertEqual(rc, 0)
        self.assertIn("\n\n0x00000000", out)

    def test_search_output_is_followed_by_blank_line_before_query(self):
        rc, out, _ = self.run_main("--search", "SUCCESS", "STATUS_PLAIN")
        self.assertEqual(rc, 0)
        self.assertIn("STATUS_SUCCESS", out)
        self.assertIn("\n\n0xC0000001", out)

    def test_missing_source_files_exit_one(self):
        empty = tempfile.mkdtemp()
        try:
            rc, _out, err = quiet_call(nl.main, ["STATUS_SUCCESS", "--repo", empty])
        finally:
            pathlib.Path(empty).rmdir()
        self.assertEqual(rc, 1)
        self.assertIn("ntstatus_lookup: error: cannot read", err)

    def test_script_exit_status_through_interpreter(self):
        import subprocess
        import sys
        usage = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(usage.returncode, 2)
        self.assertIn("usage:", usage.stderr)
        real = subprocess.run([sys.executable, str(SCRIPT), "STATUS_BUFFER_OVERFLOW"],
                              capture_output=True, text=True)
        self.assertEqual(real.returncode, 0, real.stderr)


class RealRepositoryTests(unittest.TestCase):
    """The tracked tables. These must parse without dropping any entry."""

    def test_real_query_exits_zero(self):
        rc, out, _ = quiet_call(nl.main, ["STATUS_BUFFER_OVERFLOW", "0xC0000005", "--repo", str(REPO)])
        self.assertEqual(rc, 0)
        self.assertIn("name:     STATUS_ACCESS_VIOLATION", out)

    def test_real_table_keeps_cast_and_suffixed_entries(self):
        by_name, _ = nl.load_defines(str(REPO / "src" / "ntdll" / "ntstatus.h"))
        table = nl.load_table(str(REPO / "src" / "ntdll" / "ntstatus_to_dos.c"), by_name)
        self.assertEqual(table[0xC0000225], 1168)
        self.assertEqual(table[0x80070005], 5)


if __name__ == "__main__":
    unittest.main()
