"""Unit tests for the accepted-builds golden policy (tests/differential/golden/ACCEPTED_BUILDS.json).

The checker is loaded from the repository. Its paths are patched to a temporary golden tree, and
the host build is patched, so the real corpus is only read, never modified. The real accepted list
is also checked for self-consistency against the committed corpus.
"""
import contextlib
import gzip
import hashlib
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
PROBES = {
    "probe_ntstatus": {"volume": 2, "s1_allowed": 0, "s2_allowed": 0},
    "probe_teb_peb": {"volume": 1, "s1_allowed": 0, "s2_allowed": 0},
    "probe_gone": {"volume": 1, "s1_allowed": 0, "s2_allowed": 0},
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def probe_payload(probe: str, build: str = HOST) -> bytes:
    body = {
        "windows_build": build,
        "windows_version": "10.0.26200",
        "arch": "x64",
    }
    if probe == "probe_ntstatus":
        body["rtl_mappings"] = [{"ntstatus": 0}, {"ntstatus": 1}]
    else:
        body["gs_checks"] = [{"match": True}]
    return json.dumps(body, indent=2).encode("utf-8")


class AcceptedBuildsTest(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        root = pathlib.Path(self._td.name)
        self.golden_root = root / "golden"
        self.golden_root.mkdir()
        self.dir = self.golden_root / "windows-26200.9457"
        self.dir.mkdir()
        self.probe_dir = root / "probes"
        self.probe_dir.mkdir()
        self.out_run = root / "golden-out" / "run"
        self.out_run.mkdir(parents=True)
        self.policy = root / "dualrun_policy.json"
        self.policy.write_text(json.dumps({"probes": PROBES}), encoding="utf-8")
        self.accepted_path = self.golden_root / "ACCEPTED_BUILDS.json"
        self.gaps = {"probe_gone": {"reason": "too-large-kept-out", "note": "absent for test"}}
        self.raw_probe = {}
        self.manifest = {"schema": "ntpers-golden-manifest/1", "host": {"build": HOST},
                         "probes_run": []}
        for probe in ("probe_ntstatus", "probe_teb_peb"):
            payload = probe_payload(probe)
            self.raw_probe[probe] = payload
            (self.dir / f"{probe}.json").write_bytes(payload)
            self.manifest["probes_run"].append({"probe": probe, "sha256_published": sha(payload),
                                                "stdout_sha256_raw": sha(payload),
                                                "stdout_bytes_raw": len(payload)})
        self.version = {"windows_build": HOST, "oracle": "host-captured", "arch": "x64"}
        self.write_meta()
        patches = [
            mock.patch.object(gold, "GOLDEN_ROOT", self.golden_root),
            mock.patch.object(gold, "ACCEPTED_PATH", self.accepted_path),
            mock.patch.object(gold, "POLICY_PATH", self.policy),
            mock.patch.object(gold, "PROBE_DIR", self.probe_dir),
            mock.patch.object(gold, "GOLDEN_OUT_RUN", self.out_run),
            mock.patch.object(gold, "get_host_build", return_value=HOST),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self._td.cleanup)
        os.environ.pop("NTPERS_GOLDEN_BUILD", None)

    # --- fixture helpers ---------------------------------------------------

    def write_meta(self, entry_overrides=None):
        (self.dir / "version.json").write_text(json.dumps(self.version), encoding="utf-8")
        (self.dir / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        entry = {
            "build": HOST,
            "directory": "windows-26200.9457",
            "provenance": "host-captured",
            "captured_on": "2026-10-13",
            "accepted_on": "2026-10-13",
            "version_json_sha256": sha((self.dir / "version.json").read_bytes()),
            "manifest_sha256": sha((self.dir / "manifest.json").read_bytes()),
            "rationale": "test fixture",
            "known_gaps": self.gaps,
        }
        entry.update(entry_overrides or {})
        self.accepted_path.write_text(json.dumps({
            "schema": "ntpers-accepted-golden-builds/1", "accepted": [entry]}), encoding="utf-8")

    def run_check(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = gold.check_golden_integrity()
        return rc, out.getvalue() + err.getvalue()

    def add_probe_summary(self, probe, **overrides):
        rec = next(r for r in self.manifest["probes_run"] if r["probe"] == probe) if any(
            r["probe"] == probe for r in self.manifest["probes_run"]) else None
        if rec is None:
            rec = {"probe": probe, "stdout_sha256_raw": sha(b"summary source"),
                   "stdout_bytes_raw": 14}
            self.manifest["probes_run"].append(rec)
        summary = {"schema": gold.SUMMARY_SCHEMA, "probe": probe, "build": HOST,
                   "size_bytes": rec["stdout_bytes_raw"], "sha256": rec["stdout_sha256_raw"],
                   "records": 3, "record_digest": sha(b"sorted records"),
                   "regenerate": "golden-out/publish_golden.py"}
        summary.update(overrides)
        for key in [k for k, v in overrides.items() if v is None]:
            summary.pop(key, None)
        (self.dir / f"{probe}.json").write_text(json.dumps(summary), encoding="utf-8")
        self.write_meta_keep_gaps()

    def write_meta_keep_gaps(self):
        # Re-pin the manifest after changing it so only the intended check fails.
        self.write_meta()

    # --- accepted-list behaviour -------------------------------------------

    def test_accepted_build_passes(self):
        rc, text = self.run_check()
        self.assertEqual(rc, 0, text)
        self.assertIn("accepted build 26200.9457", text)

    def test_unlisted_host_build_fails(self):
        with mock.patch.object(gold, "get_host_build", return_value="26200.9168"):
            rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("host build 26200.9168 is not in the accepted builds list", text)

    def test_env_var_does_not_bypass_the_list(self):
        with mock.patch.dict(os.environ, {"NTPERS_GOLDEN_BUILD": "26200.9168"}):
            with mock.patch.object(gold, "get_host_build", return_value="26200.9168"):
                rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("not in the accepted builds list", text)

    def test_manifest_hash_mismatch_fails(self):
        self.write_meta()  # pins the current hashes
        tampered = json.loads((self.dir / "manifest.json").read_text(encoding="utf-8"))
        tampered["captured_utc"] = "2026-10-14T00:00:00Z"
        (self.dir / "manifest.json").write_text(json.dumps(tampered), encoding="utf-8")
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("manifest.json sha256 does not match", text)

    def test_version_hash_mismatch_fails(self):
        self.version["note"] = "edited after acceptance"
        (self.dir / "version.json").write_text(json.dumps(self.version), encoding="utf-8")
        # Pin the manifest hash only; the version hash in the list is now stale.
        entry = json.loads(self.accepted_path.read_text(encoding="utf-8"))
        entry["accepted"][0]["version_json_sha256"] = "0" * 64
        self.accepted_path.write_text(json.dumps(entry), encoding="utf-8")
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("version.json sha256 does not match", text)

    def test_provenance_mismatch_fails(self):
        self.write_meta({"provenance": "pinned-reference"})
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("provenance mismatch", text)

    def test_schema_rejects_bad_entries(self):
        for override, needle in (
            ({"provenance": "unknown"}, "provenance must be one of"),
            ({"manifest_sha256": "abc"}, "manifest_sha256 must be a 64-digit"),
            ({"rationale": "  "}, "rationale must be a non-empty string"),
            ({"known_gaps": {"probe_not_policy": {"reason": "too-large-kept-out", "note": "x"}}},
             "is not a policy probe"),
            ({"known_gaps": {"probe_gone": {"reason": "because", "note": "x"}}}, "needs a reason"),
            ({"known_gaps": {"probe_gone": {"reason": "too-large-kept-out", "note": ""}}}, "needs a note"),
        ):
            with self.subTest(override=override):
                self.write_meta(override)
                rc, text = self.run_check()
                self.assertEqual(rc, 1)
                self.assertIn(needle, text)

    def test_duplicate_build_rejected(self):
        self.write_meta()
        data = json.loads(self.accepted_path.read_text(encoding="utf-8"))
        data["accepted"].append(dict(data["accepted"][0]))
        self.accepted_path.write_text(json.dumps(data), encoding="utf-8")
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("duplicate accepted build", text)

    # --- completeness and known gaps ---------------------------------------

    def test_known_gap_respected(self):
        rc, text = self.run_check()
        self.assertEqual(rc, 0, text)
        self.assertIn("KNOWN GAP probe_gone [too-large-kept-out]", text)

    def test_missing_golden_not_in_gaps_fails(self):
        self.gaps = {}
        self.write_meta()
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("probe_gone: missing golden", text)

    def test_gap_with_present_golden_fails(self):
        (self.dir / "probe_gone.json").write_bytes(probe_payload("probe_teb_peb"))
        self.write_meta()
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("listed in known_gaps but a golden is present", text)

    def test_blocked_criteria_reported_for_gap_probe(self):
        registry = {"tests": {
            "DOD-DEP-A001": {"command": ["true"], "requires_golden": ["probe_gone"]},
            "DOD-DEP-A002": {"command": ["true"], "requires_golden": ["probe_ntstatus"]},
            "DOD-DEP-A003": {"command": ["true"]},
        }}
        reqs = gold.golden_requirements(registry)
        self.assertEqual(reqs, {"DOD-DEP-A001": ["probe_gone"], "DOD-DEP-A002": ["probe_ntstatus"]})
        gaps = gold.known_gaps_for_build(HOST)
        blocked = gold.blocked_criteria(reqs, gaps)
        self.assertEqual(blocked, {"DOD-DEP-A001": [("probe_gone", "too-large-kept-out")]})

    # --- summaries and gzip sentinels --------------------------------------

    def test_valid_summary_accepted(self):
        # probe_gone becomes present as a summary; it is no longer a gap.
        self.gaps = {}
        self.add_probe_summary("probe_gone", sha256=sha(b"summary source"),
                               size_bytes=14)
        rc, text = self.run_check()
        self.assertEqual(rc, 0, text)

    def test_summary_wrong_hash_rejected(self):
        self.gaps = {}
        self.add_probe_summary("probe_gone", sha256="f" * 64)
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("summary sha256 does not match the manifest", text)

    def test_summary_missing_field_rejected(self):
        self.gaps = {}
        self.add_probe_summary("probe_gone", records=None)
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("summary missing fields records", text)

    def test_empty_placeholder_rejected(self):
        self.gaps = {}
        self.write_meta()
        (self.dir / "probe_gone.json").write_text("{}", encoding="utf-8")
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("empty or placeholder golden", text)

    def test_summary_checked_against_full_output_when_local(self):
        self.gaps = {}
        self.add_probe_summary("probe_gone", sha256=sha(b"summary source"), size_bytes=14)
        (self.out_run / "probe_gone.json").write_bytes(b"different output")
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("full output present locally", text)

    def test_gzip_golden_verified_by_manifest_hash(self):
        self.gaps = {}
        payload = probe_payload("probe_gone")
        (self.dir / "probe_gone.json.gz").write_bytes(gzip.compress(payload, mtime=0))
        self.manifest["probes_run"].append({"probe": "probe_gone", "sha256_published": sha(payload),
                                            "stdout_sha256_raw": sha(payload),
                                            "stdout_bytes_raw": len(payload)})
        self.write_meta()
        rc, text = self.run_check()
        self.assertEqual(rc, 0, text)

    def test_gzip_golden_with_wrong_hash_rejected(self):
        self.gaps = {}
        payload = probe_payload("probe_gone")
        (self.dir / "probe_gone.json.gz").write_bytes(gzip.compress(payload, mtime=0))
        self.manifest["probes_run"].append({"probe": "probe_gone", "sha256_published": "0" * 64})
        self.write_meta()
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("decompressed golden sha256 does not match", text)

    def test_ghost_gap_with_summary_but_no_manifest_record_fails(self):
        self.gaps = {}
        self.write_meta()
        summary = {"schema": gold.SUMMARY_SCHEMA, "probe": "probe_gone", "build": HOST,
                   "size_bytes": 10, "sha256": "a" * 64, "records": 1,
                   "record_digest": "b" * 64, "regenerate": "x"}
        (self.dir / "probe_gone.json").write_text(json.dumps(summary), encoding="utf-8")
        rc, text = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("no manifest record with stdout_sha256_raw", text)


class ShippedAcceptedListTest(unittest.TestCase):
    """The committed accepted list must be self-consistent with the committed corpus."""

    def test_shipped_list_validates_and_matches_corpus(self):
        policy = json.loads((REPO / "tests/differential/dualrun_policy.json").read_text(encoding="utf-8-sig"))
        accepted, errs = gold.load_accepted(policy["probes"])
        self.assertEqual(errs, [])
        self.assertIsNotNone(accepted)
        for entry in accepted["accepted"]:
            d, verr = gold.verify_accepted_dir(entry)
            self.assertEqual(verr, [], entry["build"])
            self.assertIsNotNone(d)

    def test_shipped_gaps_are_policy_probes_with_absent_goldens(self):
        policy = json.loads((REPO / "tests/differential/dualrun_policy.json").read_text(encoding="utf-8-sig"))
        accepted, _ = gold.load_accepted(policy["probes"])
        for entry in accepted["accepted"]:
            d = REPO / "tests/differential/golden" / entry["directory"]
            for probe in entry.get("known_gaps", {}):
                self.assertIn(probe, policy["probes"])
                self.assertFalse((d / f"{probe}.json").exists(), probe)
                self.assertFalse((d / f"{probe}.json.gz").exists(), probe)


class DodrunBlockedOnGoldenTest(unittest.TestCase):
    """dodrun reports a golden-dependent criterion BLOCKED-ON-GOLDEN and never runs it."""

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("dodrun_under_test", REPO / "tests" / "harness" / "dodrun.py")
        cls.dodrun = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.dodrun)

    def test_dependent_criterion_reported_blocked_and_not_run(self):
        import sys
        marker = pathlib.Path(tempfile.mkdtemp()) / "ran.txt"
        # A command that would write the marker if it ran. It must not run when blocked.
        cmd = [sys.executable, "-c", f"open({str(marker)!r}, 'w').write('ran')"]
        registry = {"DOD-DEP-A001": {"command": cmd, "requires_golden": ["probe_gone"]}}
        criterion = {"id": "DOD-DEP-A001", "tier": "P1", "dependency_class": "SELF"}
        blocked = {"DOD-DEP-A001": [("probe_gone", "too-large-kept-out")]}
        result = self.dodrun.evaluate_criterion(criterion, registry, 10.0, os.environ.copy(), blocked=blocked)
        self.assertEqual(result["state"], "BLOCKED-ON-GOLDEN")
        self.assertIn("probe_gone [too-large-kept-out]", result["note"])
        self.assertFalse(marker.exists())

    def test_unblocked_criterion_still_runs(self):
        import sys
        registry = {"DOD-DEP-A002": {"command": [sys.executable, "-c", "pass"]}}
        criterion = {"id": "DOD-DEP-A002", "tier": "P1", "dependency_class": "SELF"}
        result = self.dodrun.evaluate_criterion(criterion, registry, 10.0, os.environ.copy(), blocked={})
        self.assertEqual(result["state"], "PASS")

    def test_blocked_state_is_counted_but_never_passes(self):
        results = [{"id": "X", "tier": "P0", "state": "BLOCKED-ON-GOLDEN"}]
        summary = self.dodrun.summarize(results)
        self.assertEqual(summary["counts"]["BLOCKED-ON-GOLDEN"], 1)
        self.assertEqual(summary["tier_summary"]["P0"]["passed"], 0)
        self.assertFalse(summary["tier_summary"]["P0"]["gate_met"])


if __name__ == "__main__":
    unittest.main()
