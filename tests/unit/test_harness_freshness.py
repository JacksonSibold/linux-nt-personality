"""Unit tests for tests/harness/freshness.py.

The freshness gate delegates to vm/freshness.sh (through WSL on Windows). These
tests cover the pure path translation and the exit-code decision, with
subprocess and platform mocked so nothing runs and nothing is written.
"""
import importlib.util
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "harness" / "freshness.py"

_spec = importlib.util.spec_from_file_location("freshness_under_test", SCRIPT)
freshness = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(freshness)


class WindowsToWslPath(unittest.TestCase):
    def test_drive_path_maps_to_mnt(self):
        self.assertEqual(
            freshness.windows_to_wsl_path(r"C:\Users\Jackson\repos\ntpers-harness"),
            "/mnt/c/Users/Jackson/repos/ntpers-harness",
        )

    def test_lowercase_drive_and_forward_slashes(self):
        self.assertEqual(freshness.windows_to_wsl_path("d:/work/repo"), "/mnt/d/work/repo")

    def test_posix_path_unchanged(self):
        self.assertEqual(freshness.windows_to_wsl_path("/home/u/repo"), "/home/u/repo")

    def test_no_placeholder_remains(self):
        # The former hardcoded placeholder must never be produced for a real root.
        self.assertEqual(freshness.windows_to_wsl_path(r"C:\x\y"), "/mnt/c/x/y")


class CheckFreshnessWindowsBranch(unittest.TestCase):
    def _run(self, returncode=None, side_effect=None):
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append(cmd)
            if side_effect is not None:
                raise side_effect
            return subprocess.CompletedProcess(cmd, returncode, "", "")

        with mock.patch.object(freshness.platform, "system", return_value="Windows"), \
                mock.patch.object(freshness.subprocess, "run", side_effect=fake_run):
            result = freshness.check_freshness()
        return result, calls

    def test_fresh_returns_true_and_uses_wsl_real_root(self):
        result, calls = self._run(returncode=0)
        self.assertTrue(result)
        self.assertEqual(calls[0][:4], ["wsl", "-d", "Ubuntu", "--"])
        script_arg = calls[0][5]
        self.assertTrue(script_arg.startswith("/mnt/"), script_arg)
        self.assertTrue(script_arg.endswith("/vm/freshness.sh"), script_arg)

    def test_stale_returns_false(self):
        result, _ = self._run(returncode=1)
        self.assertFalse(result)

    def test_wsl_missing_fails_closed(self):
        result, _ = self._run(side_effect=FileNotFoundError("wsl"))
        self.assertFalse(result)


class RequireFresh(unittest.TestCase):
    def test_stale_exits_125(self):
        with mock.patch.object(freshness, "check_freshness", return_value=False):
            with self.assertRaises(SystemExit) as cm:
                freshness.require_fresh()
        self.assertEqual(cm.exception.code, 125)

    def test_fresh_returns_true(self):
        with mock.patch.object(freshness, "check_freshness", return_value=True):
            self.assertTrue(freshness.require_fresh())


if __name__ == "__main__":
    unittest.main()
