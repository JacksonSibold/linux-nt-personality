"""Unit tests for tests/harness/_paths.py (repository-root resolution).

These cover the environment override, the checkout-relative default and the
WSL translation. They do not run WSL or touch the filesystem outside the repo.
"""
import importlib.util
import os
import pathlib
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "harness" / "_paths.py"

_spec = importlib.util.spec_from_file_location("paths_under_test", SCRIPT)
paths = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(paths)


class WindowsToWslPath(unittest.TestCase):
    def test_drive_path_maps_to_mnt(self):
        self.assertEqual(
            paths.windows_to_wsl_path(r"C:\Users\Jackson\repos\ntpers-placeholders"),
            "/mnt/c/Users/Jackson/repos/ntpers-placeholders",
        )

    def test_forward_slash_drive_path(self):
        self.assertEqual(paths.windows_to_wsl_path("D:/work/tree"), "/mnt/d/work/tree")

    def test_posix_path_unchanged(self):
        self.assertEqual(
            paths.windows_to_wsl_path("/path/to/ntpers-research"),
            "/path/to/ntpers-research",
        )

    def test_backslashes_in_posix_path_normalised(self):
        self.assertEqual(paths.windows_to_wsl_path(r"/a\b/c"), "/a/b/c")


class RepoRoot(unittest.TestCase):
    def test_env_var_overrides_default(self):
        with mock.patch.dict(os.environ, {"NTPERS_ROOT": "/path/to/ntpers-research"}):
            self.assertEqual(paths.repo_root(), "/path/to/ntpers-research")

    def test_unset_defaults_to_checkout(self):
        env = {k: v for k, v in os.environ.items() if k != "NTPERS_ROOT"}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(paths.repo_root(), str(REPO))

    def test_empty_env_var_defaults_to_checkout(self):
        with mock.patch.dict(os.environ, {"NTPERS_ROOT": ""}):
            self.assertEqual(paths.repo_root(), str(REPO))


class WslRepoRoot(unittest.TestCase):
    def test_old_placeholder_value_is_identical(self):
        # With NTPERS_ROOT set to the value the hardcoded sites used, nothing changes.
        with mock.patch.dict(os.environ, {"NTPERS_ROOT": "/path/to/ntpers-research"}):
            self.assertEqual(paths.wsl_repo_root(), "/path/to/ntpers-research")

    def test_windows_env_value_converted(self):
        with mock.patch.dict(os.environ, {"NTPERS_ROOT": r"C:\src\ntpers"}):
            self.assertEqual(paths.wsl_repo_root(), "/mnt/c/src/ntpers")

    def test_default_is_checkout_as_wsl_path(self):
        env = {k: v for k, v in os.environ.items() if k != "NTPERS_ROOT"}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(paths.wsl_repo_root(), paths.windows_to_wsl_path(str(REPO)))


if __name__ == "__main__":
    unittest.main()
