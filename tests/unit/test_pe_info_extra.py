"""Branch and CLI tests for tools/pe_info.py.

The script's own --selftest covers the normal parse path. These tests drive the
remaining branches: malformed import tables, header limits, formatting of edge
values, and the command-line exit codes.
"""
import contextlib
import importlib.util
import io
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "pe_info.py"

_spec = importlib.util.spec_from_file_location("pe_info", SCRIPT)
pe_info = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pe_info)

E_LFANEW_OFF = 0x3C
PE32P_IMPORT_DIR_OFF = 0x40 + 4 + 20 + 112 + 8     # DataDirectory[1].VirtualAddress
PE32P_NUM_DIRS_OFF = 0x40 + 4 + 20 + 108           # NumberOfRvaAndSizes
PE32P_NSEC_OFF = 0x40 + 4 + 2                       # COFF NumberOfSections
PE32P_SECTIONS_OFF = 0x40 + 4 + 20 + 240            # first section header


def quiet_call(fn, *args, **kwargs):
    """Call fn with stdout and stderr captured. Returns (result, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        result = fn(*args, **kwargs)
    return result, out.getvalue(), err.getvalue()


def patched(data, offset, fmt, *values):
    buf = bytearray(data)
    struct.pack_into("<" + fmt, buf, offset, *values)
    return bytes(buf)


class UnpackAndStringTests(unittest.TestCase):
    def test_unpack_reads_little_endian(self):
        self.assertEqual(pe_info.unpack(b"\x01\x00\x00\x00", 0, "I"), (1,))

    def test_unpack_rejects_negative_offset(self):
        with self.assertRaisesRegex(pe_info.PEError, "truncated"):
            pe_info.unpack(b"\x00" * 8, -1, "I")

    def test_unpack_rejects_read_past_end(self):
        with self.assertRaisesRegex(pe_info.PEError, "truncated"):
            pe_info.unpack(b"\x00" * 3, 0, "I")

    def test_read_cstring_stops_at_nul(self):
        self.assertEqual(pe_info.read_cstring(b"xxKERNEL32.dll\x00yy", 2), "KERNEL32.dll")

    def test_read_cstring_unterminated_within_limit(self):
        with self.assertRaisesRegex(pe_info.PEError, "unterminated string at offset 0x0"):
            pe_info.read_cstring(b"abcdef", 0, limit=4)

    def test_read_cstring_replaces_non_ascii(self):
        self.assertEqual(pe_info.read_cstring(b"a\xffb\x00", 0), "a�b")

    def test_flags_text_empty_and_multiple(self):
        table = [(0x1, "A"), (0x2, "B"), (0x4, "C")]
        self.assertEqual(pe_info.flags_text(0, table), "-")
        self.assertEqual(pe_info.flags_text(0x5, table), "A C")


class RvaToOffsetTests(unittest.TestCase):
    SECTIONS = [{"va": 0x1000, "vsize": 0x400, "rawsize": 0x200, "rawptr": 0x200}]

    def test_inside_raw_data(self):
        self.assertEqual(pe_info.rva_to_offset(self.SECTIONS, 0x1010), 0x210)

    def test_virtual_only_tail_has_no_file_offset(self):
        self.assertIsNone(pe_info.rva_to_offset(self.SECTIONS, 0x1300))

    def test_before_and_after_sections(self):
        self.assertIsNone(pe_info.rva_to_offset(self.SECTIONS, 0x0FFF))
        self.assertIsNone(pe_info.rva_to_offset(self.SECTIONS, 0x2000))

    def test_zero_vsize_uses_raw_size(self):
        sections = [{"va": 0x1000, "vsize": 0, "rawsize": 0x100, "rawptr": 0x400}]
        self.assertEqual(pe_info.rva_to_offset(sections, 0x1080), 0x480)
        self.assertIsNone(pe_info.rva_to_offset(sections, 0x1100))


class FixtureMutationTests(unittest.TestCase):
    """Malformed PE32+ and PE32 files built from the module's own fixture."""

    def setUp(self):
        self.good = pe_info.build_fixture()

    def assert_import_note(self, data, needle):
        info = pe_info.parse_pe(data)
        self.assertIsNone(info["imports"])
        self.assertIn(needle, info["import_note"])

    def test_import_directory_outside_file_backed_data(self):
        data = patched(self.good, PE32P_IMPORT_DIR_OFF, "I", 0x9000)
        self.assert_import_note(data, "import directory RVA 0x9000")

    def test_import_name_outside_file_backed_data(self):
        # Descriptor 0 holds the name RVA at offset 12 of the import directory.
        data = bytearray(self.good)
        struct.pack_into("<I", data, 0x400 + 12, 0x9000)
        self.assert_import_note(bytes(data), "import name RVA 0x9000")

    def test_thunk_rva_zero_and_unmapped(self):
        # OriginalFirstThunk and FirstThunk both zero: no thunk table to read.
        data = bytearray(self.good)
        struct.pack_into("<II", data, 0x400, 0, 0)
        struct.pack_into("<I", data, 0x400 + 16, 0)
        self.assert_import_note(bytes(data), "thunk RVA for KERNEL32.dll")

    def test_import_descriptor_truncated(self):
        # Cut the file inside the first import descriptor.
        self.assert_import_note(self.good[:0x410], "truncated file")

    def test_implausibly_long_thunk_table(self):
        with mock.patch.object(pe_info, "MAX_THUNKS_PER_DLL", 2):
            self.assert_import_note(self.good, "implausibly long")

    def test_too_many_import_descriptors(self):
        with mock.patch.object(pe_info, "MAX_IMPORT_DESCRIPTORS", 1):
            self.assert_import_note(self.good, "too many import descriptors")

    def test_section_count_over_loader_limit(self):
        data = patched(self.good, PE32P_NSEC_OFF, "H", pe_info.MAX_SECTIONS + 1)
        with self.assertRaisesRegex(pe_info.PEError, "implausible section count 97"):
            pe_info.parse_pe(data)

    def test_section_table_truncated(self):
        with self.assertRaisesRegex(pe_info.PEError, "truncated"):
            pe_info.parse_pe(self.good[:PE32P_SECTIONS_OFF + 40 + 10])

    def test_zero_import_rva_means_no_imports(self):
        info = pe_info.parse_pe(patched(self.good, PE32P_IMPORT_DIR_OFF, "I", 0))
        self.assertEqual(info["imports"], [])
        self.assertIsNone(info["import_note"])

    def test_no_import_directory_entry_when_few_dirs(self):
        for count in (0, 1):
            info = pe_info.parse_pe(patched(self.good, PE32P_NUM_DIRS_OFF, "I", count))
            self.assertEqual(info["imports"], [], f"NumberOfRvaAndSizes={count}")

    def test_pe32_import_errors_use_four_byte_thunks(self):
        data = pe_info.build_fixture(pe32=True)
        info = pe_info.parse_pe(data)
        self.assertEqual(info["imports"], [("KERNEL32.dll", 2)])
        broken = patched(data, 0x58 + 96 + 8, "I", 0x9000)  # PE32 DataDirectory[1]
        self.assert_import_note(broken, "import directory RVA 0x9000")

    def test_entry_point_and_subsystem_fields(self):
        info = pe_info.parse_pe(self.good)
        self.assertEqual(info["entry_rva"], 0x1000)
        self.assertEqual(info["size_of_image"], 0x3000)
        self.assertEqual(info["image_base"], 0x140000000)
        self.assertEqual(info["subsystem_id"], 3)


