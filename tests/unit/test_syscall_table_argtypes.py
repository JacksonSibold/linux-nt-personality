"""Pin the corrected ArgTypes lists for five entries in syscall_table.def.

These entries had an ArgTypes count that differed from Arity (review SYS-1).
Each expected list follows the kernel handler's signature where one exists
(NtCreateThread, src/kernel/nt/proc_thread.c), and the public NT prototype
otherwise. The generator does not read these columns, so the tests pin the
table text itself, and also check that the checker reports no ArgTypes note
for the five rows.
"""
import importlib.util
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
TABLE = REPO / "src" / "ntdll" / "syscall_table.def"
SCRIPT = REPO / "tools" / "check_syscall_tables.py"

_spec = importlib.util.spec_from_file_location("check_syscall_tables", SCRIPT)
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)

ENTRY_RE = re.compile(
    r"^\s*NT_SYSCALL\s*\(\s*(\w+)\s*,\s*0x[0-9a-fA-F]+\s*,\s*(\d+)\s*,?(.*)\)\s*$",
    re.M)

# name -> (Arity, ArgTypes in order)
EXPECTED = {
    "NtCreateThread": (8, [
        "PHANDLE", "ACCESS_MASK", "POBJECT_ATTRIBUTES", "HANDLE",
        "PVOID", "PVOID", "PVOID", "BOOLEAN"]),
    "NtQueryDirectoryFile": (11, [
        "HANDLE", "HANDLE", "PVOID", "PVOID", "PIO_STATUS_BLOCK", "PVOID",
        "ULONG", "ULONG", "BOOLEAN", "PUNICODE_STRING", "BOOLEAN"]),
    "NtQueryDirectoryFileEx": (10, [
        "HANDLE", "HANDLE", "PVOID", "PVOID", "PIO_STATUS_BLOCK", "PVOID",
        "ULONG", "ULONG", "ULONG", "PUNICODE_STRING"]),
    "NtSetTimer": (7, [
        "HANDLE", "PLARGE_INTEGER", "PVOID", "PVOID", "BOOLEAN", "LONG",
        "PBOOLEAN"]),
    "NtAccessCheckAndAuditAlarm": (11, [
        "PUNICODE_STRING", "PVOID", "PUNICODE_STRING", "PUNICODE_STRING",
        "PSECURITY_DESCRIPTOR", "ACCESS_MASK", "PGENERIC_MAPPING", "BOOLEAN",
        "PACCESS_MASK", "PNTSTATUS", "PBOOLEAN"]),
}


def table_entries():
    text = TABLE.read_text(encoding="utf-8").replace("\r\n", "\n")
    out = {}
    for m in ENTRY_RE.finditer(text):
        name, arity, rest = m.group(1), int(m.group(2)), m.group(3)
        types = [t.strip() for t in rest.split(",") if t.strip()]
        out[name] = (arity, types)
    return out


class SyscallArgTypesTest(unittest.TestCase):
    def test_five_entries_match_signatures(self):
        entries = table_entries()
        for name, (arity, types) in EXPECTED.items():
            with self.subTest(name=name):
                self.assertIn(name, entries)
                self.assertEqual(entries[name], (arity, types))
                self.assertEqual(len(types), arity)

    def test_checker_reports_no_argtypes_note_for_these_five(self):
        report, _ = checker.check_tree(str(REPO))
        flagged = {detail.split(" ", 1)[0]
                   for kind, _, detail in report.info if kind == "argc-types"}
        self.assertFalse(flagged & set(EXPECTED), flagged & set(EXPECTED))


if __name__ == "__main__":
    unittest.main()
