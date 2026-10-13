"""Unit tests for the NTPERS_GOLDEN_BUILD opt-in in tests/differential/check_golden_integrity.py.

The checker is loaded from the repository and its ROOT-derived paths are
patched to a temporary golden tree, so the real corpus is only read, never
modified. The host build is patched to 26200.9457 and the policy pin to
26200.9168, which is the situation the opt-in exists for.

Strict default (variable unset): only the pinned build is accepted, so a
host-captured golden under windows-<host build> is rejected.
Opt-in (variable set to the host build): accepted only with the directory
name, version.json oracle and manifest.json host build all matching.
"""
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "differential" / "check_golden_integrity.py"

_spec = importlib.util.spec_from_file_location("check_golden_integrity_under_test", SCRIPT)
gold = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gold)

HOST = "26200.9457"
PIN = "26200.9168"
ENV = gold.OPT_IN_ENV


class GoldenOverrideTest(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        root = pathlib.Path(self._td.name)
        self.golden_root = root / "golden"
        self.probe_dir = root / "probes"
        self.probe_dir.mkdir(parents=True)
        self.policy = root / "dualrun_policy.json"
        self.policy.write_text(json.dumps({
            "windows_golden_build": PIN,
            "windows_version": "10.0.26200",
            "probes": {"probe_ntstatus": {"volume": 2, "s1_allowed": 0, "s2_allowed": 0}},
        }), encoding="utf-8")
        self.dir = self.golden_root / f"windows-{HOST}"
        self.dir.mkdir(parents=True)
        # Completeness sentinels the checker requires (sizes only; not policy probes).
        (self.dir / "probe_path.json").write_text("x" * 100001, encoding="utf-8")
        (self.dir / "probe_path_direct.json").write_text("{}", encoding="utf-8")
        self._write_probe_ntstatus()
        self._write_version()
        self._write_manifest()
        patches = [
            mock.patch.object(gold, "GOLDEN_ROOT", self.golden_root),
            mock.patch.object(gold, "POLICY_PATH", self.policy),
            mock.patch.object(gold, "PROBE_DIR", self.probe_dir),
            mock.patch.object(gold, "get_host_build", return_value=HOST),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self._env = mock.patch.dict(os.environ, {}, clear=False)
        self._env.start()
        self.addCleanup(self._env.stop)
        os.environ.pop(ENV, None)
        self.addCleanup(self._td.cleanup)

    def _write_probe_ntstatus(self, build=HOST):
        (self.dir / "probe_ntstatus.json").write_text(json.dumps({
            "windows_build": build,
            "windows_version": "10.0.26200",
            "arch": "x64",
            "rtl_mappings": [{"a": 1}, {"a": 2}],
        }), encoding="utf-8")

    def _write_version(self, oracle="host-captured", build=HOST):
        v = {"windows_build": build, "windows_version": "10.0.26200", "arch": "x64"}
        if oracle is not None:
            v["oracle"] = oracle
        (self.dir / "version.json").write_text(json.dumps(v), encoding="utf-8")

    def _write_manifest(self, host_build=HOST, schema="ntpers-golden-manifest/1"):
        (self.dir / "manifest.json").write_text(json.dumps({
            "schema": schema,
            "host": {"build": host_build},
        }), encoding="utf-8")

    def _run(self, env_value=None):
        os.environ.pop(ENV, None)
        if env_value is not None:
            os.environ[ENV] = env_value
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = gold.check_golden_integrity()
        return rc, err.getvalue()

    # --- strict default ---------------------------------------------------

    def test_strict_default_rejects_host_captured_build(self):
        """Variable unset: the pinned build is required, so windows-<host> is not used."""
        rc, err = self._run(None)
        self.assertEqual(rc, 1)
        self.assertIn(PIN, err)

    def test_strict_default_when_variable_is_blank(self):
        """An empty variable is the same as unset."""
        rc, _ = self._run("")
        self.assertEqual(rc, 1)

    def test_strict_default_when_variable_names_pin(self):
        """Naming the pinned build itself does not enter the opt-in path."""
        rc, err = self._run(PIN)
        self.assertEqual(rc, 1)
        self.assertNotIn("opt-in dev mode", err)

    # --- opt-in path accepted --------------------------------------------

    def test_opt_in_accepts_host_captured_golden(self):
        rc, err = self._run(HOST)
        self.assertEqual(rc, 0, err)
        self.assertIn("opt-in dev mode", err)
        self.assertIn(f"policy pin stays {PIN}", err)

    # --- opt-in path rejected ---------------------------------------------

    def test_opt_in_rejects_build_that_is_not_host(self):
        rc, err = self._run("26200.9999")
        self.assertEqual(rc, 1)
        self.assertIn("not the host build", err)

    def test_opt_in_rejects_missing_directory(self):
        for f in self.dir.iterdir():
            f.unlink()
        self.dir.rmdir()
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("no directory", err)

    def test_opt_in_rejects_missing_oracle(self):
        self._write_version(oracle=None)
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("oracle host-captured", err)

    def test_opt_in_rejects_wrong_oracle(self):
        self._write_version(oracle="pinned-build")
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("oracle host-captured", err)

    def test_opt_in_rejects_manifest_without_host_build(self):
        self._write_manifest(host_build="26200.9168")
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("host.build", err)

    def test_opt_in_rejects_missing_manifest(self):
        (self.dir / "manifest.json").unlink()
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("manifest.json unreadable", err)

    def test_opt_in_rejects_manifest_without_schema(self):
        self._write_manifest(schema="other/1")
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("no ntpers-golden-manifest schema", err)

    def test_opt_in_still_enforces_completeness(self):
        """The opt-in does not relax the completeness sentinels."""
        (self.dir / "probe_path.json").unlink()
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("incomplete/invalid", err)

    def test_opt_in_still_enforces_per_probe_build(self):
        """A probe golden stamped with another build fails even under the opt-in."""
        self._write_probe_ntstatus(build=PIN)
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("windows_build", err)

    def test_opt_in_still_enforces_volume(self):
        """Policy volume must still match the golden case count under the opt-in."""
        policy = json.loads(self.policy.read_text(encoding="utf-8"))
        policy["probes"]["probe_ntstatus"]["volume"] = 5
        self.policy.write_text(json.dumps(policy), encoding="utf-8")
        rc, err = self._run(HOST)
        self.assertEqual(rc, 1)
        self.assertIn("volume", err)


if __name__ == "__main__":
    unittest.main()
