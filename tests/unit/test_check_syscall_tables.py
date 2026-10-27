"""Unit tests for tools/check_syscall_tables.py.

Most cases build a small fixture tree with the checker's own render_fixture()
and inject one fault at a time. The last class runs the checker on the real
repository, so it fails when the tracked copies of the table disagree.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_syscall_tables.py"

_spec = importlib.util.spec_from_file_location("check_syscall_tables", SCRIPT)
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)

SERVICES = checker.SELFTEST_SERVICES


def kinds(report):
    return sorted({k for k, _, _ in report.findings})


def fixture_report(tmp, **faults):
    checker.render_fixture(tmp, SERVICES, **faults)
    report, _ = checker.check_tree(tmp)
    return report


class ParserTests(unittest.TestCase):
    def test_table_entries_and_arity(self):
        text = ("#define NT_SYSCALL(Name, Ssn, Arity, ...)\n"
                "NT_SYSCALL(NtClose, 0x000f, 1, HANDLE)\n"
                "NT_SYSCALL(NtTestAlert, 0x01c8, 0)\n")
        entries, unparsed = checker.parse_table(text)
        self.assertEqual(entries, [("NtClose", 0x0f, 1, 1),
                                   ("NtTestAlert", 0x1c8, 0, 0)])
        self.assertEqual(unparsed, 0)

    def test_unparsed_table_line_is_counted(self):
        entries, unparsed = checker.parse_table("NT_SYSCALL(NtClose, zz, 1)\n")
        self.assertEqual(entries, [])
        self.assertEqual(unparsed, 1)

    def test_kernel_list_entries(self):
        entries, unparsed = checker.parse_kernel(
            "NT_SERVICE(0x00bc, NtCreateProcess, 8)\n")
        self.assertEqual(entries, [("NtCreateProcess", 0xbc, 8)])
        self.assertEqual(unparsed, 0)

    def test_stub_shape_and_alias(self):
        text = ("NtClose:\n    mov %rcx, %r10\n    mov $0x000f, %eax\n"
                "    syscall\n    ret\n    .globl ZwClose\n"
                "    .set ZwClose, NtClose\n")
        stubs, labels = checker.parse_stubs(text)
        self.assertEqual(stubs, [("NtClose", 0x0f, "ZwClose", "ZwClose",
                                  "NtClose")])
        self.assertEqual(labels, ["NtClose"])

    def test_def_keeps_repeated_names_and_aliases(self):
        names, aliases = checker.parse_def(
            "LIBRARY ntdll\nEXPORTS\n    NtClose\n    ZwClose=NtClose\n"
            "    NtClose\n")
        self.assertEqual(names, ["NtClose", "ZwClose", "NtClose"])
        self.assertEqual(aliases, {"ZwClose": "NtClose"})

    def test_zw_name_matches_gen_stubs(self):
        self.assertEqual(checker.zw_name("NtClose"), "ZwClose")
        self.assertEqual(checker.zw_name("NtRtlNtStatusToDosError"),
                         "ZwRtlNtStatusToDosError")


class FixtureTests(unittest.TestCase):
    def test_clean_fixture_has_no_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp)
            self.assertEqual(report.findings, [])
            self.assertEqual(kinds(report), [])

    def test_gap_is_information_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp)
            self.assertTrue(any(k == "ssn-gap" for k, _, _ in report.info))

    def test_missing_name_in_kernel_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES)
            path = pathlib.Path(tmp, checker.KERNEL_LISTS[0])
            path.write_text("".join(ln for ln in path.read_text().splitlines(True)
                                    if "NtClose" not in ln), encoding="utf-8")
            report, _ = checker.check_tree(tmp)
            self.assertEqual(kinds(report), ["missing"])
            self.assertIn(checker.KERNEL_LISTS[0], report.findings[0][1])

    def test_extra_name_in_stubs(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES)
            path = pathlib.Path(tmp, checker.STUBS)
            path.write_text(path.read_text() + (
                "NtBogus:\n    mov %rcx, %r10\n    mov $0x0001, %eax\n"
                "    syscall\n    ret\n    .globl ZwBogus\n"
                "    .set ZwBogus, NtBogus\n"), encoding="utf-8")
            report, _ = checker.check_tree(tmp)
            self.assertEqual(kinds(report), ["extra"])

    def test_ssn_mismatch_in_stubs(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, stub_ssn={"NtClose": 0x0010})
            self.assertEqual(kinds(report), ["ssn-mismatch"])

    def test_ssn_mismatch_in_kernel_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, kernel_ssn={"NtCreateFile": 0x0056})
            self.assertEqual(kinds(report), ["ssn-mismatch"])

    def test_duplicate_name_in_kernel_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, dup_kernel=True)
            self.assertEqual(kinds(report), ["duplicate"])

    def test_duplicate_ssn_across_names(self):
        services = [("NtClose", 0x000f, 1, "HANDLE"),
                    ("NtOther", 0x000f, 1, "HANDLE")]
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, services)
            report, _ = checker.check_tree(tmp)
            self.assertIn("duplicate", kinds(report))

    def test_argc_disagreement_with_kernel(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, kernel_argc={"NtClose": 2})
            self.assertEqual(kinds(report), ["argc-mismatch"])

    def test_argtypes_count_is_information_only(self):
        services = [("NtClose", 0x000f, 2, "HANDLE")]
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, services)
            report, _ = checker.check_tree(tmp)
            self.assertEqual(report.findings, [])
            self.assertIn("argc-types", [k for k, _, _ in report.info])

    def test_missing_stub_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, drop_stub={"NtTestAlert"})
            self.assertEqual(kinds(report), ["missing"])

    def test_missing_export_in_def(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, def_omit={"NtClose"})
            self.assertEqual(kinds(report), ["missing"])

    def test_missing_zw_export_in_def(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, def_omit={"ZwClose"})
            self.assertEqual(kinds(report), ["missing"])

    def test_wrong_alias_target_in_stubs_and_def(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, bad_alias={"ZwClose"})
            self.assertEqual(kinds(report), ["alias"])
            files = {path for _, path, _ in report.findings}
            self.assertEqual(files, {checker.STUBS, checker.NTDLL_DEF})

    def test_nr_services_must_be_max_plus_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fixture_report(tmp, nr=0x1dd)
            self.assertEqual(kinds(report), ["nr-services"])

    def test_duplicate_export_line_in_def(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES)
            path = pathlib.Path(tmp, checker.NTDLL_DEF)
            path.write_text(path.read_text() + "    NtClose\n", encoding="utf-8")
            report, _ = checker.check_tree(tmp)
            self.assertEqual(kinds(report), ["duplicate"])

    def test_unparsed_table_line_fails_parse(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES)
            path = pathlib.Path(tmp, checker.TABLE)
            path.write_text(path.read_text() + "NT_SYSCALL(NtBad, zz, 1)\n",
                            encoding="utf-8")
            report, count = checker.check_tree(tmp)
            self.assertEqual(kinds(report), ["parse"])
            self.assertEqual(count, 0)

    def test_missing_file_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            report, count = checker.check_tree(tmp)
            self.assertEqual(kinds(report), ["missing-file"])
            self.assertEqual(count, 0)


class MainTests(unittest.TestCase):
    def run_main(self, argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = checker.main(argv)
        return code, out.getvalue()

    def test_selftest_exits_zero(self):
        code, out = self.run_main(["--selftest"])
        self.assertEqual(code, 0)
        self.assertIn("selftest ok", out)

    def test_clean_tree_exits_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES)
            code, out = self.run_main(["--root", tmp])
            self.assertEqual(code, 0)
            self.assertIn("parity holds", out)

    def test_mismatch_exits_one_and_names_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES, kernel_ssn={"NtClose": 0x0010})
            code, out = self.run_main(["--root", tmp])
            self.assertEqual(code, 1)
            self.assertIn("FAIL [ssn-mismatch]", out)

    def test_quiet_prints_only_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker.render_fixture(tmp, SERVICES, kernel_ssn={"NtClose": 0x0010})
            code, out = self.run_main(["--root", tmp, "--quiet"])
            self.assertEqual(code, 1)
            self.assertEqual(out.strip().count("\n"), 0)

    def test_bad_root_is_usage_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, _ = self.run_main(["--root", str(pathlib.Path(tmp, "nope"))])
            self.assertEqual(code, 2)


class RepositoryParityTests(unittest.TestCase):
    """The tracked copies of the NT syscall table must agree."""

    def test_repository_has_no_mismatch(self):
        report, count = checker.check_tree(str(REPO))
        self.assertEqual(
            report.findings, [],
            "syscall table copies disagree:\n" + "\n".join(
                f"  {k} {p}: {d}" for k, p, d in report.findings))
        # 173 = the 171 services plus NtLoadDriver and NtUnloadDriver (owner pre-authorised).
        self.assertEqual(count, 173)

    def test_repository_exit_status_is_zero(self):
        code, _ = MainTests().run_main(["--root", str(REPO)])
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
