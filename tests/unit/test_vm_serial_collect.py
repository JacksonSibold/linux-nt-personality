"""Unit tests for tools/vm_serial_collect.py and the guest emitter vm/emit-results.sh."""
import base64
import contextlib
import hashlib
import importlib.util
import io
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "vm_serial_collect.py"
EMITTER = REPO / "vm" / "emit-results.sh"

_spec = importlib.util.spec_from_file_location("vm_serial_collect", SCRIPT)
collector = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(collector)

KERNEL_LINE = "[    1.234567] binfmt_pe: loaded minimal.exe"


def encode(data: bytes, width: int = 76):
    text = base64.b64encode(data).decode("ascii")
    if not text:
        return []
    return [text[i:i + width] for i in range(0, len(text), width)]


def frame(path: str, data: bytes, size=None, sha=None, width: int = 76):
    size = len(data) if size is None else size
    sha = hashlib.sha256(data).hexdigest() if sha is None else sha
    return ["@@NTPERS-FILE %s %d %s@@" % (path, size, sha)] + encode(data, width) + ["@@END@@"]


def emit(*frames, begin=True, end=True):
    lines = []
    if begin:
        lines.append(collector.EMIT_BEGIN)
    for item in frames:
        lines.extend(item)
    if end:
        lines.append(collector.EMIT_END)
    return lines


RESULT = b"0\n"
JSON = b'{\n  "test": "m0",\n  "status": "PASS",\n  "exit_code": 42\n}\n'


class FrameVerificationTests(unittest.TestCase):
    def test_valid_files_round_trip(self):
        lines = emit(frame(".dodresult/result.exitcode", RESULT),
                     frame(".dodresult/test_m0.json", JSON))
        out = collector.collect_lines(lines)
        self.assertTrue(out.complete)
        self.assertEqual(out.errors, [])
        self.assertEqual(out.files[".dodresult/result.exitcode"], RESULT)
        self.assertEqual(out.files[".dodresult/test_m0.json"], JSON)
        self.assertEqual(out.frames, 2)

    def test_empty_file_and_all_byte_values(self):
        blob = bytes(range(256)) * 4
        lines = emit(frame(".dodresult/empty.txt", b""), frame(".dodresult/blob.bin", blob))
        out = collector.collect_lines(lines)
        self.assertEqual(out.errors, [])
        self.assertEqual(out.files[".dodresult/empty.txt"], b"")
        self.assertEqual(out.files[".dodresult/blob.bin"], blob)

    def test_crlf_log_is_accepted(self):
        lines = emit(frame(".dodresult/result.exitcode", RESULT))
        text = "\r\n".join(lines) + "\r\n"
        out = collector.collect_lines(text.splitlines())
        self.assertEqual(out.errors, [])
        self.assertEqual(out.files[".dodresult/result.exitcode"], RESULT)

    def test_corrupted_hash_is_rejected(self):
        bad = "0" * 64
        out = collector.collect_lines(emit(frame(".dodresult/result.exitcode", RESULT, sha=bad)))
        self.assertTrue(any("sha256 mismatch" in e for e in out.errors), out.errors)
        self.assertNotIn(".dodresult/result.exitcode", out.files)

    def test_corrupted_body_is_rejected(self):
        lines = frame(".dodresult/test_m0.json", JSON)
        body_index = 1
        flipped = lines[body_index]
        replacement = "B" if flipped[0] != "B" else "C"
        lines[body_index] = replacement + flipped[1:]
        out = collector.collect_lines(emit(lines))
        self.assertTrue(out.errors)
        self.assertNotIn(".dodresult/test_m0.json", out.files)

    def test_wrong_size_is_rejected(self):
        out = collector.collect_lines(emit(frame(".dodresult/result.exitcode", RESULT, size=5)))
        self.assertTrue(any("size mismatch" in e for e in out.errors), out.errors)

    def test_undecodable_body_is_rejected(self):
        lines = frame(".dodresult/result.exitcode", RESULT)
        lines[1] = lines[1] + "A"  # breaks the base64 length/padding
        out = collector.collect_lines(emit(lines))
        self.assertTrue(out.errors)
        self.assertNotIn(".dodresult/result.exitcode", out.files)

    def test_truncated_frame_before_next_header(self):
        first = frame(".dodresult/test_m0.json", JSON)[:-1]  # drop @@END@@
        out = collector.collect_lines(emit(first, frame(".dodresult/result.exitcode", RESULT)))
        self.assertTrue(any("truncated" in e for e in out.errors), out.errors)
        self.assertEqual(out.files.get(".dodresult/result.exitcode"), RESULT)
        self.assertNotIn(".dodresult/test_m0.json", out.files)

    def test_truncated_frame_at_emit_end(self):
        first = frame(".dodresult/test_m0.json", JSON)[:-1]
        out = collector.collect_lines(emit(first))
        self.assertTrue(any("truncated" in e for e in out.errors), out.errors)
        self.assertTrue(out.complete)

    def test_log_ends_inside_frame_is_incomplete(self):
        lines = emit(frame(".dodresult/result.exitcode", RESULT), end=False)
        lines += ["@@NTPERS-FILE .dodresult/test_m0.json 10 " + "a" * 64 + "@@", "QUJD"]
        out = collector.collect_lines(lines)
        self.assertFalse(out.complete)
        self.assertTrue(any("truncated" in e for e in out.errors), out.errors)


