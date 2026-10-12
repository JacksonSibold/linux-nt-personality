"""Every tracked script with a shebang must be executable in the git index.

The -x guards in vm/runvm.sh and vm/kbuild.sh skip the freshness gate when
the script is not executable, so a 100644 mode silently disables the gate on
Linux. This test reads the index (not the working tree) so it checks what a
clone will get.
"""
import pathlib
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def _git(*args):
    return subprocess.run(
        ["git", *args], cwd=REPO, check=True, capture_output=True
    ).stdout


class TrackedScriptModesTest(unittest.TestCase):
    def setUp(self):
        try:
            _git("rev-parse", "--git-dir")
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("not a git checkout")

    def _scripts(self):
        # Mode and path for every tracked *.sh file, read from the index.
        out = _git("ls-files", "-s", "--", "*.sh").decode("utf-8")
        entries = []
        for line in out.splitlines():
            meta, path = line.split("\t", 1)
            mode = meta.split()[0]
            entries.append((mode, path))
        return entries

    def test_scripts_with_shebang_are_executable_in_index(self):
        entries = self._scripts()
        self.assertTrue(entries, "no tracked .sh files found")
        not_exec = []
        for mode, path in entries:
            blob = _git("cat-file", "blob", f":{path}")
            if not blob.startswith(b"#!"):
                continue
            if mode != "100755":
                not_exec.append(f"{mode} {path}")
        self.assertEqual(
            not_exec, [],
            "tracked scripts with a shebang must have mode 100755 in the index "
            "(run: git update-index --chmod=+x <file>)",
        )


if __name__ == "__main__":
    unittest.main()
