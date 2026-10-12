"""Tests for the conflict-marker check in tools/check_repo.py.

Each test builds a small tree in a temporary directory and runs check_file on
it, so the real repository is never judged by these tests.
"""
import importlib.util
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_repo.py"

_spec = importlib.util.spec_from_file_location("check_repo", SCRIPT)
cr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cr)

OPEN = "<<<<<<< HEAD\n"
MID = "=======\n"
CLOSE = ">>>>>>> haiku/other\n"


class ConflictMarkerTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, data):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            data = data.encode("utf-8")
        path.write_bytes(data)

    def check(self, rel):
        failures = []
        text = cr.check_file(str(self.root), rel, failures)
        return failures, text

    def markers(self, failures):
        return [f for f in failures if f[0] == "conflict-marker"]

    def test_full_conflict_block_is_flagged_per_line(self):
        self.write("Makefile", "CFLAGS := -O0\n" + OPEN + "CFLAGS += -g\n" + MID +
                   "CFLAGS += -O2\n" + CLOSE)
        failures, _ = self.check("Makefile")
        marks = self.markers(failures)
        self.assertEqual([f[2].split(":")[0] for f in marks],
                         ["line 2", "line 4", "line 6"], marks)
        self.assertEqual({f[1] for f in marks}, {"Makefile"})

    def test_lone_separator_without_open_marker_is_not_flagged(self):
        # A Markdown setext heading underline is a bare "=======" line.
        self.write("notes.md", "Title\n=======\n\nBody text.\n")
        failures, text = self.check("notes.md")
        self.assertEqual((failures, text), ([], 1))

    def test_lone_close_marker_is_flagged(self):
        self.write("stray.txt", "text\n" + CLOSE)
        failures, _ = self.check("stray.txt")
        self.assertEqual(len(self.markers(failures)), 1)

    def test_indented_or_mid_line_markers_are_not_flagged(self):
        self.write("doc.py", "  " + OPEN.rstrip("\n") + "\n" +
                   "s = '" + MID.rstrip("\n") + "'\n" +
                   "x = 1 " + CLOSE)
        failures, _ = self.check("doc.py")
        self.assertEqual(self.markers(failures), [])

    def test_crlf_conflict_block_is_flagged(self):
        data = (OPEN + MID + CLOSE).replace("\n", "\r\n").encode("ascii")
        self.write("crlf.txt", data)
        failures, _ = self.check("crlf.txt")
        marks = self.markers(failures)
        self.assertEqual(len(marks), 3, failures)
        self.assertEqual([f[2].split(":")[0] for f in marks],
                         ["line 1", "line 2", "line 3"])
        # The file is pure CRLF, so the line-ending check must stay quiet.
        self.assertEqual([f for f in failures if f[0] == "crlf-mix"], [])

    def test_crlf_separator_without_open_marker_is_not_flagged(self):
        self.write("setext.md", b"Title\r\n=======\r\n")
        failures, _ = self.check("setext.md")
        self.assertEqual(self.markers(failures), [])

    def test_binary_file_with_marker_text_is_skipped(self):
        self.write("blob.bin", b"\x00" + OPEN.encode() + MID.encode() + CLOSE.encode())
        failures, text = self.check("blob.bin")
        self.assertEqual((failures, text), ([], 0))

    def test_summary_counts_marker_failures(self):
        self.write("Makefile", OPEN + MID + CLOSE)
        rc, out, _ = self._run()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL [conflict-marker] Makefile: line 1:", out)
        self.assertIn("3 failure(s)", out)

    def _run(self):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = cr.run(str(self.root), False)
        return rc, out.getvalue(), ""


if __name__ == "__main__":
    unittest.main()
