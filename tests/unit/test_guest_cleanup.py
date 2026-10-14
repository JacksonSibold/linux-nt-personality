"""Tests for tools/linux-env/guest-cleanup.sh.

Runs the script against a fake home directory: dry run deletes nothing, --apply
removes only object/kbuild intermediates under the allowlisted trees, protected
names survive everywhere, and non-allowlisted trees are never visited.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "linux-env" / "guest-cleanup.sh"


def _sh():
    return shutil.which("sh")


def _write(path: Path, data: bytes = b"x" * 16) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class GuestCleanupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if _sh() is None:
            raise unittest.SkipTest("no sh on PATH")

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.home = Path(self._tmp.name)
        # Allowlisted build tree: intermediates are deleted, source is kept.
        _write(self.home / "prtest" / "main" / "kernel" / "sched.o")
        _write(self.home / "prtest" / "main" / "kernel" / "sched.c")
        _write(self.home / "prtest" / "main" / "fs" / "built-in.a")
        _write(self.home / "prtest" / "main" / "fs" / ".dcache.o.d")
        _write(self.home / "prtest" / "main" / "fs" / "x.mod.c")
        _write(self.home / "prtest" / "main" / "fs" / ".tmp_vmlinux.nm-sort")
        _write(self.home / "syncfix" / "linux-src" / "drivers" / "foo.ko")
        _write(self.home / "syncfix" / "other" / "bar.ko")
        # Protected names inside an allowlisted tree must survive.
        _write(self.home / "prtest" / "bzImage")
        _write(self.home / "prtest" / ".config")
        _write(self.home / "prtest" / "run.cpio.gz")
        _write(self.home / "prtest" / "initramfs-dev.o")
        _write(self.home / "prtest" / "disk.qcow2")
        _write(self.home / "prtest" / "id_ed25519")
        _write(self.home / "prtest" / "vmlinux.o")
        # Trees outside the allowlist must never be touched.
        _write(self.home / "k32a" / "agent.o")
        _write(self.home / "main-proof" / "x.o")
        # diffwork: object files plus build/ and obj/ directories.
        _write(self.home / "diffwork" / "linux-src" / "lib" / "a.o")
        _write(self.home / "diffwork" / "linux-src" / "build" / "tool.bin")
        _write(self.home / "diffwork" / "linux-src" / "build" / "bzImage")
        _write(self.home / "diffwork" / "other" / "obj" / "y.o")
        _write(self.home / "diffwork" / "other" / "README.txt")
        # A .git directory is never descended into.
        _write(self.home / "syncfix" / "repo" / ".git" / "pack.o")

    def tearDown(self):
        self._tmp.cleanup()

    def _run(self, *args):
        return subprocess.run(
            [_sh(), str(SCRIPT), "--home", str(self.home), *args],
            capture_output=True,
            text=True,
            timeout=120,
        )

    def _exists(self, rel: str) -> bool:
        return (self.home / rel).exists()

    def test_dry_run_deletes_nothing(self):
        res = self._run()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("dry-run", res.stdout)
        self.assertIn("TOTAL", res.stdout)
        self.assertTrue(self._exists("prtest/main/kernel/sched.o"))
        self.assertTrue(self._exists("diffwork/linux-src/build/tool.bin"))

    def test_apply_end_to_end(self):
        # One --apply run (each run is slow on Windows Git Bash because of the
        # /proc scan), then every expectation is checked against that run.
        res = self._run("--apply")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("removed", res.stdout)
        # Object and kbuild intermediates in allowlisted trees are gone.
        for rel in (
            "prtest/main/kernel/sched.o",
            "prtest/main/fs/built-in.a",
            "prtest/main/fs/.dcache.o.d",
            "prtest/main/fs/x.mod.c",
            "prtest/main/fs/.tmp_vmlinux.nm-sort",
            "syncfix/other/bar.ko",
            "diffwork/linux-src/lib/a.o",
            "diffwork/other/obj/y.o",
        ):
            self.assertFalse(self._exists(rel), rel)
        # diffwork build/ and obj/ directories are removed when emptied.
        self.assertFalse(self._exists("diffwork/other/obj"))
        # Protected names survive, including inside allowlisted trees.
        for rel in (
            "prtest/bzImage",
            "prtest/.config",
            "prtest/run.cpio.gz",
            "prtest/initramfs-dev.o",
            "prtest/disk.qcow2",
            "prtest/id_ed25519",
            "prtest/vmlinux.o",
            "diffwork/linux-src/build/bzImage",
            "prtest/main/kernel/sched.c",
            "diffwork/other/README.txt",
        ):
            self.assertTrue(self._exists(rel), rel)
        # Trees outside the allowlist and .git directories are never touched.
        self.assertTrue(self._exists("k32a/agent.o"))
        # The built kernel tree syncfix/linux-src is kept whole.
        self.assertTrue(self._exists("syncfix/linux-src/drivers/foo.ko"))
        self.assertTrue(self._exists("main-proof/x.o"))
        self.assertTrue(self._exists("syncfix/repo/.git/pack.o"))

    def test_missing_home_is_an_error(self):
        res = subprocess.run(
            [_sh(), str(SCRIPT), "--home", str(self.home / "nope")],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 2)

    def test_unknown_argument_is_an_error(self):
        res = self._run("--bogus")
        self.assertEqual(res.returncode, 2)


if __name__ == "__main__":
    unittest.main()
