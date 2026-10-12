"""Unit tests for tests/harness/dodrun.py (pure logic and process-state mapping).

dodrun.py is loaded by path so the harness directory need not be on sys.path.
Tests here never write into the repository: result files and reports are not
produced, and subprocess commands are either the running interpreter or
synthetic one-liners. The registry YAML path is exercised only when PyYAML is
installed; the JSON registry path needs no third-party modules.
"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "harness" / "dodrun.py"

_spec = importlib.util.spec_from_file_location("dodrun_under_test", SCRIPT)
dodrun = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dodrun)


def _py(code):
    """Command list that runs a one-line snippet with the current interpreter."""
    return [sys.executable, "-c", code]


class RunCommandStateMapping(unittest.TestCase):
    def setUp(self):
        self.env = os.environ.copy()
        self.cwd = REPO

    def test_exit_zero_is_pass(self):
        r = dodrun.run_command(_py("import sys; sys.exit(0)"), 30, self.cwd, self.env)
        self.assertEqual(r["state"], "PASS")

    def test_exit_one_is_fail(self):
        r = dodrun.run_command(_py("import sys; sys.exit(1)"), 30, self.cwd, self.env)
        self.assertEqual(r["state"], "FAIL")

    def test_exit_two_is_unimplemented(self):
        r = dodrun.run_command(_py("import sys; sys.exit(2)"), 30, self.cwd, self.env)
        self.assertEqual(r["state"], "UNIMPLEMENTED")

    def test_exit_three_is_skip(self):
        r = dodrun.run_command(_py("import sys; sys.exit(3)"), 30, self.cwd, self.env)
        self.assertEqual(r["state"], "SKIP")

    def test_exit_125_is_error(self):
        r = dodrun.run_command(_py("import sys; sys.exit(125)"), 30, self.cwd, self.env)
        self.assertEqual(r["state"], "ERROR")

    def test_timeout_is_timeout(self):
        r = dodrun.run_command(_py("import time; time.sleep(30)"), 0.5, self.cwd, self.env)
        self.assertEqual(r["state"], "TIMEOUT")
        self.assertIn("timed out", r["note"])

    def test_missing_executable_is_error_not_pass(self):
        r = dodrun.run_command(["no-such-binary-ntpers-xyz"], 5, self.cwd, self.env)
        self.assertEqual(r["state"], "ERROR")
        self.assertIsNone(r["exit_code"])


class PositiveControls(unittest.TestCase):
    def test_canaries_are_portable_and_pass(self):
        # The canaries must not depend on POSIX-only binaries (true/false/sh),
        # otherwise the harness refuses to run on a Windows host.
        ok, msg = dodrun.verify_positive_controls(os.environ.copy(), 10.0, REPO)
        self.assertTrue(ok, msg)


class RegistryAndCommands(unittest.TestCase):
    def test_get_registry_entry_dict_and_shorthand(self):
        reg = {"A": {"command": ["x"]}, "B": "y"}
        self.assertEqual(dodrun.get_registry_entry(reg, "A"), {"command": ["x"]})
        self.assertEqual(dodrun.get_registry_entry(reg, "B"), {"command": "y"})
        self.assertIsNone(dodrun.get_registry_entry(reg, "C"))

    def test_build_command_normalisation(self):
        self.assertIsNone(dodrun.build_command({}))
        self.assertIsNone(dodrun.build_command({"command": None}))
        self.assertIsNone(dodrun.build_command({"command": ""}))
        self.assertIsNone(dodrun.build_command({"command": "unimplemented"}))
        self.assertEqual(dodrun.build_command({"command": "t.sh"}), ["t.sh"])
        self.assertEqual(dodrun.build_command({"command": ["a", 1]}), ["a", "1"])
        with self.assertRaises(ValueError):
            dodrun.build_command({"command": 5})

    def test_load_registry_json_with_tests_key(self):
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "reg.json"
            p.write_text(json.dumps({"tests": {"DOD-X-A001": {"command": ["t"]}}}), encoding="utf-8")
            reg = dodrun.load_registry(p)
            self.assertEqual(reg, {"DOD-X-A001": {"command": ["t"]}})

    def test_load_registry_json_flat_mapping_and_null(self):
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "flat.json"
            p.write_text(json.dumps({"DOD-X-A002": {"command": "unimplemented"}}), encoding="utf-8")
            self.assertIn("DOD-X-A002", dodrun.load_registry(p))
            q = pathlib.Path(td) / "null.json"
            q.write_text("null", encoding="utf-8")
            self.assertEqual(dodrun.load_registry(q), {})

    def test_load_registry_rejects_list(self):
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "bad.json"
            p.write_text("[1, 2]", encoding="utf-8")
            with self.assertRaises(ValueError):
                dodrun.load_registry(p)

    def test_load_registry_yaml_when_available(self):
        if not dodrun.HAS_YAML:
            self.skipTest("PyYAML not installed; YAML registry parsing not exercised")
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "reg.yaml"
            p.write_text("tests:\n  DOD-X-A003:\n    command: [\"t\"]\n    timeout: 5\n", encoding="utf-8")
            reg = dodrun.load_registry(p)
            self.assertEqual(reg["DOD-X-A003"]["timeout"], 5)


class EvaluateCriterion(unittest.TestCase):
    def setUp(self):
        self.env = os.environ.copy()

    def _eval(self, criterion, registry):
        return dodrun.evaluate_criterion(criterion, registry, 5.0, self.env)

    def test_unregistered_is_unimplemented(self):
        r = self._eval({"id": "DOD-Q-A001", "tier": "P0", "dependency_class": "LOCAL"}, {})
        self.assertEqual(r["state"], "UNIMPLEMENTED")
        self.assertEqual(r["note"], "no test registered")

    def test_null_command_is_unimplemented(self):
        r = self._eval({"id": "DOD-Q-A002", "tier": "P0"}, {"DOD-Q-A002": {"command": None}})
        self.assertEqual(r["state"], "UNIMPLEMENTED")

    def test_explicit_skip(self):
        reg = {"DOD-Q-A003": {"command": ["x"], "skip": True, "skip_reason": "no hw"}}
        r = self._eval({"id": "DOD-Q-A003", "tier": "P1"}, reg)
        self.assertEqual(r["state"], "SKIP")
        self.assertEqual(r["note"], "no hw")

    def test_gated_without_evidence_never_runs_test(self):
        # The registered command must not run when the evidence is absent.
        reg = {"DOD-Q-B001": {"command": _py("import sys; sys.exit(0)")}}
        with mock.patch.object(dodrun, "run_command") as run:
            r = self._eval({"id": "DOD-Q-B001", "tier": "P0", "dependency_class": "EXT-GATED"}, reg)
        self.assertEqual(r["state"], "GATED")
        run.assert_not_called()

    def test_gated_with_missing_evidence_file_override(self):
        reg = {"DOD-Q-B002": {"command": ["x"], "evidence": "no/such/evidence.json"}}
        r = self._eval({"id": "DOD-Q-B002", "tier": "P0", "dependency_class": "EXT-GATED"}, reg)
        self.assertEqual(r["state"], "GATED")
        self.assertIn("evidence file missing", r["note"])

    def test_registered_passing_command(self):
        reg = {"DOD-Q-C001": {"command": _py("import sys; sys.exit(0)"), "timeout": 20}}
        r = self._eval({"id": "DOD-Q-C001", "tier": "P2"}, reg)
        self.assertEqual(r["state"], "PASS")


class RollupAndRegressions(unittest.TestCase):
    def _rec(self, cid, tier, state):
        return {"id": cid, "tier": tier, "state": state}

    def test_summarize_tier_gates(self):
        results = [
            self._rec("a", "P0", "PASS"), self._rec("b", "P0", "FAIL"),
            self._rec("c", "P1", "PASS"), self._rec("d", "P1", "PASS"),
            self._rec("e", "P3", "UNIMPLEMENTED"),
        ]
        s = dodrun.summarize(results)
        self.assertEqual(s["total"], 5)
        self.assertEqual(s["counts"]["PASS"], 3)
        self.assertFalse(s["tier_summary"]["P0"]["gate_met"])
        self.assertTrue(s["tier_summary"]["P1"]["gate_met"])
        self.assertIsNone(s["tier_summary"]["P3"]["gate"])
        self.assertTrue(s["tier_summary"]["P3"]["gate_met"])

    def test_summarize_empty_has_zero_rates(self):
        s = dodrun.summarize([])
        self.assertEqual(s["total"], 0)
        self.assertEqual(s["tier_summary"]["P0"]["pass_rate"], 0.0)
        self.assertFalse(s["tier_summary"]["P0"]["gate_met"])

    def test_regression_and_improvement_detection(self):
        with tempfile.TemporaryDirectory() as td:
            prev = pathlib.Path(td) / "results-prev.json"
            prev.write_text(json.dumps({"results": [
                {"id": "x", "state": "PASS"}, {"id": "y", "state": "UNIMPLEMENTED"},
                {"id": "z", "state": "FAIL"},
            ]}), encoding="utf-8")
            cur = [
                {"id": "x", "state": "FAIL"},
                {"id": "y", "state": "PASS"},
                {"id": "z", "state": "FAIL"},
                {"id": "new", "state": "PASS"},
            ]
            regs, imps = dodrun.detect_regressions(cur, prev)
            self.assertEqual(regs, [{"id": "x", "previous": "PASS", "current": "FAIL"}])
            self.assertEqual(imps, [{"id": "y", "previous": "UNIMPLEMENTED", "current": "PASS"}])

    def test_no_previous_run_means_no_regressions(self):
        self.assertEqual(dodrun.detect_regressions([{"id": "x", "state": "FAIL"}], None), ([], []))

    def test_find_previous_run_picks_latest_name(self):
        with tempfile.TemporaryDirectory() as td:
            d = pathlib.Path(td)
            (d / "results-20260101T000000Z.json").write_text("{}", encoding="utf-8")
            (d / "results-20260301T000000Z.json").write_text("{}", encoding="utf-8")
            self.assertEqual(dodrun.find_previous_run(d).name, "results-20260301T000000Z.json")
            self.assertIsNone(dodrun.find_previous_run(d / "missing"))


class CommandResolution(unittest.TestCase):
    def test_python3_maps_to_running_interpreter_when_absent(self):
        # On Windows hosts there is commonly no 'python3' on PATH; the registry
        # uses it, so it must fall back to the interpreter running the harness.
        with mock.patch("shutil.which", return_value=None):
            self.assertEqual(dodrun.resolve_command(["python3", "t.py"]), [sys.executable, "t.py"])

    def test_python3_kept_when_present(self):
        with mock.patch("shutil.which", return_value="/usr/bin/python3"):
            self.assertEqual(dodrun.resolve_command(["python3", "t.py"]), ["python3", "t.py"])

    def test_other_commands_untouched(self):
        with mock.patch("shutil.which", return_value=None):
            self.assertEqual(dodrun.resolve_command(["make", "test"]), ["make", "test"])


if __name__ == "__main__":
    unittest.main()
