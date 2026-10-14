"""Tests for vm/make-kernel-config.sh --check (the PROFILE switch).

The tests write small .config fixtures to a temporary directory and run the
check mode on them. They never touch a kernel tree. The required and forbidden
lists are read from the script itself, so the tests follow the script.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "vm" / "make-kernel-config.sh"


def _array(text, name):
    """Return the words of the bash array NAME=( ... ) in TEXT (comments removed)."""
    m = re.search(rf"^{name}=\(\n(.*?)^\)", text, re.S | re.M)
    if not m:
        raise AssertionError(f"array {name} not found in {SCRIPT.name}")
    words = []
    for line in m.group(1).splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            words.extend(w.strip('"') for w in line.split())
    return words


def _required(profile):
    text = SCRIPT.read_text(encoding="utf-8")
    words = _array(text, "REQUIRED_SHARED")
    if profile == "min":
        words += _array(text, "REQUIRED_MIN_EXTRA")
    else:
        words += _array(text, "REQUIRED_FULL_EXTRA")
    return words


@unittest.skipUnless(shutil.which("bash"), "bash is needed to run the script")
class KernelConfigCheckTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = pathlib.Path(self._tmp.name)

    def _check(self, lines, profile):
        cfg = self.dir / f"{profile}.config"
        cfg.write_text("\n".join(lines) + "\n", encoding="utf-8")
        env = {"PATH": "/usr/bin:/bin", "HOME": str(self.dir), "PROFILE": profile}
        proc = subprocess.run(
            ["bash", str(SCRIPT), "--check", str(cfg)],
            capture_output=True, text=True, env=env, timeout=60,
        )
        return proc.returncode, proc.stdout + proc.stderr

    def _complete(self, profile, extra=()):
        lines = [f"CONFIG_{s}=y" for s in _required(profile)]
        lines += list(extra)
        return lines

    def test_min_accepts_a_complete_fixture(self):
        rc, out = self._check(self._complete("min"), "min")
        self.assertEqual(rc, 0, out)
        self.assertIn("CHECK PASS (min)", out)

    def test_min_rejects_networking(self):
        rc, out = self._check(self._complete("min", ["CONFIG_NET=y"]), "min")
        self.assertEqual(rc, 1, out)
        self.assertIn("FORBIDDEN: CONFIG_NET is enabled", out)

    def test_min_rejects_modules_and_block(self):
        rc, out = self._check(
            self._complete("min", ["CONFIG_MODULES=m", "CONFIG_BLOCK=y"]), "min"
        )
        self.assertEqual(rc, 1, out)
        self.assertIn("CONFIG_MODULES", out)
        self.assertIn("CONFIG_BLOCK", out)

    def test_min_rejects_debug_info_other_than_none(self):
        rc, out = self._check(
            self._complete("min", ["CONFIG_DEBUG_INFO=y", "CONFIG_DEBUG_INFO_DWARF5=y"]),
            "min",
        )
        self.assertEqual(rc, 1, out)
        self.assertIn("debug info enabled", out)

    def test_min_rejects_a_missing_required_option(self):
        lines = [l for l in self._complete("min") if l != "CONFIG_SERIAL_8250_CONSOLE=y"]
        rc, out = self._check(lines, "min")
        self.assertEqual(rc, 1, out)
        self.assertIn("MISSING: CONFIG_SERIAL_8250_CONSOLE=y", out)

    def test_min_does_not_require_the_9p_transport(self):
        required = _required("min")
        for sym in ("VIRTIO_BLK", "NET_9P", "9P_FS", "NET"):
            self.assertNotIn(sym, required)

    def test_full_accepts_networking_and_the_9p_transport(self):
        rc, out = self._check(self._complete("full", ["CONFIG_NET=y"]), "full")
        self.assertEqual(rc, 0, out)
        self.assertIn("CHECK PASS (full)", out)

    def test_full_requires_the_9p_transport(self):
        lines = [l for l in self._complete("full") if l != "CONFIG_NET_9P=y"]
        rc, out = self._check(lines, "full")
        self.assertEqual(rc, 1, out)
        self.assertIn("MISSING: CONFIG_NET_9P=y", out)

    def test_unknown_profile_is_rejected(self):
        rc, out = self._check(self._complete("min"), "huge")
        self.assertEqual(rc, 1, out)
        self.assertIn("PROFILE must be min or full", out)


if __name__ == "__main__":
    unittest.main()
