"""Unit tests for tests/harness/lint_integrity.py.

Each structural check is exercised against a synthetic tree in a temporary
directory: lint_integrity.ROOT (and POLICY) are patched to point there, so
the real repository files are only read, never modified. The golden-corpus
check shells out to a separate script and is intentionally not covered here.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "harness" / "lint_integrity.py"

_spec = importlib.util.spec_from_file_location("lint_integrity_under_test", SCRIPT)
lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lint)


def _write(path: pathlib.Path, text: str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class SyntheticTree(unittest.TestCase):
    """Base: a temporary repo root with lint's ROOT/POLICY pointed at it."""

    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._td.name)
        self.policy = self.root / "tests" / "differential" / "dualrun_policy.json"
        patches = [
            mock.patch.object(lint, "ROOT", self.root),
            mock.patch.object(lint, "POLICY", self.policy),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self._td.cleanup)
        self.guest = self.root / "tests" / "kernel" / "guest"
        self.guest.mkdir(parents=True)

    def run_check(self, fn):
        # lint's fail() prints to stderr; keep test output quiet.
        with contextlib.redirect_stderr(io.StringIO()):
            return fn()


class PolicyCheck(SyntheticTree):
    def test_missing_policy_is_violation(self):
        self.assertEqual(len(self.run_check(lint.check_policy)), 1)

    def test_invalid_policy_json_is_violation(self):
        _write(self.policy, "{not json")
        self.assertEqual(len(self.run_check(lint.check_policy)), 1)

    def test_nonzero_s1_allowed_is_violation(self):
        _write(self.policy, json.dumps({"probes": {"probe_x": {"s1_allowed": 1, "s2_allowed": 0}}}))
        v = self.run_check(lint.check_policy)
        self.assertEqual(len(v), 1)
        self.assertIn("s1_allowed=1", v[0])

    def test_nonzero_s2_allowed_is_violation(self):
        _write(self.policy, json.dumps({"probes": {"probe_x": {"s1_allowed": 0, "s2_allowed": 2}}}))
        self.assertEqual(len(self.run_check(lint.check_policy)), 1)

    def test_zero_allowances_clean(self):
        _write(self.policy, json.dumps({"probes": {"probe_x": {"s1_allowed": 0, "s2_allowed": 0}}}))
        self.assertEqual(self.run_check(lint.check_policy), [])


class MeasuredAssignment(SyntheticTree):
    def test_measured_var_from_success_is_flagged(self):
        _write(self.guest / "run_bad.sh", "NT_SYSCALLS=$SUCCESS\n")
        self.assertTrue(self.run_check(lint.check_measured_assignment))

    def test_measured_var_from_instrument_is_clean(self):
        _write(self.guest / "run_good.sh", "NT_SYSCALLS=$(grep -c do_nt_syscall /tmp/trace)\n")
        self.assertEqual(self.run_check(lint.check_measured_assignment), [])


class HardcodedStatus(SyntheticTree):
    def _check(self, body):
        _write(self.guest / "run_t.sh", body)
        return self.run_check(lint.check_hardcoded_status)

    def test_volume_literal_4096_flagged(self):
        self.assertTrue(self._check('VOLUME="4096"\n'))

    def test_fixed_missing_service_flagged(self):
        self.assertTrue(self._check('MISSING_SERVICE="OUT_OF_MEMORY"\n'))

    def test_derived_missing_service_clean(self):
        self.assertEqual(self._check('MISSING_SERVICE="$OP"\n'), [])

    def test_status_literal_flagged_but_variable_clean(self):
        self.assertTrue(self._check('echo \'{"status": "PASS"}\' > out.json\n'))
        self.assertEqual(self._check('echo "{\\"status\\": \\"$STATUS\\"}"\n'), [])


class SwallowedErrors(SyntheticTree):
    def test_or_true_on_measurement_flagged(self):
        _write(self.guest / "run_m.sh", "NT_SYSCALLS=$(collect) || true\n")
        self.assertTrue(self.run_check(lint.check_swallow))

    def test_or_true_on_setup_whitelisted(self):
        _write(self.guest / "run_s.sh", "mkdir -p /mnt/x || true\n")
        self.assertEqual(self.run_check(lint.check_swallow), [])

    def test_bare_except_pass_in_harness_flagged(self):
        h = self.root / "tests" / "harness"
        _write(h / "helper.py", "try:\n    x()\nexcept:\n    pass\n")
        self.assertTrue(self.run_check(lint.check_swallow))