class FormatInfoTests(unittest.TestCase):
    def _info(self, **overrides):
        info = {
            "format": "PE32+", "machine": "x86-64 (AMD64)", "machine_id": 0x8664,
            "timestamp": 0, "characteristics": 0x0022, "subsystem": "windows console",
            "subsystem_id": 3, "image_base": 0x400000, "entry_rva": 0x1000,
            "size_of_image": 0x3000, "dll_flags": 0, "sections": [],
            "imports": [], "import_note": None,
        }
        info.update(overrides)
        return info

    def test_zero_entry_point_is_reported_as_none(self):
        text = pe_info.format_info("x", 1, self._info(entry_rva=0))
        self.assertIn("entry point:     none (0)", text)

    def test_unavailable_imports_show_reason(self):
        text = pe_info.format_info("x", 1, self._info(imports=None, import_note="why"))
        self.assertIn("imports:         unavailable (why)", text)

    def test_empty_import_list(self):
        text = pe_info.format_info("x", 1, self._info())
        self.assertIn("imports:         0 DLL(s), 0 function(s)", text)

    def test_no_dll_flags_prints_dash(self):
        text = pe_info.format_info("x", 1, self._info(dll_flags=0))
        self.assertIn("dll flags:       -", text)

    def test_section_permission_letters(self):
        sections = [
            {"name": ".rwx", "vsize": 1, "va": 0x1000, "rawsize": 1, "rawptr": 0x200,
             "chars": 0xE0000000},
            {"name": ".none", "vsize": 1, "va": 0x2000, "rawsize": 1, "rawptr": 0x300,
             "chars": 0},
            {"name": ".rx", "vsize": 1, "va": 0x3000, "rawsize": 1, "rawptr": 0x400,
             "chars": 0x60000000},
        ]
        text = pe_info.format_info("x", 1, self._info(sections=sections))
        self.assertRegex(text, r"\.rwx\s+.*\srwx\n")
        self.assertRegex(text, r"\.none\s+.*\s---\n")
        self.assertRegex(text, r"\.rx\s+.*\sr-x\n")

    def test_timestamp_is_rendered_in_utc(self):
        text = pe_info.format_info("x", 1, self._info(timestamp=1700000000))
        self.assertIn("2023-11-14 22:13:20 UTC (raw 1700000000)", text)


