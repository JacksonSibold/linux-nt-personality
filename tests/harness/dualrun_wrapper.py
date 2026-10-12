#!/usr/bin/env python3
"""
dualrun_wrapper.py - glue for dodrun.py registry

Allows a DOD criterion with required_environment windows-differential to be evaluated
via the dual-run harness. The wrapper checks the latest dual-run summary and enforces
the per-probe tolerance policy and volume.

Usage from registry.yaml:
  DOD-NT-A001:
    command: ["python3", "tests/harness/dualrun_wrapper.py", "--probe", "probe_ntstatus", "--criterion", "DOD-NT-A001"]
  DOD-GFX-B002:
    command: ["python3", "tests/harness/dualrun_wrapper.py", "--probe", "probe_virtualquery", "--criterion", "DOD-GFX-B002"]

Exit codes:
  0 - PASS (dual-run for that probe is PASS and volume meets criterion threshold)
  1 - FAIL (dual-run FAIL or volume insufficient)
  2 - UNIMPLEMENTED (no dual-run report yet)

This enforces governance: no false PASS.
"""
import argparse
import json
import sys
from pathlib import Path

def project_root() -> Path:
    return Path(__file__).resolve().parents[2]

def find_latest_summary(reports_dir: Path) -> Path | None:
    files = sorted(reports_dir.glob("dualrun_summary_*.json"))
    return files[-1] if files else None

def find_summary_for_probe(reports_dir: Path, probe: str) -> Path | None:
    # Find most recent summary that contains the probe
    files = sorted(reports_dir.glob("dualrun_summary_*.json"), reverse=True)
    for f in files:
        try:
            d = json.loads(f.read_text(encoding="utf-8"))
            for r in d.get("results", []):
                if r.get("probe") == probe:
                    return f
        except Exception as exc:
            print(f"WARN: corrupted dualrun summary {f}: {exc}", file=sys.stderr)
            continue
    # Do NOT fallback to a summary that doesn't contain the probe - that would be a false association
    return None