class DefaultNonZero(SyntheticTree):
    def test_measured_default_one_flagged(self):
        _write(self.guest / "run_d.sh", "NT_SYSCALLS=1\n")
        self.assertTrue(self.run_check(lint.check_default_nonzero))

    def test_parameter_default_one_flagged(self):
        _write(self.guest / "run_e.sh", "X=${COUNT:-1}\n")
        self.assertTrue(self.run_check(lint.check_default_nonzero))

    def test_zero_default_clean(self):
        _write(self.guest / "run_f.sh", "NT_SYSCALLS=0\n")
        self.assertEqual(self.run_check(lint.check_default_nonzero), [])


class NormalisationGuards(SyntheticTree):
    def test_dos_device_without_volume_prefix_flagged(self):
        d = self.root / "tests" / "differential" / "dualrun.py"
        _write(d, "def f(low):\n    if 'dos_device' in low:\n        return 'S4'\n")
        self.assertTrue(self.run_check(lint.check_normalization_masks_real_diff))

    def test_missing_dualrun_is_silent(self):
        self.assertEqual(self.run_check(lint.check_normalization_masks_real_diff), [])


class EvidenceBundles(SyntheticTree):
    def _bundle(self, files, run_id="RUN1", tamper=None):
        bdir = self.root / "reports" / "evidence" / "DOD-X"
        bdir.mkdir(parents=True)
        manifest = {}
        h = hashlib.sha256()
        for name, body in files.items():
            (bdir / name).write_bytes(body)
            manifest[name] = hashlib.sha256(body).hexdigest()
            h.update(body)
        h.update(run_id.encode())
        (bdir / "hash.txt").write_text(h.hexdigest(), encoding="utf-8")
        (bdir / "evidence.json").write_text(
            json.dumps({"run_id": run_id, "manifest": manifest}), encoding="utf-8")
        (bdir / "run_id.txt").write_text(run_id, encoding="utf-8")
        if tamper:
            tamper(bdir)
        return bdir

    def test_consistent_bundle_is_clean(self):
        self._bundle({"out.json": b"{}"})
        self.assertEqual(self.run_check(lint.check_evidence_bundle_hash_covers_all_files), [])

    def test_modified_manifested_file_is_flagged(self):
        self._bundle({"out.json": b"{}"}, tamper=lambda d: (d / "out.json").write_bytes(b"{\"x\":1}"))
        self.assertTrue(self.run_check(lint.check_evidence_bundle_hash_covers_all_files))

    def test_stowaway_file_is_flagged(self):
        self._bundle({"out.json": b"{}"}, tamper=lambda d: (d / "extra.bin").write_bytes(b"z"))
        v = self.run_check(lint.check_evidence_bundle_hash_covers_all_files)
        self.assertTrue(any("untracked file extra.bin" in m for m in v), v)

    def test_run_id_mismatch_is_flagged(self):
        self._bundle({"out.json": b"{}"}, tamper=lambda d: (d / "run_id.txt").write_text("OTHER", encoding="utf-8"))
        self.assertTrue(self.run_check(lint.check_evidence_bundle_hash_covers_all_files))


class FileMissingIsError(SyntheticTree):
    def test_missing_treated_as_unimplemented_flagged(self):
        w = self.root / "tests" / "harness" / "dualrun_wrapper.py"
        _write(w, "\n".join([
            'if not p.exists():',
            '    print("ERROR: golden file missing")',
            '    return 2',
        ]) + "\n")
        self.assertTrue(self.run_check(lint.check_file_missing_is_error))

    def test_missing_with_return_125_clean(self):
        w = self.root / "tests" / "harness" / "dualrun_wrapper.py"
        _write(w, "\n".join([
            'if not p.exists():',
            '    print("ERROR: golden file missing")',
            '    return 125',
        ]) + "\n")
        self.assertEqual(self.run_check(lint.check_file_missing_is_error), [])


if __name__ == "__main__":
    unittest.main()