class SelftestTests(unittest.TestCase):
    def test_selftest_passes_and_reports(self):
        rc, out, err = quiet_call(pe_info.main, ["--selftest"])
        self.assertEqual(rc, 0)
        self.assertIn("all PE parser checks passed", out)
        self.assertEqual(err, "")

    def test_selftest_reports_wrong_parse(self):
        real = pe_info.parse_pe

        def wrong_format(data):
            info = real(data)
            info["format"] = "bogus"
            return info

        with mock.patch.object(pe_info, "parse_pe", side_effect=wrong_format):
            rc, _out, err = quiet_call(pe_info.selftest)
        self.assertEqual(rc, 1)
        self.assertIn("selftest FAIL: PE32+: format bogus", err)

    def test_selftest_reports_wrong_error_message(self):
        real = pe_info.parse_pe

        def other_error(data):
            try:
                return real(data)
            except pe_info.PEError:
                raise pe_info.PEError("something else entirely")

        with mock.patch.object(pe_info, "parse_pe", side_effect=other_error):
            rc, _out, err = quiet_call(pe_info.selftest)
        self.assertEqual(rc, 1)
        self.assertIn("wrong message", err)

    def test_selftest_reports_missing_error(self):
        real = pe_info.parse_pe

        def no_error(data):
            try:
                return real(data)
            except pe_info.PEError:
                return {}

        with mock.patch.object(pe_info, "parse_pe", side_effect=no_error):
            rc, _out, err = quiet_call(pe_info.selftest)
        self.assertEqual(rc, 1)
        self.assertIn("no error raised", err)


class MainExitCodeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, name, data):
        path = self.dir / name
        path.write_bytes(data)
        return str(path)

    def test_no_arguments_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(pe_info.main, [])
        self.assertEqual(cm.exception.code, 2)

    def test_unknown_option_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(pe_info.main, ["--bogus"])
        self.assertEqual(cm.exception.code, 2)

    def test_two_paths_is_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            quiet_call(pe_info.main, ["a.exe", "b.exe"])
        self.assertEqual(cm.exception.code, 2)

    def test_valid_file_prints_summary_and_exits_zero(self):
        path = self._write("fixture.exe", pe_info.build_fixture())
        rc, out, err = quiet_call(pe_info.main, [path])
        self.assertEqual(rc, 0)
        self.assertIn("file:            fixture.exe", out)
        self.assertIn("KERNEL32.dll: 2", out)
        self.assertEqual(err, "")

    def test_selftest_flag_ignores_path(self):
        rc, out, _err = quiet_call(pe_info.main, ["--selftest", str(self.dir / "absent")])
        self.assertEqual(rc, 0)
        self.assertIn("all PE parser checks passed", out)

    def test_missing_file_exits_one(self):
        rc, _out, err = quiet_call(pe_info.main, [str(self.dir / "absent.exe")])
        self.assertEqual(rc, 1)
        self.assertIn("cannot read", err)

    def test_directory_exits_one(self):
        rc, _out, err = quiet_call(pe_info.main, [str(self.dir)])
        self.assertEqual(rc, 1)
        self.assertIn("cannot read", err)

    def test_non_pe_file_exits_one(self):
        path = self._write("text.bin", b"hello, not a PE file\n")
        rc, _out, err = quiet_call(pe_info.main, [path])
        self.assertEqual(rc, 1)
        self.assertIn("not a PE file", err)
        self.assertIn("text.bin", err)

    def test_script_exit_status_through_interpreter(self):
        ok = subprocess.run([sys.executable, str(SCRIPT), "--selftest"],
                            capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        bad = subprocess.run([sys.executable, str(SCRIPT), str(self.dir / "absent")],
                             capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)
        usage = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(usage.returncode, 2)
        self.assertIn("usage:", usage.stderr)


if __name__ == "__main__":
    unittest.main()
