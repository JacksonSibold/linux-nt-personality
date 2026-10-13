"""Unit tests for tools/service_inventory.py.

The script is loaded by path (main() sits behind a __main__ guard). Pure
helpers get small synthetic inputs. The filesystem tests build a fixture tree
in a temporary directory. One test runs the real repository read-only and
checks the counts it must have.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "service_inventory.py"

_spec = importlib.util.spec_from_file_location("service_inventory", SCRIPT)
si = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(si)


def quiet(fn, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kwargs)


def make_tree(root, service_list, sources):
    """Write a fixture repository: the service list and handler sources."""
    hdir = root / si.HANDLER_DIR
    hdir.mkdir(parents=True, exist_ok=True)
    (root / si.SERVICE_LIST).parent.mkdir(parents=True, exist_ok=True)
    (root / si.SERVICE_LIST).write_text(service_list, encoding="utf-8")
    for name, text in sources.items():
        (hdir / name).write_text(text, encoding="utf-8")


class StubDetectionTest(unittest.TestCase):
    def test_plain_not_implemented_is_stub(self):
        self.assertTrue(si.body_is_stub(" return STATUS_NOT_IMPLEMENTED; "))

    def test_not_supported_is_stub(self):
        self.assertTrue(si.body_is_stub(" return STATUS_NOT_SUPPORTED; "))

    def test_void_casts_are_ignored(self):
        self.assertTrue(si.body_is_stub(" (void)a; (void)b[2]; return STATUS_NOT_SUPPORTED; "))

    def test_conditional_is_not_stub(self):
        self.assertFalse(si.body_is_stub(
            " if (x) return STATUS_NOT_IMPLEMENTED; return STATUS_SUCCESS; "))

    def test_success_is_not_stub(self):
        self.assertFalse(si.body_is_stub(" return STATUS_SUCCESS; "))

    def test_other_status_is_not_stub(self):
        self.assertFalse(si.body_is_stub(" return STATUS_INVALID_PARAMETER; "))


class ScanTest(unittest.TestCase):
    def test_prototype_is_not_a_definition(self):
        self.assertEqual(list(si.scan_definitions("NTSTATUS nt_svc_NtX(void);")), [])

    def test_commented_definition_is_ignored(self):
        text = "/* NTSTATUS nt_svc_NtX(void) { return 0; } */"
        self.assertEqual(list(si.scan_definitions(text)), [])

    def test_string_with_brace_does_not_end_body(self):
        text = 'NTSTATUS nt_svc_NtX(void) { const char *s = "}"; return STATUS_SUCCESS; }'
        defs = list(si.scan_definitions(text))
        self.assertEqual(len(defs), 1)
        self.assertIn("STATUS_SUCCESS", defs[0][2])

    def test_weak_attribute_is_reported(self):
        text = "__attribute__((weak)) NTSTATUS nt_svc_NtX(void) { return STATUS_SUCCESS; }"
        defs = list(si.scan_definitions(text))
        self.assertEqual([(n, w) for n, w, _ in defs], [("NtX", True)])

    def test_area_rules(self):
        self.assertEqual(si.area_of("NtQueryTimerResolution"), "system")
        self.assertEqual(si.area_of("NtSetTimer"), "sync")
        self.assertEqual(si.area_of("NtQueryEaFile"), "io")
        self.assertEqual(si.area_of("NtFooBar"), si.OTHER_AREA)


class FixtureTreeTest(unittest.TestCase):
    LIST = (
        "NT_SERVICE(0x0046, NtYieldExecution, 0)\n"
        "NT_SERVICE(0x0031, NtQueryPerformanceCounter, 2)\n"
        "NT_SERVICE(0x0040, NtOpenEvent, 3)\n"
        "NT_SERVICE(0x0001, NtWeakOnly, 1)\n"
        "NT_SERVICE(0x0003, NtAbsent, 2)\n"
        "NT_SERVICE(0x0002, NtOverridden, 2)\n"
    )

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        make_tree(self.root, self.LIST, {
            "impl.c": "NTSTATUS nt_svc_NtYieldExecution(void) { cond_resched(); return STATUS_SUCCESS; }\n"
                      "NTSTATUS nt_svc_NtQueryPerformanceCounter(void) { return STATUS_NOT_IMPLEMENTED; }\n",
            "weak.c": "__attribute__((weak)) NTSTATUS nt_svc_NtOpenEvent(void) { return STATUS_NOT_IMPLEMENTED; }\n"
                      "__attribute__((weak)) NTSTATUS nt_svc_NtWeakOnly(void) { return STATUS_SUCCESS; }\n"
                      "__attribute__((weak)) NTSTATUS nt_svc_NtOverridden(void) { return STATUS_NOT_IMPLEMENTED; }\n",
            "over.c": "NTSTATUS nt_svc_NtOverridden(void) { return STATUS_SUCCESS; }\n",
            "x_test.c": "NTSTATUS nt_svc_NtAbsent(void) { return STATUS_SUCCESS; }\n",
        })

    def tearDown(self):
        self._tmp.cleanup()

    def test_classification(self):
        rows = {r["name"]: r for r in si.run_inventory(str(self.root))}
        self.assertEqual(rows["NtYieldExecution"]["status"], si.IMPLEMENTED)
        self.assertEqual(rows["NtQueryPerformanceCounter"]["status"], si.STUB)
        self.assertEqual(rows["NtOpenEvent"]["status"], si.WEAK)
        self.assertEqual(rows["NtWeakOnly"]["status"], si.WEAK)
        self.assertEqual(rows["NtAbsent"]["status"], si.MISSING)
        self.assertEqual(rows["NtOverridden"]["status"], si.IMPLEMENTED)
        self.assertEqual(rows["NtOverridden"]["file"], "src/kernel/nt/over.c")

    def test_summary_counts(self):
        rows = si.run_inventory(str(self.root))
        counts = si.summarize(rows)
        self.assertEqual(counts[si.IMPLEMENTED], 2)
        self.assertEqual(counts[si.STUB], 1)
        self.assertEqual(counts[si.WEAK], 2)
        self.assertEqual(counts[si.MISSING], 1)

    def test_markdown_is_deterministic_and_has_areas(self):
        rows = si.run_inventory(str(self.root))
        first = si.render_markdown(rows)
        self.assertEqual(first, si.render_markdown(rows))
        self.assertIn("## Area: system", first)
        self.assertIn("| 0x0046 | `NtYieldExecution` | 0 | IMPLEMENTED |", first)

    def test_main_writes_out_file(self):
        out = self.root / "SERVICES.md"
        rc = quiet(si.main, ["--root", str(self.root), "--markdown", "--out", str(out)])
        self.assertEqual(rc, 0)
        self.assertIn("# NT service handler inventory", out.read_text(encoding="utf-8"))

    def test_missing_service_list_is_an_error(self):
        empty = pathlib.Path(tempfile.mkdtemp())
        rc = quiet(si.main, ["--root", str(empty)])
        self.assertEqual(rc, 1)


class SelftestTest(unittest.TestCase):
    def test_builtin_selftest_passes(self):
        self.assertFalse(quiet(si.selftest))


class RepositoryTest(unittest.TestCase):
    """Read-only checks against the real tree. Counts must agree with the table."""

    def test_every_table_entry_is_classified(self):
        rows = si.run_inventory(str(REPO))
        services = si.parse_services(str(REPO))
        self.assertEqual(len(rows), len(services))
        self.assertGreater(len(rows), 0)
        for r in rows:
            self.assertIn(r["status"], si.STATUSES)

    def test_ssns_are_unique(self):
        ssns = [s["ssn"] for s in si.parse_services(str(REPO))]
        self.assertEqual(len(ssns), len(set(ssns)))

    def test_generated_report_is_current(self):
        report = REPO / "docs" / "status" / "SERVICES.md"
        if not report.exists():
            self.skipTest("docs/status/SERVICES.md not generated yet")
        rows = si.run_inventory(str(REPO))
        self.assertEqual(report.read_text(encoding="utf-8"), si.render_markdown(rows))


if __name__ == "__main__":
    unittest.main()
