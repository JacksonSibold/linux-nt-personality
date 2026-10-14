"""Unit tests for tools/check_os_identity.py.

Most tests build small synthetic trees in a temporary directory so they check
the audit logic, not the live identity values. One test checks that the real
policy file parses and that every declared site file exists.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_os_identity.py"
POLICY = REPO / "tests" / "conformance" / "os-identity-policy.json"

_spec = importlib.util.spec_from_file_location("check_os_identity_under_test", TOOL)
audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(audit)


def _policy(sites, fields=None, scan_roots=None):
    return {
        "reference_build": "test",
        "fields": fields or {
            "dwBuildNumber": {"value": 26200},
            "wProductType": {"value": 1},
        },
        "sites": sites,
        "scan_roots": scan_roots or ["src"],
    }


class _Tree:
    def __init__(self, files, policy):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        for rel, text in files.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        self.policy_path = self.root / "policy.json"
        self.policy_path.write_text(json.dumps(policy), encoding="utf-8")

    def run(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = audit.main(["--root", str(self.root), "--policy", str(self.policy_path)])
        return code, buf.getvalue()

    def close(self):
        self._tmp.cleanup()


SITE_BUILD = {"file": "src/a.c", "function": "get_version", "field": "dwBuildNumber"}
SITE_TYPE = {"file": "src/a.c", "function": "get_version", "field": "wProductType"}


class FunctionSpan(unittest.TestCase):
    def test_brace_on_next_line(self):
        lines = ["static int f(int x)", "{", "    return x;", "}", "int g(void) { return 0; }"]
        self.assertEqual(audit.function_span(lines, "f"), (0, 3))

    def test_prototype_is_skipped(self):
        lines = ["int f(void);", "int f(void) {", "  return 1;", "}"]
        self.assertEqual(audit.function_span(lines, "f"), (1, 3))

    def test_missing_function_returns_none(self):
        self.assertIsNone(audit.function_span(["int g(void) {", "}"], "f"))


class SiteChecks(unittest.TestCase):
    def test_all_declared_values_match_is_clean(self):
        src = (
            "int get_version(void) {\n"
            "    info->dwBuildNumber = 26200;\n"
            "    info->wProductType = 1;\n"
            "    return 0;\n"
            "}\n"
        )
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD, SITE_TYPE]))
        try:
            code, out = t.run()
        finally:
            t.close()
        self.assertEqual(code, 0, out)
        self.assertIn("violations: 0", out)

    def test_mismatch_is_reported(self):
        src = "int get_version(void) {\n    info->dwBuildNumber = 20348;\n    info->wProductType = 1;\n}\n"
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD, SITE_TYPE]))
        try:
            code, out = t.run()
        finally:
            t.close()
        self.assertEqual(code, 1)
        self.assertIn("mismatch", out)
        self.assertIn("found=20348 declared=26200", out)

    def test_hex_literal_is_parsed(self):
        src = "int get_version(void) {\n    info->dwBuildNumber = 0x6658;\n    info->wProductType = 1;\n}\n"
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD, SITE_TYPE]))
        try:
            code, _ = t.run()
        finally:
            t.close()
        self.assertEqual(code, 0)

    def test_missing_assignment_is_reported(self):
        src = "int get_version(void) {\n    info->wProductType = 1;\n}\n"
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD, SITE_TYPE]))
        try:
            code, out = t.run()
        finally:
            t.close()
        self.assertEqual(code, 1)
        self.assertIn("missing-assignment", out)

    def test_missing_function_is_reported(self):
        src = "int other(void) {\n    return 0;\n}\n"
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD]))
        try:
            code, out = t.run()
        finally:
            t.close()
        self.assertEqual(code, 1)
        self.assertIn("missing-function", out)


class UndeclaredScan(unittest.TestCase):
    def test_assignment_outside_declared_function_is_flagged(self):
        src = (
            "int get_version(void) {\n    info->dwBuildNumber = 26200;\n    return 0;\n}\n"
            "int rogue(void) {\n    info->dwBuildNumber = 20348;\n    return 0;\n}\n"
        )
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD]))
        try:
            code, out = t.run()
        finally:
            t.close()
        self.assertEqual(code, 1)
        self.assertIn("undeclared src/a.c:6 dwBuildNumber = 20348", out)

    def test_assignment_inside_declared_function_is_not_flagged(self):
        src = "int get_version(void) {\n    info->dwBuildNumber = 26200;\n    return 0;\n}\n"
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD]))
        try:
            findings = audit.scan_undeclared(t.root, json.loads(t.policy_path.read_text(encoding="utf-8")))
        finally:
            t.close()
        self.assertEqual(findings, [])

    def test_non_identity_field_is_ignored(self):
        src = "int get_version(void) {\n    info->dwBuildNumber = 26200;\n}\nint x(void) {\n    s->other = 7;\n}\n"
        t = _Tree({"src/a.c": src}, _policy([SITE_BUILD]))
        try:
            code, _ = t.run()
        finally:
            t.close()
        self.assertEqual(code, 0)

    def test_files_outside_scan_roots_are_ignored(self):
        src = "int rogue(void) {\n    info->dwBuildNumber = 20348;\n}\n"
        t = _Tree({"tests/b.c": src}, _policy([], scan_roots=["src"]))
        try:
            code, _ = t.run()
        finally:
            t.close()
        self.assertEqual(code, 0)


class Policy(unittest.TestCase):
    def test_unreadable_policy_exits_2(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            code = audit.main(["--root", str(REPO), "--policy", str(REPO / "does-not-exist.json")])
        self.assertEqual(code, 2)

    def test_repository_policy_declares_existing_sites(self):
        policy = json.loads(POLICY.read_text(encoding="utf-8"))
        self.assertEqual(policy["schema"], "ntpers-os-identity-policy/1")
        for site in policy["sites"]:
            self.assertIn(site["field"], policy["fields"], site)
            self.assertTrue((REPO / site["file"]).is_file(), site["file"])


if __name__ == "__main__":
    unittest.main()
