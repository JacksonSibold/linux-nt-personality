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


class RefusalClassTest(unittest.TestCase):
    """refusal_class: STUB for a bare stub return, REFUSES for a refusal after
    validation or with another refusal status, None for anything else."""

    def test_bare_stub_returns_are_stub(self):
        self.assertEqual(si.refusal_class(" return STATUS_NOT_IMPLEMENTED; "), si.STUB)
        self.assertEqual(si.refusal_class(" (void)a; return STATUS_NOT_SUPPORTED; "), si.STUB)

    def test_bare_other_refusals_are_refuses(self):
        self.assertEqual(si.refusal_class(" return STATUS_ACCESS_DENIED; "), si.REFUSES)
        self.assertEqual(si.refusal_class(" (void)a; return STATUS_INVALID_DEVICE_REQUEST; "),
                         si.REFUSES)

    def test_validation_prefix_then_refusal_is_refuses(self):
        body = " if (!a) return STATUS_INVALID_PARAMETER; return STATUS_NOT_SUPPORTED; "
        self.assertEqual(si.refusal_class(body), si.REFUSES)
        body = " if (!a) return STATUS_INVALID_HANDLE; if (b) return STATUS_INVALID_ADDRESS; " \
               "return STATUS_ACCESS_DENIED; "
        self.assertEqual(si.refusal_class(body), si.REFUSES)

    def test_braced_validation_is_accepted(self):
        body = " if (!a) { return STATUS_INVALID_PARAMETER; } return STATUS_NOT_IMPLEMENTED; "
        self.assertEqual(si.refusal_class(body), si.REFUSES)

    def test_conditional_refusal_is_not_refusal(self):
        self.assertIsNone(si.refusal_class(
            " if (x) return STATUS_NOT_IMPLEMENTED; return STATUS_NOT_SUPPORTED; "))

    def test_other_statement_disqualifies(self):
        self.assertIsNone(si.refusal_class(
            " if (!a) return STATUS_INVALID_PARAMETER; x = 1; return STATUS_NOT_SUPPORTED; "))

    def test_validation_status_outside_set_disqualifies(self):
        self.assertIsNone(si.refusal_class(
            " if (!a) return STATUS_INVALID_INFO_CLASS; return STATUS_NOT_SUPPORTED; "))

    def test_success_and_empty_bodies_are_not_refusals(self):
        self.assertIsNone(si.refusal_class(" return STATUS_SUCCESS; "))
        self.assertIsNone(si.refusal_class("  "))

    def test_body_class_defaults_to_implemented(self):
        self.assertEqual(si.body_class(" return STATUS_SUCCESS; "), si.IMPLEMENTED)


