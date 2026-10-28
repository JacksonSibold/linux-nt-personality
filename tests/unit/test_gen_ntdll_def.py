"""Unit tests for build/gen_ntdll_def.py.

The script has no importable functions, so it is run as a subprocess against
temporary .def fixtures and the generated ntdll.def is inspected.
"""
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "gen_ntdll_def.py"

# Exports the script always emits, regardless of the syscall table.
# NtRaiseException is a syscall-table entry, so it is not in the fixed set.
FIXED_EXPORT_COUNT = 6 + 12 + 10 + 3  # Ldr + Rtl + exception/VEH + misc


def run_script(src_text):
    """Run gen_ntdll_def.py. Returns (returncode, stdout, def_lines or None)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        src = tmp_path / "syscall_table.def"
        dst = tmp_path / "ntdll.def"
        src.write_text(src_text)
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), str(src), str(dst)],
            capture_output=True, text=True,
        )
        lines = dst.read_text().splitlines() if dst.exists() else None
        return proc.returncode, proc.stdout, lines


def export_lines(lines):
    """Export entries are the indented lines after the EXPORTS keyword."""
    return [ln.strip() for ln in lines if ln.startswith("    ")]


def run_with_extras(extras_text):
    """Run the script with a ntdll_exports.txt next to the syscall table."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        (tmp_path / "syscall_table.def").write_text("NT_SYSCALL(NtClose, 0x0f)\n")
        (tmp_path / "ntdll_exports.txt").write_text(extras_text)
        dst = tmp_path / "ntdll.def"
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), str(tmp_path / "syscall_table.def"), str(dst)],
            capture_output=True, text=True,
        )
        lines = dst.read_text().splitlines() if dst.exists() else None
        return proc, lines


class GenNtdllDefExtrasTests(unittest.TestCase):

    def test_extra_exports_are_appended_after_the_fixed_ones(self):
        proc, lines = run_with_extras("RtlAllocateHeap\nRtlFreeHeap\n")
        self.assertEqual(proc.returncode, 0)
        exports = export_lines(lines)
        self.assertEqual(exports[-2:], ["RtlAllocateHeap", "RtlFreeHeap"])
        self.assertEqual(len(exports), 2 + FIXED_EXPORT_COUNT + 2)
        printed = int(re.search(r"with (\d+) exports", proc.stdout).group(1))
        self.assertEqual(printed, len(exports))

    def test_comments_blank_lines_and_aliases(self):
        proc, lines = run_with_extras("# heap\n\n  RtlA   # trailing comment\nRtlB=RtlA\n")
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(export_lines(lines)[-2:], ["RtlA", "RtlB=RtlA"])

    def test_duplicate_extra_export_is_an_error(self):
        proc, _ = run_with_extras("RtlA\nRtlA\n")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("RtlA is listed twice", proc.stderr)

    def test_no_extras_file_changes_nothing(self):
        _, _, lines = run_script("NT_SYSCALL(NtClose, 0x0f)\n")
        self.assertEqual(len(export_lines(lines)), 2 + FIXED_EXPORT_COUNT)


class GenNtdllDefTests(unittest.TestCase):

    def test_header(self):
        _, _, lines = run_script("NT_SYSCALL(NtClose, 0x0f)\n")
        self.assertEqual(lines[0], "LIBRARY ntdll")
        self.assertEqual(lines[1], "EXPORTS")

    def test_nt_and_zw_pair_emitted_per_syscall(self):
        _, _, lines = run_script("NT_SYSCALL(NtClose, 0x0f)\n")
        exports = export_lines(lines)
        self.assertIn("NtClose", exports)
        self.assertIn("ZwClose=NtClose", exports)
        self.assertLess(exports.index("NtClose"), exports.index("ZwClose=NtClose"))

    def test_zw_alias_for_name_without_nt_prefix(self):
        _, _, lines = run_script("NT_SYSCALL(Foo, 0x01)\n")
        exports = export_lines(lines)
        self.assertIn("Foo", exports)
        self.assertIn("ZwFoo=Foo", exports)

    def test_fixed_exports_present(self):
        _, _, lines = run_script("")
        exports = export_lines(lines)
        for name in ("LdrLoadDll", "RtlUnwindEx", "KiUserExceptionDispatcher",
                     "NtCurrentTeb", "DllMain", "_DllMainCRTStartup",
                     "DllMainCRTStartup", "_NtRaiseException"):
            self.assertIn(name, exports)

    def test_export_count_is_pairs_plus_fixed_set(self):
        entries = "".join(
            f"NT_SYSCALL(Nt{n}, 0x{n:02x})\n" for n in range(1, 4))
        _, _, lines = run_script(entries)
        self.assertEqual(len(export_lines(lines)),
                         3 * 2 + FIXED_EXPORT_COUNT)

    def test_empty_table_emits_only_fixed_exports(self):
        _, stdout, lines = run_script("")
        self.assertEqual(len(export_lines(lines)), FIXED_EXPORT_COUNT)
        self.assertIn("Generated", stdout)

    def test_printed_export_count_matches_written_exports(self):
        _, stdout, lines = run_script(
            "NT_SYSCALL(NtClose, 0x0f)\nNT_SYSCALL(NtOpen, 0x10)\n")
        printed = int(re.search(r"with (\d+) exports", stdout).group(1))
        self.assertEqual(printed, len(export_lines(lines)))

    def test_missing_arguments_fail(self):
        proc = subprocess.run(
            [sys.executable, str(SCRIPT)], capture_output=True, text=True,
        )
        self.assertNotEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
