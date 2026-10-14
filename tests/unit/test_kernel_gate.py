"""Unit tests for tools/kernel_gate.py.

The gate compares what the repository expects with `nm` and `ar t` output. These
tests use a synthetic repository (service list, handler sources, Makefile) and
synthetic nm / ar text, so the verdict logic is checked without a kernel. One test
runs the real repository read-only: the expectations come from the inventory, and a
synthetic binary built from them must pass, while removing one expected symbol
must fail on exactly that service.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "kernel_gate.py"

_spec = importlib.util.spec_from_file_location("kernel_gate", SCRIPT)
kg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kg)
si = kg.si

SERVICES = (
    "NT_SERVICE(0x0001, NtFoo, 1)\n"
    "NT_SERVICE(0x0002, NtBar, 0)\n"
    "NT_SERVICE(0x0003, NtBaz, 2)\n"
    "NT_SERVICE(0x0004, NtGap, 1)\n"
    "NT_SERVICE(0x0005, NtWeak, 1)\n"
)
HANDLERS = {
    "a.c": (
        "NTSTATUS nt_svc_NtFoo(void) { return STATUS_SUCCESS; }\n"
        "NTSTATUS nt_svc_NtBar(void) { return STATUS_NOT_IMPLEMENTED; }\n"
        "NTSTATUS nt_svc_NtBaz(PVOID p, ULONG n) { if (!p) return STATUS_INVALID_PARAMETER; return STATUS_ACCESS_DENIED; }\n"
        "__attribute__((weak)) NTSTATUS nt_svc_NtWeak(void) { return STATUS_NOT_IMPLEMENTED; }\n"
    ),
}
MAKEFILE = (
    "ccflags-y += -w\n"
    "obj-$(CONFIG_NT_PERSONALITY) += dispatch.o\n"
    "obj-$(CONFIG_NT_PERSONALITY) += job.o\n"
)
AR_OK = "dispatch.o\njob.o\n"
CONFIG_ON = "CONFIG_NT_PERSONALITY=y\n"


def make_repo(root, services=SERVICES, handlers=HANDLERS, makefile=MAKEFILE):
    (root / si.SERVICE_LIST).parent.mkdir(parents=True, exist_ok=True)
    (root / si.SERVICE_LIST).write_text(services, encoding="utf-8")
    hdir = root / si.HANDLER_DIR
    hdir.mkdir(parents=True, exist_ok=True)
    for name, text in handlers.items():
        (hdir / name).write_text(text, encoding="utf-8")
    (root / kg.MAKEFILE).parent.mkdir(parents=True, exist_ok=True)
    (root / kg.MAKEFILE).write_text(makefile, encoding="utf-8")


def nm_for(names, weak=(), entries=True):
    """Synthetic nm output: strong T symbols for names, W for weak, plus the entry points."""
    lines = []
    addr = 0xffffffff81000000
    syms = [(n, "T") for n in names] + [(n, "W") for n in weak]
    if entries:
        syms += [(e, "T") for e in kg.ENTRY_SYMBOLS]
    for name, kind in syms:
        lines.append("%016x %s %s" % (addr, kind, name))
        addr += 0x10
    return "\n".join(lines) + "\n"


def gate(root, nm_text, ar_text=AR_OK, config_text=CONFIG_ON):
    rows = quiet(si.run_inventory, str(root))
    makefile = (root / kg.MAKEFILE).read_text(encoding="utf-8")
    return kg.evaluate(rows, nm_text, makefile, ar_text, config_text)


def quiet(fn, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kwargs)


class GateTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        make_repo(self.root)
        self.good_names = ["nt_svc_NtFoo", "nt_svc_NtBar", "nt_svc_NtBaz"]

    def tearDown(self):
        self._tmp.cleanup()


class PassFailTest(GateTestCase):
    def test_passes_when_every_expected_symbol_is_strong(self):
        report = gate(self.root, nm_for(self.good_names, weak=["nt_svc_NtWeak"]))
        self.assertEqual(report["verdict"], "PASS", report["problems"])
        self.assertEqual(report["services"]["expected"], 3)
        self.assertEqual(report["services"]["present"], 3)
        self.assertEqual(report["services"]["shortfalls"], [])

    def test_gaps_and_stubs_are_reported_without_failing(self):
        report = gate(self.root, nm_for(self.good_names))
        self.assertEqual(report["verdict"], "PASS")
        gaps = {g["name"]: g["kernel_class"] for g in report["services"]["gaps"]}
        self.assertEqual(gaps, {"NtGap": si.MISSING, "NtWeak": si.WEAK})
        self.assertEqual([s["name"] for s in report["services"]["stubs"]], ["NtBar"])

    def test_fails_on_a_missing_expected_symbol(self):
        names = [n for n in self.good_names if n != "nt_svc_NtBaz"]
        report = gate(self.root, nm_for(names))
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual([s["name"] for s in report["services"]["shortfalls"]], ["NtBaz"])
        self.assertEqual(report["services"]["shortfalls"][0]["binary"], "absent")

    def test_weak_only_symbol_is_a_shortfall_for_an_expected_service(self):
        report = gate(self.root, nm_for(["nt_svc_NtFoo", "nt_svc_NtBaz"], weak=["nt_svc_NtBar"]))
        self.assertEqual(report["verdict"], "FAIL")
        short = {s["name"]: s["binary"] for s in report["services"]["shortfalls"]}
        self.assertEqual(short, {"NtBar": "defined, weak binding"})

    def test_fails_on_a_missing_entry_point(self):
        nm = nm_for(self.good_names)
        nm = "\n".join(l for l in nm.splitlines() if not l.endswith(" do_nt_syscall")) + "\n"
        report = gate(self.root, nm)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["entry_points"]["missing"], ["do_nt_syscall"])

    def test_empty_binary_fails_with_every_service_short(self):
        report = gate(self.root, "")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(len(report["services"]["shortfalls"]), 3)


class ObjectCheckTest(GateTestCase):
    def test_fails_on_a_missing_expected_object(self):
        report = gate(self.root, nm_for(self.good_names), ar_text="dispatch.o\n")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["objects"]["missing"], ["job.o"])

    def test_members_recorded_with_a_directory_match_by_base_name(self):
        report = gate(self.root, nm_for(self.good_names),
                      ar_text="/build/arch/x86/nt/dispatch.o\n/build/arch/x86/nt/job.o\n")
        self.assertEqual(report["verdict"], "PASS", report["problems"])
        self.assertEqual(report["objects"]["missing"], [])

    def test_extra_archive_member_is_reported_and_passes(self):
        report = gate(self.root, nm_for(self.good_names), ar_text=AR_OK + "stubs.o\n")
        self.assertEqual(report["verdict"], "PASS")
        self.assertEqual(report["objects"]["extra"], ["stubs.o"])

    def test_object_check_skipped_when_personality_is_off(self):
        report = gate(self.root, nm_for(self.good_names), ar_text=None,
                      config_text="CONFIG_NT_PERSONALITY=n\n")
        self.assertEqual(report["verdict"], "PASS")
        self.assertFalse(report["objects"]["checked"])
        self.assertIn("not y", report["objects"]["note"])

    def test_missing_archive_fails_when_personality_is_on(self):
        report = gate(self.root, nm_for(self.good_names), ar_text=None)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("built-in.a not found", report["objects"]["error"])

    def test_no_config_skips_the_object_check(self):
        report = gate(self.root, nm_for(self.good_names), ar_text=None, config_text=None)
        self.assertEqual(report["verdict"], "PASS")
        self.assertIn("no .config", report["objects"]["note"])

    def test_makefile_objects_are_parsed_in_order(self):
        text = ("ccflags-y += -w\n"
                "obj-$(CONFIG_NT_PERSONALITY) += dispatch.o\n"
                "obj-y += other.o\n"
                "obj-$(CONFIG_NT_PERSONALITY) += nt_io.o\n")
        self.assertEqual(kg.parse_makefile_objects(text), ["dispatch.o", "nt_io.o"])


class NmParseTest(unittest.TestCase):
    def test_symbol_classes(self):
        nm = ("ffffffff81000000 T nt_svc_A\n"
              "ffffffff81000010 W nt_svc_B\n"
              "                 w nt_svc_C\n"
              "                 U nt_svc_D\n"
              "ffffffff81000020 t nt_svc_E\n"
              "ffffffff81000030 A nt_svc_F\n")
        got = kg.parse_nm(nm)
        self.assertEqual(got["nt_svc_A"], "strong")
        self.assertEqual(got["nt_svc_B"], "weak")
        self.assertEqual(got["nt_svc_C"], "undefined")
        self.assertEqual(got["nt_svc_D"], "undefined")
        self.assertEqual(got["nt_svc_E"], "strong")
        self.assertEqual(got["nt_svc_F"], "other")

    def test_strongest_definition_wins(self):
        nm = ("ffffffff81000000 W nt_svc_X\n"
              "ffffffff81000010 T nt_svc_X\n"
              "                 w nt_svc_X\n")
        self.assertEqual(kg.parse_nm(nm)["nt_svc_X"], "strong")

    def test_archive_listing_is_one_name_per_line(self):
        self.assertEqual(kg.parse_ar("dispatch.o\n\njob.o\n"), ["dispatch.o", "job.o"])

    def test_archive_members_recorded_with_a_directory_match_by_base_name(self):
        listing = "/home/ntpers/linux/arch/x86/nt/dispatch.o\nnt_io.o\n"
        self.assertEqual(kg.parse_ar(listing), ["dispatch.o", "nt_io.o"])


class ReportTest(GateTestCase):
    def test_text_report_names_the_shortfall_and_verdict(self):
        names = [n for n in self.good_names if n != "nt_svc_NtBaz"]
        rows = quiet(si.run_inventory, str(self.root))
        report = kg.evaluate(rows, nm_for(names), MAKEFILE, AR_OK, CONFIG_ON)
        text = kg.render_text(report, "vmlinux", str(self.root))
        self.assertIn("SHORTFALL: 1", text)
        self.assertIn("NtBaz", text)
        self.assertIn("VERDICT: FAIL", text)

    def test_main_exit_codes_and_json(self):
        nm = self.root / "nm.txt"
        ar = self.root / "ar.txt"
        cfg = self.root / ".config"
        nm.write_text(nm_for(self.good_names), encoding="utf-8")
        ar.write_text(AR_OK, encoding="utf-8")
        cfg.write_text(CONFIG_ON, encoding="utf-8")
        args = ["--repo", str(self.root), "--nm", str(nm), "--ar", str(ar), "--config", str(cfg)]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(kg.main(args + ["--json"]), 0)
        self.assertEqual(json.loads(out.getvalue())["verdict"], "PASS")

        nm.write_text(nm_for(self.good_names[:2]), encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(kg.main(args), 1)

    def test_main_usage_error_on_missing_nm_file(self):
        with contextlib.redirect_stderr(io.StringIO()):
            code = kg.main(["--repo", str(self.root), "--nm", str(self.root / "absent.txt")])
        self.assertEqual(code, 2)


class RepositoryExpectationTest(unittest.TestCase):
    """The expectations are whatever the inventory says, not a constant."""

    def test_expected_symbols_come_from_the_inventory(self):
        rows = quiet(si.run_inventory, str(REPO))
        expected = [r["name"] for r in rows if r["kernel_class"] in kg.STRONG_CLASSES]
        nm = nm_for(["nt_svc_" + n for n in expected])
        makefile = (REPO / kg.MAKEFILE).read_text(encoding="utf-8")
        objects = kg.parse_makefile_objects(makefile)
        report = kg.evaluate(rows, nm, makefile, "\n".join(objects) + "\n", CONFIG_ON)
        self.assertEqual(report["verdict"], "PASS", report["problems"])
        self.assertEqual(report["services"]["expected"], len(expected))
        self.assertEqual(len(report["services"]["gaps"]), len(rows) - len(expected))
        self.assertEqual(report["objects"]["expected"], objects)
        self.assertGreater(len(objects), 0)

    def test_removing_one_expected_symbol_fails_on_that_service(self):
        rows = quiet(si.run_inventory, str(REPO))
        expected = [r["name"] for r in rows if r["kernel_class"] in kg.STRONG_CLASSES]
        victim = expected[0]
        nm = nm_for(["nt_svc_" + n for n in expected if n != victim])
        makefile = (REPO / kg.MAKEFILE).read_text(encoding="utf-8")
        objects = kg.parse_makefile_objects(makefile)
        report = kg.evaluate(rows, nm, makefile, "\n".join(objects) + "\n", CONFIG_ON)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual([s["name"] for s in report["services"]["shortfalls"]], [victim])


if __name__ == "__main__":
    unittest.main()