class ClassRefusalTest(unittest.TestCase):
    """STATUS_INVALID_INFO_CLASS is a REFUSES status only for a body that refuses
    every class: the final return is unconditional and every earlier statement is
    a validation return. Any dispatch on the class makes the body IMPLEMENTED."""

    def test_bare_invalid_info_class_refuses_every_class(self):
        self.assertEqual(si.refusal_class(" return STATUS_INVALID_INFO_CLASS; "), si.REFUSES)

    def test_void_cast_invalid_info_class_refuses_every_class(self):
        self.assertEqual(si.refusal_class(
            " (void)a; (void)b; return STATUS_INVALID_INFO_CLASS; "), si.REFUSES)

    def test_validated_invalid_info_class_refuses_every_class(self):
        self.assertEqual(si.refusal_class(
            " if (!a) return STATUS_INVALID_PARAMETER; return STATUS_INVALID_INFO_CLASS; "),
            si.REFUSES)

    def test_class_dispatch_is_not_a_refusal(self):
        self.assertIsNone(si.refusal_class(
            " switch (c) { case 1: return STATUS_SUCCESS; default: return STATUS_INVALID_INFO_CLASS; } "))

    def test_conditional_invalid_info_class_is_not_a_refusal(self):
        self.assertIsNone(si.refusal_class(
            " if (c == 1) return STATUS_INVALID_INFO_CLASS; return STATUS_SUCCESS; "))

    def test_invalid_info_class_is_not_a_stub(self):
        self.assertNotEqual(si.refusal_class(" return STATUS_INVALID_INFO_CLASS; "), si.STUB)

    def test_fixture_refuse_all_classes_is_refuses(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make_tree(root, "NT_SERVICE(0x001c, NtSetInformationProcess, 4)\n", {
                "proc.c": "NTSTATUS nt_svc_NtSetInformationProcess(HANDLE a, ULONG b, PVOID c, ULONG d)\n"
                          "{\n    (void)a; (void)c; (void)d;\n    return STATUS_INVALID_INFO_CLASS;\n}\n",
            })
            rows = quiet(si.run_inventory, root)
        self.assertEqual([r["status"] for r in rows], [si.REFUSES])

    def test_fixture_class_dispatch_is_implemented(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make_tree(root, "NT_SERVICE(0x001c, NtSetInformationProcess, 4)\n", {
                "proc.c": "NTSTATUS nt_svc_NtSetInformationProcess(HANDLE a, ULONG b, PVOID c, ULONG d)\n"
                          "{\n    switch (b) {\n    case 5: return STATUS_SUCCESS;\n"
                          "    default: return STATUS_INVALID_INFO_CLASS;\n    }\n}\n",
            })
            rows = quiet(si.run_inventory, root)
        self.assertEqual([r["status"] for r in rows], [si.IMPLEMENTED])


class BranchLabelTest(unittest.TestCase):
    def test_kernel_conditions(self):
        self.assertEqual(si.kernel_branch_labels("ifdef", "__KERNEL__"), (si.KERNEL, si.USERSPACE))
        self.assertEqual(si.kernel_branch_labels("ifndef", "__KERNEL__"), (si.USERSPACE, si.KERNEL))
        self.assertEqual(si.kernel_branch_labels("if", "defined(__KERNEL__)"),
                         (si.KERNEL, si.USERSPACE))
        self.assertEqual(si.kernel_branch_labels("if", "defined __KERNEL__"),
                         (si.KERNEL, si.USERSPACE))
        self.assertEqual(si.kernel_branch_labels("if", "!defined(__KERNEL__)"),
                         (si.USERSPACE, si.KERNEL))

    def test_other_conditions_are_transparent(self):
        self.assertEqual(si.kernel_branch_labels("ifdef", "__linux__"), (None, None))
        self.assertEqual(si.kernel_branch_labels("if", "defined(__KERNEL__) && X"), (None, None))
        self.assertEqual(si.kernel_branch_labels("if", "CONFIG_X86_64"), (None, None))

    def test_branch_lines(self):
        text = "a\n#ifdef __KERNEL__\nk\n#else\nu\n#endif\nz\n"
        labels = si.branch_labels(text)
        self.assertEqual(labels[2], si.KERNEL)
        self.assertEqual(labels[4], si.USERSPACE)
        self.assertIsNone(labels[0])
        self.assertIsNone(labels[6])

    def test_nested_transparent_conditional_keeps_outer_label(self):
        text = "#ifndef __KERNEL__\n#ifdef __linux__\nx\n#else\ny\n#endif\n#endif\n"
        labels = si.branch_labels(text)
        self.assertEqual(labels[2], si.USERSPACE)
        self.assertEqual(labels[4], si.USERSPACE)

    def test_elif_makes_branch_unlabelled(self):
        text = "#ifdef __KERNEL__\na\n#elif X\nb\n#else\nc\n#endif\n"
        labels = si.branch_labels(text)
        self.assertEqual(labels[1], si.KERNEL)
        self.assertIsNone(labels[3])
        self.assertIsNone(labels[5])

    def test_definition_label_comes_from_its_branch(self):
        text = ("#ifdef __KERNEL__\nNTSTATUS nt_svc_NtA(void) { return STATUS_NOT_IMPLEMENTED; }\n"
                "#else\nNTSTATUS nt_svc_NtA(void) { return STATUS_SUCCESS; }\n#endif\n")
        defs = list(si.scan_definitions_with_branch(text))
        self.assertEqual([d[3] for d in defs], [si.KERNEL, si.USERSPACE])


class ScanFunctionsTest(unittest.TestCase):
    def test_finds_definitions_not_calls_or_prototypes(self):
        text = ("static NTSTATUS helper_one(int a)\n{\n    if (a) { foo(a); }\n    return 0;\n}\n"
                "NTSTATUS proto_only(void);\n"
                "NTSTATUS nt_svc_NtX(void) { return 0; }\n")
        self.assertEqual(list(si.scan_functions(text)), ["helper_one"])

    def test_macro_bodies_are_skipped(self):
        text = "#define LOCK_BODY(a) { return 0; }\nstatic int real_one(void) { return 1; }\n"
        self.assertEqual(list(si.scan_functions(text)), ["real_one"])

    def test_normalised_names(self):
        self.assertEqual(si.norm_name("NtCreateSection"), si.norm_name("create_section"))
        self.assertEqual(si.norm_name("nt_query_directory_object_internal"),
                         si.norm_name("NtQueryDirectoryObject"))
        self.assertNotEqual(si.norm_name("NtCreateProcess"), si.norm_name("NtCreateProcessEx"))


class ClassifyTest(unittest.TestCase):
    """Rows for synthetic handler maps (no filesystem)."""

    SVC = [{"ssn": 1, "name": "NtA", "argc": 0}]

    def classify_one(self, handler, impls=None):
        return si.classify(self.SVC, {"NtA": handler} if handler else {}, impls)[0]

    def test_hybrid_status_text(self):
        h = {"strong": ("a.c", si.IMPLEMENTED), "weak": None,
             "labelled": {si.USERSPACE: ("a.c", si.IMPLEMENTED), si.KERNEL: ("a.c", si.STUB)}}
        row = self.classify_one(h)
        self.assertEqual(row["status"], si.hybrid_status(si.IMPLEMENTED, si.STUB))
        self.assertEqual(row["status"], "IMPLEMENTED (userspace) / STUB-NOT-IMPLEMENTED (kernel)")
        self.assertTrue(si.is_hybrid(row["status"]))
        self.assertEqual(row["file"], "a.c")

    def test_hybrid_needs_both_labels(self):
        h = {"strong": ("a.c", si.REFUSES), "weak": None,
             "labelled": {si.KERNEL: ("a.c", si.REFUSES)}}
        row = self.classify_one(h)
        self.assertEqual(row["status"], si.REFUSES)
        self.assertFalse(si.is_hybrid(row["status"]))

    def test_unreachable_only_without_wrapper(self):
        impls = {si.norm_name("NtA"): [("src/kernel/nt/mm.c", "nt_a_internal")]}
        row = self.classify_one(None, impls)
        self.assertEqual(row["status"], si.UNREACHABLE)
        self.assertEqual(row["file"], "src/kernel/nt/mm.c")

    def test_weak_wins_over_unreachable(self):
        impls = {si.norm_name("NtA"): [("src/kernel/nt/mm.c", "nt_a_internal")]}
        row = self.classify_one({"strong": None, "weak": "src/kernel/nt/w.c", "labelled": {}}, impls)
        self.assertEqual(row["status"], si.WEAK)

    def test_no_match_is_missing(self):
        row = self.classify_one(None, {})
        self.assertEqual(row["status"], si.MISSING)

    def test_summary_counts_hybrid_keys(self):
        rows = [{"status": si.hybrid_status(si.IMPLEMENTED, si.STUB), "area": "other",
                 "ssn": 1, "name": "x", "argc": 0, "file": ""}]
        counts = si.summarize(rows)
        self.assertEqual(counts[si.hybrid_status(si.IMPLEMENTED, si.STUB)], 1)
        self.assertEqual(counts[si.IMPLEMENTED], 0)
        self.assertIn(si.hybrid_status(si.IMPLEMENTED, si.STUB), si.ordered_statuses(counts))


class BranchTreeTest(unittest.TestCase):
    """Filesystem fixtures for hybrids, refusals and the unreachable-wrapper rule."""

    LIST = (
        "NT_SERVICE(0x0010, NtCreateProcessEx, 9)\n"
        "NT_SERVICE(0x0011, NtDenied, 1)\n"
        "NT_SERVICE(0x0012, NtCreateSection, 7)\n"
        "NT_SERVICE(0x0013, NtNothing, 1)\n"
    )

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        make_tree(self.root, self.LIST, {
            "proc.c": "#ifndef __KERNEL__\n"
                      "NTSTATUS nt_svc_NtCreateProcessEx(PHANDLE h)\n"
                      "{ if (!h) return STATUS_INVALID_PARAMETER; return STATUS_SUCCESS; }\n"
                      "#else /* __KERNEL__ */\n"
                      "NTSTATUS nt_svc_NtCreateProcessEx(PHANDLE a)\n"
                      "{ (void)a; return STATUS_NOT_IMPLEMENTED; }\n"
                      "#endif\n",
            "deny.c": "NTSTATUS nt_svc_NtDenied(PHANDLE a) { (void)a; return STATUS_ACCESS_DENIED; }\n",
            "mm.c": "static NTSTATUS nt_create_section(ULONG a)\n{ return STATUS_SUCCESS; }\n"
                    "#define X(a) { return 0; }\n",
        })

    def tearDown(self):
        self._tmp.cleanup()

    def test_userspace_implemented_kernel_stub_is_hybrid(self):
        rows = {r["name"]: r for r in si.run_inventory(str(self.root))}
        self.assertEqual(rows["NtCreateProcessEx"]["status"],
                         "IMPLEMENTED (userspace) / STUB-NOT-IMPLEMENTED (kernel)")
        self.assertEqual(rows["NtCreateProcessEx"]["file"], "src/kernel/nt/proc.c")

    def test_refusal_file_classified(self):
        rows = {r["name"]: r for r in si.run_inventory(str(self.root))}
        self.assertEqual(rows["NtDenied"]["status"], si.REFUSES)

    def test_section_function_without_wrapper_is_unreachable(self):
        rows = {r["name"]: r for r in si.run_inventory(str(self.root))}
        self.assertEqual(rows["NtCreateSection"]["status"], si.UNREACHABLE)
        self.assertEqual(rows["NtCreateSection"]["file"], "src/kernel/nt/mm.c")

    def test_no_implementation_stays_missing(self):
        rows = {r["name"]: r for r in si.run_inventory(str(self.root))}
        self.assertEqual(rows["NtNothing"]["status"], si.MISSING)

    def test_markdown_states_the_rules(self):
        md = si.render_markdown(si.run_inventory(str(self.root)))
        self.assertIn("### Classes", md)
        self.assertIn("UNREACHABLE-WRAPPER-MISSING", md)
        self.assertIn("| hybrid |", md)


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
            self.assertTrue(r["status"] in si.STATUSES or si.is_hybrid(r["status"]), r["status"])

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
