"""Unit tests for tools/provenance_scan.py.

Fixtures are written to temporary directories and scanned with --root style
arguments, so the tests never change the tree. One test runs the SPDX check
on the real repository, matching the CI step.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "provenance_scan.py"

_spec = importlib.util.spec_from_file_location("provenance_scan", SCRIPT)
ps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ps)


def quiet_call(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        result = fn(*args, **kwargs)
    return result, out.getvalue()


def write(root, rel, data):
    full = pathlib.Path(root, rel)
    full.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        full.write_bytes(data)
    else:
        full.write_bytes(data.encode("utf-8"))


class ParseExpressionTests(unittest.TestCase):
    def test_well_formed(self):
        for expr in ("MIT", "GPL-2.0-only OR MIT", "GPL-2.0 WITH Linux-syscall-note",
                     "(MIT OR Apache-2.0) AND BSD-3-Clause", "LicenseRef-Acme",
                     "GPL-2.0+", "DocumentRef-doc:LicenseRef-x", "GPL-2.0 */"):
            with self.subTest(expr=expr):
                self.assertIsNone(ps.parse_expression(expr)[1])

    def test_malformed(self):
        for expr in ("", "   ", "MIT AND", "MIT OR)", "(MIT", "MIT BSD", "WITH MIT",
                     "GPL-2.0 WITH", "MIT ! BSD", "()", "MIT AND ((BSD"):
            with self.subTest(expr=expr):
                self.assertIsNotNone(ps.parse_expression(expr)[1])

    def test_comment_closer_is_stripped(self):
        self.assertEqual(ps.parse_expression("GPL-2.0 */"), ("GPL-2.0", None))


class ScanFileTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def scan(self, rel, data):
        write(self.root, rel, data)
        return ps.scan_file(self.root, rel)

    def test_kernel_style_header_is_linux_class(self):
        rec = self.scan("k.c", "/* " + ps.SPDX_MARK + " GPL-2.0 */\n/* Linux kernel */\n")
        self.assertEqual(rec["class"], ps.LINUX)
        self.assertEqual(rec["spdx"][0]["expression"], "GPL-2.0")
        self.assertEqual(rec["spdx"][0]["line"], 1)
        self.assertEqual(rec["linux_kernel_mentions"], 1)

    def test_copyright_only_is_original(self):
        rec = self.scan("o.c", "/* Copyright (c) 2021 Example */\nint x;\n")
        self.assertEqual(rec["class"], ps.ORIGINAL)
        self.assertEqual(rec["copyright"][0]["line"], 1)

    def test_no_marker_is_unknown(self):
        self.assertEqual(self.scan("u.txt", "plain\n")["class"], ps.UNKNOWN)
        self.assertEqual(self.scan("Makefile", "all:\n")["class"], ps.UNKNOWN)

    def test_notice_file(self):
        self.assertEqual(self.scan("LICENSE", "text\n")["class"], ps.NOTICE)
        self.assertEqual(self.scan("docs/COPYING.txt", "text\n")["class"], ps.NOTICE)

    def test_generated_header(self):
        rec = self.scan("g.inc", "/* AUTO-GENERATED - DO NOT EDIT */\n{ 1 },\n")
        self.assertEqual(rec["class"], ps.GENERATED)

    def test_patch_file_is_linux_class(self):
        rec = self.scan("p/x.patch", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n")
        self.assertTrue(rec["patch"])
        self.assertEqual(rec["class"], ps.LINUX)

    def test_patch_name_without_diff_headers_is_not_a_patch(self):
        rec = self.scan("q.patch", "just notes\n")
        self.assertFalse(rec["patch"])

    def test_windows_header_name_is_flagged(self):
        rec = self.scan("include/ntstatus.h", "/* status values */\n")
        self.assertTrue(rec["windows_marker"])
        self.assertEqual(rec["class"], ps.UNKNOWN)

    def test_prose_is_not_read_for_header_markers(self):
        rec = self.scan("notes.md", "Discusses the Linux kernel and Microsoft Corporation.\n")
        self.assertFalse(rec["header_kernel_style"])
        self.assertFalse(rec["windows_marker"])
        self.assertEqual(rec["class"], ps.UNKNOWN)
        self.assertEqual(rec["linux_kernel_mentions"], 1)

    def test_binary_file(self):
        rec = self.scan("blob.dat", b"\x00\x01\x02")
        self.assertTrue(rec["binary"])
        self.assertEqual(rec["class"], ps.UNKNOWN)


class ListAndCheckTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def test_walk_excludes_git_dir_and_self_report(self):
        write(self.root, "a.txt", "x\n")
        write(self.root, ".git/config", "x\n")
        write(self.root, ps.SELF_REPORT, "x\n")
        self.assertEqual(ps.list_files(self.root, use_git=False), ["a.txt"])

    def test_check_passes_on_clean_tree(self):
        write(self.root, "ok.c", "/* " + ps.SPDX_MARK + " MIT */\n")
        rc, out = quiet_call(ps.run_check, self.root, use_git=False)
        self.assertEqual(rc, 0)
        self.assertIn("0 malformed", out)

    def test_check_fails_on_malformed_tag(self):
        write(self.root, "bad.c", "/* " + ps.SPDX_MARK + " MIT AND */\n")
        rc, out = quiet_call(ps.run_check, self.root, use_git=False)
        self.assertEqual(rc, 1)
        self.assertIn("FAIL [spdx-malformed] bad.c:1:", out)

    def test_check_ignores_text_that_is_not_a_tag(self):
        write(self.root, "c.c", "/* Copyright (c) 2020 Someone */\nGPL mentioned here\n")
        rc, _ = quiet_call(ps.run_check, self.root, use_git=False)
        self.assertEqual(rc, 0)

    def test_main_check_mode_and_json_output(self):
        write(self.root, "a.c", "/* " + ps.SPDX_MARK + " MIT */\n")
        rc, _ = quiet_call(ps.main, ["--root", self.root, "--check"])
        self.assertEqual(rc, 0)
        out_path = pathlib.Path(self.root, "out.json")
        rc, _ = quiet_call(ps.main, ["--root", self.root, "--json", str(out_path)])
        self.assertEqual(rc, 0)
        self.assertIn('"class": "original"', out_path.read_text(encoding="utf-8"))

    def test_main_rejects_unknown_option(self):
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(io.StringIO()):
            ps.main(["--no-such-option"])
        self.assertEqual(cm.exception.code, 2)


class ReportTests(unittest.TestCase):
    def test_markdown_has_sections_and_escapes_pipes(self):
        records = [{
            "path": "a|b.c", "size": 3, "binary": False,
            "spdx": [{"line": 1, "expression": "MIT", "error": None}],
            "copyright": [{"line": 1, "text": "Copyright x | y"}],
            "linux_kernel_mentions": 0, "gpl_mentions": 0, "patch": False,
            "header_kernel_style": False, "windows_marker": False,
            "generated": False, "notice_file": False,
            "class": ps.ORIGINAL, "reasons": ["notice present"],
        }]
        text = ps.render_markdown(records)
        for heading in ("## Summary", "## Files for human review",
                        "## Files with notices or unknown provenance"):
            self.assertIn(heading, text)
        self.assertIn("a\\|b.c", text)
        self.assertIn("| original | 1 |", text)
        self.assertIn("Malformed SPDX tags | 0", text)

    def test_real_tree_has_no_malformed_tags(self):
        rc, out = quiet_call(ps.run_check, str(REPO))
        self.assertEqual(rc, 0, out)


class SelftestTests(unittest.TestCase):
    def test_builtin_selftest_passes(self):
        failures = ps.selftest()
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
