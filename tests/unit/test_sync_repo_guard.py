"""The guest sync script must refuse to run without a private GUEST_DEST.

The refusals happen before any SSH call, so these tests need no guest.
"""
import os
import shutil
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "linux-env" / "sync-repo.sh"


def _run(dest, extra_env=None):
    env = {k: v for k, v in os.environ.items() if k not in ("GUEST_DEST", "GUEST_DEST_FORCE")}
    if dest is not None:
        env["GUEST_DEST"] = dest
    if extra_env:
        env.update(extra_env)
    bash = shutil.which("bash")
    if bash is None:
        raise unittest.SkipTest("bash not on PATH")
    return subprocess.run([bash, str(SCRIPT), str(REPO)], env=env,
                          capture_output=True, text=True, timeout=60)


@unittest.skipUnless(SCRIPT.exists(), "sync-repo.sh missing")
class SyncRepoGuardTest(unittest.TestCase):
    def test_unset_is_refused(self):
        r = _run(None)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("GUEST_DEST is required", r.stderr)

    def test_shared_default_is_refused(self):
        r = _run("ntpers-research")
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("refusing GUEST_DEST=ntpers-research", r.stderr)

    def test_paths_are_refused(self):
        for bad in ("a/b", "../x", "/abs", ".hidden"):
            with self.subTest(dest=bad):
                r = _run(bad)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn("plain directory name", r.stderr)

    def test_script_has_no_bare_default(self):
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn(":-ntpers-research", text)


if __name__ == "__main__":
    unittest.main()