class InterleavingTests(unittest.TestCase):
    def test_kernel_lines_between_and_inside_frames_are_ignored(self):
        head = frame(".dodresult/test_m0.json", JSON)
        body_with_noise = [head[0], KERNEL_LINE] + head[1:-1] + [KERNEL_LINE, head[-1]]
        lines = emit([KERNEL_LINE], body_with_noise, [KERNEL_LINE],
                     frame(".dodresult/result.exitcode", RESULT))
        out = collector.collect_lines(lines)
        self.assertEqual(out.errors, [])
        self.assertEqual(out.files[".dodresult/test_m0.json"], JSON)
        self.assertEqual(out.files[".dodresult/result.exitcode"], RESULT)
        # Only lines inside a frame are counted as noise; the two outside frames are not.
        self.assertEqual(out.noise_lines, 2)

    def test_kernel_line_glued_to_header_is_still_found(self):
        lines = emit([KERNEL_LINE + "@@NTPERS-FILE .dodresult/result.exitcode 2 "
                      + hashlib.sha256(b"0\n").hexdigest() + "@@"] + encode(b"0\n") + ["@@END@@"])
        out = collector.collect_lines(lines)
        self.assertEqual(out.files.get(".dodresult/result.exitcode"), b"0\n")


class PathAndDuplicateTests(unittest.TestCase):
    def test_identical_duplicate_is_accepted_once(self):
        lines = emit(frame(".dodresult/result.exitcode", RESULT),
                     frame(".dodresult/result.exitcode", RESULT))
        out = collector.collect_lines(lines)
        self.assertEqual(out.errors, [])
        self.assertEqual(out.duplicates, 1)
        self.assertEqual(out.files[".dodresult/result.exitcode"], RESULT)

    def test_conflicting_duplicate_is_rejected(self):
        lines = emit(frame(".dodresult/result.exitcode", RESULT),
                     frame(".dodresult/result.exitcode", b"1\n"))
        out = collector.collect_lines(lines)
        self.assertTrue(any("conflicting duplicate" in e for e in out.errors), out.errors)

    def test_unsafe_paths_are_rejected(self):
        for path in ["/etc/passwd", ".dodresult/../../etc/passwd", "vm/test_m0.sh",
                     ".dodresult/./x", ".dodresult//x", ".dodresult\\x", "C:/x"]:
            with self.subTest(path=path):
                out = collector.collect_lines(emit(frame(path, RESULT)))
                self.assertTrue(out.errors, "accepted %r" % path)
                self.assertNotIn(path, out.files)

    def test_check_path_accepts_result_paths(self):
        self.assertIsNone(collector.check_path(".dodresult/test_m0.json", ".dodresult/"))
        self.assertIsNotNone(collector.check_path(".dodresult/a", "other/"))


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vmcollect-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.dir = pathlib.Path(self.tmp)

    def run_cli(self, lines):
        log = self.dir / "serial.log"
        log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        out = self.dir / "share"
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = collector.main(["--log", str(log), "--out", str(out)])
        return rc, out, err.getvalue()

    def test_valid_log_writes_exact_bytes(self):
        rc, out, _ = self.run_cli(emit(frame(".dodresult/test_m0.json", JSON),
                                       frame(".dodresult/result.exitcode", RESULT)))
        self.assertEqual(rc, 0)
        self.assertEqual((out / ".dodresult" / "test_m0.json").read_bytes(), JSON)
        self.assertEqual((out / ".dodresult" / "result.exitcode").read_bytes(), RESULT)

    def test_incomplete_emit_writes_nothing_and_returns_3(self):
        rc, out, _ = self.run_cli(emit(frame(".dodresult/result.exitcode", RESULT), end=False))
        self.assertEqual(rc, collector.EXIT_INCOMPLETE)
        self.assertFalse(out.exists())

    def test_verification_error_writes_nothing_and_returns_1(self):
        rc, out, err = self.run_cli(emit(frame(".dodresult/result.exitcode", RESULT),
                                         frame(".dodresult/test_m0.json", JSON, sha="0" * 64)))
        self.assertEqual(rc, collector.EXIT_VERIFY)
        self.assertFalse(out.exists())
        self.assertIn("sha256 mismatch", err)

    def test_missing_log_returns_usage_error(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = collector.main(["--log", str(self.dir / "nope.log"), "--out", str(self.dir)])
        self.assertEqual(rc, collector.EXIT_USAGE)


@unittest.skipUnless(EMITTER.exists() and shutil.which("sh") and shutil.which("base64")
                     and shutil.which("sha256sum") and shutil.which("find"),
                     "needs sh, base64, sha256sum and find on PATH")
class EmitterRoundTripTests(unittest.TestCase):
    """The real guest emitter, run by sh on the host, must round-trip through the collector."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vmemit-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.share = pathlib.Path(self.tmp) / "share"
        results = self.share / ".dodresult"
        results.mkdir(parents=True)
        (results / "result.exitcode").write_bytes(RESULT)
        (results / "test_m0.json").write_bytes(JSON)
        (results / "blob.bin").write_bytes(bytes(range(256)) * 300)
        (results / "empty.txt").write_bytes(b"")

    def test_emitter_output_is_verified_and_rebuilt(self):
        proc = subprocess.run(["sh", EMITTER.as_posix(), self.share.as_posix()],
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        log = pathlib.Path(self.tmp) / "serial.log"
        log.write_text(KERNEL_LINE + "\n" + proc.stdout + KERNEL_LINE + "\n", encoding="utf-8")
        dest = pathlib.Path(self.tmp) / "rebuilt"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = collector.main(["--log", str(log), "--out", str(dest)])
        self.assertEqual(rc, 0)
        for name in ["result.exitcode", "test_m0.json", "blob.bin", "empty.txt"]:
            with self.subTest(name=name):
                self.assertEqual((dest / ".dodresult" / name).read_bytes(),
                                 (self.share / ".dodresult" / name).read_bytes())


if __name__ == "__main__":
    unittest.main()
