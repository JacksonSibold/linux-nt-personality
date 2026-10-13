"""Unit tests for tools/vm_initrd_pack.py (newc writer, base pass-through, caps)."""
import contextlib
import gzip
import importlib.util
import io
import pathlib
import shutil
import stat
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "vm_initrd_pack.py"

_spec = importlib.util.spec_from_file_location("vm_initrd_pack", SCRIPT)
packer = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(packer)


def run_main(argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = packer.main(argv)
    return rc, out.getvalue(), err.getvalue()


def read_archive(path: pathlib.Path):
    raw = path.read_bytes()
    assert raw[:2] == b"\x1f\x8b", "output must be gzip"
    return packer.read_newc(gzip.decompress(raw))


class PackerTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="vminitrd-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        self.out = self.tmp / "out.cpio.gz"


class NewcFormatTests(PackerTestCase):
    def test_round_trip_names_of_every_alignment(self):
        entries = {}
        for i in range(1, 9):
            name = "d%s" % ("x" * i)
            entries[name] = packer.Entry(stat.S_IFREG | 0o755, bytes([i]) * i)
        blob = packer.write_newc(entries)
        self.assertEqual(len(blob) % 4, 0)
        parsed = packer.read_newc(blob)
        self.assertEqual(sorted(parsed), sorted(entries))
        for name, entry in entries.items():
            self.assertEqual(parsed[name].data, entry.data)
            self.assertEqual(parsed[name].mode, entry.mode)

    def test_output_is_deterministic(self):
        (self.src / "a.txt").write_bytes(b"hello\n")
        (self.src / "sub").mkdir()
        (self.src / "sub" / "b.txt").write_bytes(b"world\n")
        rc1, _, _ = run_main(["--add", "%s=payload" % self.src, "--out", str(self.out)])
        first = self.out.read_bytes()
        out2 = self.tmp / "again.cpio.gz"
        rc2, _, _ = run_main(["--add", "%s=payload" % self.src, "--out", str(out2)])
        self.assertEqual((rc1, rc2), (0, 0))
        self.assertEqual(first, out2.read_bytes())

    def test_gzip_header_has_no_name_or_time(self):
        (self.src / "f").write_bytes(b"x")
        run_main(["--add", "%s=f" % (self.src / "f"), "--out", str(self.out)])
        raw = self.out.read_bytes()
        self.assertEqual(raw[4:8], b"\0\0\0\0")  # mtime field zero
        self.assertEqual(raw[3] & 0x08, 0)  # FNAME not set


class TreeTests(PackerTestCase):
    def test_directory_tree_becomes_payload_with_parents_and_modes(self):
        (self.src / "vm" / "deep").mkdir(parents=True)
        (self.src / "vm" / "deep" / "t.sh").write_bytes(b"echo hi\n")
        rc, out, _ = run_main(["--add", "%s=mnt/share/vm" % (self.src / "vm"),
                               "--out", str(self.out)])
        self.assertEqual(rc, 0)
        entries = read_archive(self.out)
        self.assertIn("mnt", entries)
        self.assertIn("mnt/share", entries)
        self.assertTrue(stat.S_ISDIR(entries["mnt/share/vm/deep"].mode))
        self.assertEqual(entries["mnt/share/vm/deep/t.sh"].data, b"echo hi\n")
        self.assertEqual(stat.S_IMODE(entries["mnt/share/vm/deep/t.sh"].mode), 0o755)

    def test_crlf_is_converted_only_for_sh_files(self):
        (self.src / "s.sh").write_bytes(b"line1\r\nline2\r\n")
        (self.src / "data.txt").write_bytes(b"keep\r\n")
        run_main(["--add", "%s=out" % (self.src / "s.sh"),
                  "--add", "%s=out.txt" % (self.src / "data.txt"),
                  "--out", str(self.out)])
        entries = read_archive(self.out)
        self.assertEqual(entries["out"].data, b"line1\nline2\n")
        self.assertEqual(entries["out.txt"].data, b"keep\r\n")

    def test_symlinks_are_stored_as_links(self):
        link_target = pathlib.Path("busybox")
        (self.src / "busybox").write_bytes(b"ELF")
        run_main(["--link", "bin/sh=busybox", "--link", "bin/base64=busybox",
                  "--out", str(self.out)])
        entries = read_archive(self.out)
        self.assertTrue(stat.S_ISLNK(entries["bin/sh"].mode))
        self.assertEqual(entries["bin/sh"].data, b"busybox")
        self.assertEqual(entries["bin/base64"].data, link_target.as_posix().encode())


class BaseArchiveTests(PackerTestCase):
    def make_base(self, path, entries, gz=True):
        blob = packer.write_newc(entries)
        path.write_bytes(gzip.compress(blob) if gz else blob)

    def test_base_entries_pass_through_and_are_replaced_by_name(self):
        base = self.tmp / "base.cpio.gz"
        self.make_base(base, {
            "init": packer.Entry(stat.S_IFREG | 0o755, b"old init\n"),
            "bin/busybox": packer.Entry(stat.S_IFREG | 0o755, b"BB"),
            "tests_kernel/minimal.exe": packer.Entry(stat.S_IFREG | 0o755, b"MZ"),
        })
        new_init = self.src / "init.sh"
        new_init.write_bytes(b"#!/bin/sh\n")
        rc, out, _ = run_main(["--base", str(base), "--add", "%s=init" % new_init,
                               "--out", str(self.out)])
        self.assertEqual(rc, 0)
        self.assertIn("replaced init", out)
        entries = read_archive(self.out)
        self.assertEqual(entries["init"].data, b"#!/bin/sh\n")
        self.assertEqual(entries["bin/busybox"].data, b"BB")
        self.assertEqual(entries["tests_kernel/minimal.exe"].data, b"MZ")

    def test_uncompressed_base_is_accepted(self):
        base = self.tmp / "base.cpio"
        self.make_base(base, {"x": packer.Entry(stat.S_IFREG | 0o644, b"1")}, gz=False)
        rc, _, _ = run_main(["--base", str(base), "--link", "y=x", "--out", str(self.out)])
        self.assertEqual(rc, 0)
        entries = read_archive(self.out)
        self.assertEqual(entries["x"].data, b"1")
        self.assertTrue(stat.S_ISLNK(entries["y"].mode))

    def test_non_archive_base_is_a_usage_error(self):
        base = self.tmp / "junk.bin"
        base.write_bytes(b"not a cpio at all, definitely not")
        rc, _, err = run_main(["--base", str(base), "--link", "y=x", "--out", str(self.out)])
        self.assertEqual(rc, 2)
        self.assertFalse(self.out.exists())


class LimitTests(PackerTestCase):
    def test_payload_over_cap_writes_nothing(self):
        (self.src / "big.bin").write_bytes(b"\0" * (3 * 1024))
        rc, _, err = run_main(["--add", "%s=big.bin" % (self.src / "big.bin"),
                               "--max-kb", "2", "--out", str(self.out)])
        self.assertEqual(rc, 1)
        self.assertIn("exceeds cap", err)
        self.assertFalse(self.out.exists())

    def test_payload_at_cap_is_accepted(self):
        (self.src / "ok.bin").write_bytes(b"\0" * 2048)
        rc, _, _ = run_main(["--add", "%s=ok.bin" % (self.src / "ok.bin"),
                             "--max-kb", "2", "--out", str(self.out)])
        self.assertEqual(rc, 0)

    def test_missing_source_is_rejected(self):
        rc, _, err = run_main(["--add", "%s=x" % (self.src / "nope"), "--out", str(self.out)])
        self.assertEqual(rc, 1)
        self.assertIn("missing source", err)
        self.assertFalse(self.out.exists())

    def test_path_traversal_in_dest_is_rejected(self):
        (self.src / "f").write_bytes(b"x")
        rc, _, _ = run_main(["--add", "%s=../escape" % (self.src / "f"), "--out", str(self.out)])
        self.assertEqual(rc, 2)
        self.assertFalse(self.out.exists())

    def test_bad_pair_syntax_is_a_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                run_main(["--add", "no-equals-sign", "--out", str(self.out)])
        self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