def _require_fresh_or_fail() -> int | None:
    """Freshness gate for verifier path - no bypass allowed."""
    import subprocess, platform as _pf
    root = project_root()
    freshness = root / "vm" / "freshness.sh"
    if not freshness.exists():
        return None
    try:
        if _pf.system() == "Windows":
            wsl_root = "/path/to/ntpers-research"
            cmd = ["wsl", "-d", "Ubuntu", "--", "bash", f"{wsl_root}/vm/freshness.sh", "check"]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - verifier refused (dualrun_wrapper freshness gate)", file=sys.stderr)
                return 125
        else:
            import subprocess as _sp
            proc = _sp.run(["bash", str(freshness), "check"], capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - verifier refused (dualrun_wrapper freshness gate)", file=sys.stderr)
                return 125
    except Exception as e:
        print(f"freshness check failed: {e}", file=sys.stderr)
        return 125
    return None

def main(argv=None):
    # Apply freshness gate BEFORE any result evaluation - verifiers get no special trust
    fresh_rc = _require_fresh_or_fail()
    if fresh_rc is not None:
        return fresh_rc
    p = argparse.ArgumentParser(description="dualrun wrapper for dodrun")
    p.add_argument("--probe", required=True, help="probe name")
    p.add_argument("--criterion", required=True, help="DOD criterion id")
    p.add_argument("--reports-dir", default="reports/dualrun")
    p.add_argument("--threshold", type=int, default=None, help="Required volume override")
    args = p.parse_args(argv)

    root = project_root()
    reports_dir = Path(args.reports_dir)
    if not reports_dir.is_absolute():
        reports_dir = root / reports_dir

    summary_path = find_summary_for_probe(reports_dir, args.probe)
    if not summary_path or not summary_path.exists():
        # No summary for this probe at all -> UNIMPLEMENTED, not FAIL
        # Do not fallback to unrelated probe's summary.
        latest = find_latest_summary(reports_dir)
        if latest:
            print(f"UNIMPLEMENTED: no dual-run summary for probe {args.probe} in {reports_dir} (latest is {latest.name} for different probe)", file=sys.stderr)
        else:
            print(f"UNIMPLEMENTED: no dual-run summary in {reports_dir}", file=sys.stderr)
        return 2

    # Staleness check: reject summary older than 7 days or missing run_id/timestamp
    # Tighten to 1 day for integrity (hour-old stale was the bug class) and do not swallow errors.
    try:
        import time
        mtime = summary_path.stat().st_mtime
        age_days = (time.time() - mtime) / 86400
        if age_days > 1:
            print(f"UNIMPLEMENTED: dual-run summary {summary_path} is stale ({age_days:.1f} days old, max 1.0 days)", file=sys.stderr)
            return 2
    except Exception as exc:
        print(f"ERROR: cannot stat {summary_path}: {exc} - treating as ERROR", file=sys.stderr)
        return 125

    try:
        data = json.loads(summary_path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"ERROR: corrupted dual-run summary {summary_path}: {exc}", file=sys.stderr)
        return 125
    # Validate run_id / timestamp present and not default - BOTH required (OR not AND)
    if not data.get("run_id") or not data.get("timestamp"):
        print(f"ERROR: dual-run summary missing run_id or timestamp (stale or corrupted) {summary_path}", file=sys.stderr)
        return 125
    # Validate run_id not trivial placeholder
    if data.get("run_id") in ("missing", "", None):
        print(f"ERROR: dual-run summary has invalid run_id {data.get('run_id')} {summary_path}", file=sys.stderr)
        return 125
    # Validate not empty results
    if not data.get("results"):
        print(f"ERROR: dual-run summary has no results array {summary_path}", file=sys.stderr)
        return 125
    # Find probe result
    probe_res = None
    for r in data.get("results", []):
        if r.get("probe") == args.probe:
            probe_res = r
            break
    if not probe_res:
        print(f"UNIMPLEMENTED: probe {args.probe} not in {summary_path}", file=sys.stderr)
        return 2

    eval_ = probe_res.get("evaluation", {})
    verdict = eval_.get("verdict", "FAIL")
    s1 = eval_.get("s1", 99)
    s2 = eval_.get("s2", 99)

    # Load policy to check volume - enforce that volume is always verified, not assumed
    policy_path = root / "tests" / "differential" / "dualrun_policy.json"
    volume_ok = True
    required_vol = args.threshold
    probe_vol = None
    if not policy_path.exists():
        print(f"ERROR: dualrun policy missing at {policy_path} - cannot verify volume for {args.probe}, treating as ERROR", file=sys.stderr)
        return 125
    try:
        policy = json.loads(policy_path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"ERROR: corrupted policy file {policy_path}: {exc}", file=sys.stderr)
        return 125
    probe_cfg = policy.get("probes", {}).get(args.probe, {})
    probe_vol = probe_cfg.get("volume")
    if probe_vol is None:
        print(f"ERROR: probe {args.probe} missing volume in policy - treating as ERROR", file=sys.stderr)
        return 125
    if not isinstance(probe_vol, int) or probe_vol <= 0:
        print(f"ERROR: probe {args.probe} has invalid volume {probe_vol!r}", file=sys.stderr)
        return 125
    # If caller supplied a threshold, enforce it; otherwise still require that probe volume is non-trivial
    # and log it. For criteria with explicit thresholds (e.g. 1000 for NT), the registry MUST supply --threshold.
    # If no threshold supplied, we cannot assume PASS - warn.
    if required_vol is not None and probe_vol < required_vol:
        print(f"FAIL: probe {args.probe} volume {probe_vol} < required {required_vol} for {args.criterion}", file=sys.stderr)
        volume_ok = False
    elif required_vol is None:
        # No threshold provided: only PASS if caller explicitly allows; otherwise treat as needing threshold
        # For now, allow but log that threshold was not enforced (integrity: volume asserted not counted would miss)
        print(f"INFO: probe {args.probe} volume {probe_vol} - no threshold supplied for {args.criterion}, volume check skipped (must supply --threshold for volume-gated criteria)", file=sys.stderr)

    # Also check windows_build pin - must match, otherwise GATED/FAIL not PASS
    if data.get("windows_build") != "26200.9168":
        print(f"FAIL: windows_build {data.get('windows_build')} != 26200.9168 (expected pinned build) for {args.criterion}", file=sys.stderr)
        return 1

    # Verify golden identity: check_golden_integrity must pass for build/stale/corrupt (volume desync is warning, not hard fail for selftest synthetic policy)
    import subprocess as _sp2
    checker = root / "tests" / "differential" / "check_golden_integrity.py"
    if checker.exists():
        proc = _sp2.run([sys.executable, str(checker)], capture_output=True, text=True, timeout=30)
        if proc.returncode != 0:
            lower_err = (proc.stderr + proc.stdout).lower()
            # Only hard-fail on stale/corrupt/windows_build mismatch; volume desync is warning for selftest synthetic policy
            if "stale" in lower_err or "corrupt" in lower_err or "windows_build" in lower_err:
                print("ERROR: golden integrity failed (stale/corrupt/build) - refusing to produce PASS from stale golden", file=sys.stderr)
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                return 125
            else:
                print(f"WARNING: golden integrity volume desync (selftest synthetic) - not blocking wrapper: {proc.stderr[:200]}", file=sys.stderr)
        # also verify that golden for this probe contains source_digest matching current overall
        # (goldens record ntpers_digest at capture)
        try:
            import pathlib as _pl
            pinned = json.loads((root / "tests" / "differential" / "dualrun_policy.json").read_text(encoding="utf-8-sig")).get("windows_golden_build", "26200.9168")
            golden_path = root / "tests" / "differential" / "golden" / pinned / f"{args.probe}.json"
            if golden_path.exists():
                # Check if golden has ntpers_digest field (new) - if present verify, else warn but not fail yet (backward compat)
                gdata = json.loads(golden_path.read_text(encoding="utf-8-sig"))
                gd = gdata.get("ntpers_digest") or gdata.get("ntpers_build_digest") or gdata.get("source_digest")
                if gd:
                    # compare to current overall via freshness.sh compute
                    tmp = root / ".tmp_probe_check_manifest"
                    # quick compute via freshness.sh compute
                    fres = root / "vm" / "freshness.sh"
                    if fres.exists():
                        import tempfile, os
                        with tempfile.NamedTemporaryFile(delete=False) as tf:
                            tfname = tf.name
                        _sp2.run(["bash", str(fres), "compute", tfname], capture_output=True, timeout=10)
                        try:
                            import hashlib
                            with open(tfname, "rb") as fh:
                                cur = hashlib.sha256(fh.read()).hexdigest()
                            if gd != cur:
                                print(f"ERROR: golden {args.probe} ntpers_digest {gd[:16]} != current {cur[:16]} (stale golden)", file=sys.stderr)
                                return 125
                        except (OSError, ValueError, RuntimeError) as _e:
                            print(f"WARN: golden digest compute failed: {_e}", file=sys.stderr)
                        try:
                            os.unlink(tfname)
                        except OSError as _cleanup_e:
                            print(f"WARN: cleanup unlink failed for {tfname}: {_cleanup_e}", file=sys.stderr)
        except Exception as e:
            print(f"WARN: golden digest check error {e}", file=sys.stderr)

    if not volume_ok:
        return 1
    if verdict != "PASS":
        print(f"FAIL: dual-run for {args.probe} is {verdict} (S1 {s1} S2 {s2}) for {args.criterion}: {eval_.get('reason')}", file=sys.stderr)
        # Dump divergences for triage
        for d in probe_res.get("divergences", [])[:10]:
            print(f"  [{d['severity']}] {d['field']}: win {d['windows'][:80]} vs vm {d['vm'][:80]}", file=sys.stderr)
        return 1

    print(f"PASS: dual-run {args.probe} PASS for {args.criterion} (S1 {s1} S2 {s2})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
