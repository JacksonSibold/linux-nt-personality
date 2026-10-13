"""Unit tests for the SKIP mechanism in tests/harness/selftest.py.

A WSL-dependent selftest must report SKIP with its reason when the Ubuntu
distro is missing, and must never turn a real assertion failure into a skip.
"""
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "harness" / "selftest.py"

_spec = importlib.util.spec_from_file_location("selftest_under_test", SCRIPT)
selftest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(selftest)


class WslSkipTest(unittest.TestCase):
    def test_non_windows_needs_no_wsl(self):
        with mock.patch("platform.system", return_value="Linux"):
            self.assertIsNone(selftest.wsl_ubuntu_check())

    def test_missing_wsl_exe_gives_reason(self):
        with mock.patch("platform.system", return_value="Windows"), \
                mock.patch.object(selftest.shutil, "which", return_value=None):
            reason = selftest.wsl_ubuntu_check()
        self.assertEqual(reason, "wsl.exe not found on PATH")

    def test_uninstalled_wsl_gives_reason(self):
        done = mock.Mock(returncode=1)
        with mock.patch("platform.system", return_value="Windows"), \
                mock.patch.object(selftest.shutil, "which", return_value="C:/Windows/System32/wsl.exe"), \
                mock.patch.object(selftest.subprocess, "run", return_value=done):
            reason = selftest.wsl_ubuntu_check()
        self.assertIn("Ubuntu unavailable", reason)
        self.assertIn("wsl exit 1", reason)

    def test_working_wsl_is_not_skipped(self):
        done = mock.Mock(returncode=0)
        with mock.patch("platform.system", return_value="Windows"), \
                mock.patch.object(selftest.shutil, "which", return_value="wsl"), \
                mock.patch.object(selftest.subprocess, "run", return_value=done):
            self.assertIsNone(selftest.wsl_ubuntu_check())
            selftest.require_wsl()  # must not raise

    def test_require_wsl_raises_skip_with_reason(self):
        with mock.patch.object(selftest, "wsl_ubuntu_check", return_value="wsl.exe not found on PATH"):
            with self.assertRaises(selftest.SkipTest) as cm:
                selftest.require_wsl()
        self.assertIn("wsl.exe not found on PATH", str(cm.exception))

    def test_run_records_skip_and_continues(self):
        def needs_wsl(tmp):
            selftest.require_wsl()
            raise AssertionError("must not reach here when WSL is missing")

        skipped = []
        with mock.patch.object(selftest, "wsl_ubuntu_check", return_value="no distro"), \
                tempfile.TemporaryDirectory() as td:
            selftest._run(needs_wsl, pathlib.Path(td), skipped)
        self.assertEqual(len(skipped), 1)
        self.assertEqual(skipped[0][0], "needs_wsl")
        self.assertIn("no distro", skipped[0][1])

    def test_run_does_not_hide_real_failure(self):
        """An AssertionError is not a skip: it propagates and fails the run."""
        def broken(tmp):
            raise AssertionError("real failure")

        skipped = []
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(AssertionError):
                selftest._run(broken, pathlib.Path(td), skipped)
        self.assertEqual(skipped, [])


if __name__ == "__main__":
    unittest.main()
