#!/usr/bin/env python3
"""
dodrun.py - Definition-of-Done conformance runner for project ntpers.

This is the project's source of truth for completion. It is intentionally
hard to game:

* A criterion with no registered test is UNIMPLEMENTED, never PASS.
* A criterion whose dependency_class is EXT-GATED can only PASS if an evidence
  file proving the controlling artifact was obtained is present and hash-verified.
* Exit code is non-zero on any new failure (regression) or if a tier gate is missed.

Usage:
    python3 tests/harness/dodrun.py
    python3 tests/harness/dodrun.py --registry tests/harness/registry.yaml
    python3 tests/harness/dodrun.py --reports-dir reports
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import yaml
    HAS_YAML = True
except ImportError:  # pragma: no cover
    yaml = None  # type: ignore
    HAS_YAML = False


# ---------------------------------------------------------------------------
# Constants and schema
# ---------------------------------------------------------------------------

RESULT_STATES = ["PASS", "FAIL", "ERROR", "TIMEOUT", "SKIP", "UNIMPLEMENTED", "GATED", "BLOCKED-ON-GOLDEN"]

def golden_blocked_map(registry: Dict[str, Any]) -> Dict[str, List[Tuple[str, str]]]:
    """criterion -> [(probe, reason)] for registry entries whose requires_golden lists a known gap
    of the accepted golden build. These criteria are reported BLOCKED-ON-GOLDEN, never PASS."""
    import importlib.util as _ilu
    spec = _ilu.spec_from_file_location("check_golden_integrity", project_root() / "tests" / "differential" / "check_golden_integrity.py")
    mod = _ilu.module_from_spec(spec)
    spec.loader.exec_module(mod)
    host = mod.get_host_build()
    gaps = mod.known_gaps_for_build(host) if host else {}
    return mod.blocked_criteria(mod.golden_requirements(registry), gaps)

TIER_GATES = {
    "P0": 1.0,   # 100%
    "P1": 0.98,  # >= 98%
    "P2": 0.85,  # >= 85%
    # P3 is best-effort; no numeric gate.
}

TIER_ORDER = ["P0", "P1", "P2", "P3"]

EVIDENCE_REQUIRED_KEYS = {"artifact_path", "artifact_sha256"}

DEFAULT_TIMEOUT = 120.0  # seconds


# ---------------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------------

def project_root() -> Path:
    """Return the repository root from any invocation location."""
    # This file lives at <root>/tests/harness/dodrun.py
    return Path(__file__).resolve().parents[2]


def load_json(path: Path) -> Any:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def load_yaml(path: Path) -> Any:
    if not HAS_YAML:
        raise RuntimeError(
            "PyYAML is not installed. Install it with:\n"
            "  pip install --break-system-packages pyyaml\n"
            "or use a virtual environment."
        )
    with open(path, "r", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def load_registry(path: Path) -> Dict[str, Any]:
    """Load registry.yaml (or registry.json)."""
    if path.suffix.lower() in (".yaml", ".yml"):
        data = load_yaml(path)
    else:
        data = load_json(path)

    if data is None:
        return {}

    # Support either a top-level mapping of criterion -> entry or a "tests" key.
    if isinstance(data, dict) and "tests" in data and isinstance(data["tests"], dict):
        return data["tests"]
    if isinstance(data, dict):
        return data

    raise ValueError(f"Registry {path} must be a mapping of criterion id -> test entry")


def get_registry_entry(registry: Dict[str, Any], criterion_id: str) -> Optional[Dict[str, Any]]:
    entry = registry.get(criterion_id)
    if entry is None:
        return None
    if isinstance(entry, dict):
        return entry
    # Allow shorthand: command string
    return {"command": entry}


# ---------------------------------------------------------------------------
# Evidence verification
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_evidence(evidence_path: Path) -> Tuple[bool, str]:
    """
    Verify an evidence file.

    Returns (ok, reason).
    ok=True means the evidence file exists, is well-formed, names an existing
    artifact, and the artifact's SHA-256 matches the recorded hash.
    """
    if not evidence_path.exists():
        return False, f"evidence file missing: {evidence_path}"

    try:
        evidence = load_json(evidence_path)
    except Exception as exc:
        return False, f"evidence file is not valid JSON: {exc}"

    missing = EVIDENCE_REQUIRED_KEYS - set(evidence.keys())
    if missing:
        return False, f"evidence missing keys: {sorted(missing)}"

    artifact_path = Path(evidence["artifact_path"]).expanduser()
    if not artifact_path.is_absolute():
        artifact_path = project_root() / artifact_path

    if not artifact_path.exists():
        return False, f"artifact missing: {artifact_path}"

    expected_hash = evidence["artifact_sha256"].lower().strip()
    actual_hash = sha256_file(artifact_path)
    if actual_hash != expected_hash:
        return False, (
            f"artifact hash mismatch for {artifact_path}: "
            f"expected {expected_hash[:16]}..., got {actual_hash[:16]}..."
        )

    return True, "evidence verified"


def default_evidence_path(criterion_id: str) -> Path:
    return project_root() / "evidence" / f"{criterion_id}.evidence.json"


# ---------------------------------------------------------------------------
# Test execution
# ---------------------------------------------------------------------------

def run_command(cmd: List[str], timeout: float, cwd: Path, env: Dict[str, str]) -> Dict[str, Any]:
    """
    Execute a test command and return a result record.

    State rules:
      * exit code 0            -> PASS
      * exit code 2            -> UNIMPLEMENTED (dualrun_wrapper and canaries)
      * exit code 3            -> SKIP
      * exit code 124/137      -> TIMEOUT (VM harness timeout, GNU timeout KILL)
      * exit code 125          -> ERROR (harness/missing artifact)
      * negative exit code     -> ERROR (signal/crash)
      * exit code >= 0xC0000000 -> ERROR (Windows NTSTATUS exception)
      * non-zero exit code     -> FAIL
      * subprocess timeout     -> TIMEOUT
      * command not found      -> ERROR
    """
    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        duration = time.monotonic() - start
        if proc.returncode == 0:
            state = "PASS"
        elif proc.returncode == 2:
            state = "UNIMPLEMENTED"
        elif proc.returncode == 3:
            state = "SKIP"
        elif proc.returncode in (124, 137):
            state = "TIMEOUT"
        elif proc.returncode == 125:
            state = "ERROR"
        elif proc.returncode < 0 or proc.returncode >= 0xC0000000:
            # Negative: POSIX signal. >= 0xC0000000: Windows NTSTATUS exception
            # (access violation etc.), reported unsigned by subprocess. Both are crashes.
            state = "ERROR"
        else:
            state = "FAIL"
        return {
            "state": state,
            "exit_code": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "duration_seconds": round(duration, 6),
            "note": "",
        }
    except subprocess.TimeoutExpired as exc:
        duration = time.monotonic() - start
        return {
            "state": "TIMEOUT",
            "exit_code": None,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "",
            "duration_seconds": round(duration, 6),
            "note": f"timed out after {timeout}s",
        }
    except FileNotFoundError as exc:
        return {
            "state": "ERROR",
            "exit_code": None,
            "stdout": "",
            "stderr": str(exc),
            "duration_seconds": 0.0,
            "note": f"command not found: {cmd[0]}",
        }
    except Exception as exc:  # pragma: no cover
        return {
            "state": "ERROR",
            "exit_code": None,
            "stdout": "",
            "stderr": str(exc),
            "duration_seconds": 0.0,
            "note": f"exception during execution: {type(exc).__name__}",
        }


PYTHON_ALIASES = ("python3", "python")


def resolve_command(cmd: List[str]) -> List[str]:
    """Map a bare 'python3' or 'python' (as used in registry.yaml) to the running interpreter.

    This applies on every platform and does not depend on PATH lookup. On Windows,
    shutil.which('python3') finds the Microsoft Store alias in WindowsApps, which
    is not None, so a which-guard skips the mapping and the alias exits 9009
    ("Python was not found"). The harness runs under a working interpreter, so
    that interpreter is the one to use for command checks.
    """
    if cmd and cmd[0] in PYTHON_ALIASES:
        return [sys.executable] + list(cmd[1:])
    return list(cmd)


def build_command(entry: Dict[str, Any]) -> Optional[List[str]]:
    """Normalize the command field to a list of strings, or None."""
    cmd = entry.get("command")
    if cmd is None or cmd == "" or cmd == "unimplemented":
        return None
    if isinstance(cmd, str):
        return [cmd]
    if isinstance(cmd, list):
        return [str(x) for x in cmd]
    raise ValueError(f"Unsupported command type: {type(cmd)}")


def evaluate_criterion(
    criterion: Dict[str, Any],
    registry: Dict[str, Any],
    default_timeout: float,
    env: Dict[str, str],
    blocked: Optional[Dict[str, List[Tuple[str, str]]]] = None,
) -> Dict[str, Any]:
    """
    Evaluate a single criterion and return a result record.

    Governance rules are applied in this order:
      1. EXT-GATED criteria require verified evidence; otherwise -> GATED.
      2. A criterion with no registered test -> UNIMPLEMENTED.
      3. Explicit skip -> SKIP.
      4. Run the test -> PASS/FAIL/ERROR/TIMEOUT.
    """
    cid = criterion["id"]
    tier = criterion.get("tier", "P3")
    dep_class = criterion.get("dependency_class", "EXT-DOC")
    entry = get_registry_entry(registry, cid)

    result: Dict[str, Any] = {
        "id": cid,
        "tier": tier,
        "dependency_class": dep_class,
        "state": "UNIMPLEMENTED",
        "exit_code": None,
        "stdout": "",
        "stderr": "",
        "duration_seconds": 0.0,
        "note": "",
    }

    # 0. Golden dependency on a known gap of the accepted build: BLOCKED, never PASS.
    if blocked and cid in blocked:
        gaps_txt = ", ".join(f"{p} [{r}]" for p, r in blocked[cid])
        result["state"] = "BLOCKED-ON-GOLDEN"
        result["note"] = f"requires golden for {gaps_txt}; no golden on the accepted build"
        return result

    # 1. GATED check.
    if dep_class == "EXT-GATED":
        evidence_path_override = None
        if entry and "evidence" in entry:
            evidence_path_override = Path(entry["evidence"])
        evidence_path = evidence_path_override or default_evidence_path(cid)
        if not evidence_path.is_absolute():
            evidence_path = project_root() / evidence_path

        ok, reason = verify_evidence(evidence_path)
        if not ok:
            result["state"] = "GATED"
            result["note"] = reason
            return result
        # Evidence verified; fall through to normal test evaluation.
        result["note"] = reason

    # 2. Registered-test check.
    if entry is None:
        result["state"] = "UNIMPLEMENTED"
        result["note"] = "no test registered"
        return result

    if entry.get("skip"):
        result["state"] = "SKIP"
        result["note"] = entry.get("skip_reason", "explicitly skipped")
        return result

    cmd = build_command(entry)
    if cmd is None:
        result["state"] = "UNIMPLEMENTED"
        result["note"] = "registered test is unimplemented"
        return result

    cmd = resolve_command(cmd)

    # Expand relative command paths against project root.
    if not os.path.isabs(cmd[0]):
        candidate = project_root() / cmd[0]
        if candidate.exists():
            cmd[0] = str(candidate)

    timeout = float(entry.get("timeout", default_timeout))
    cwd = Path(entry.get("cwd", project_root()))

    run_result = run_command(cmd, timeout, cwd, env)
    result.update(run_result)

    # Preserve the evidence note if we already set one.
    if dep_class == "EXT-GATED" and result["state"] == "PASS":
        result["note"] = (result["note"] + "; test passed").lstrip("; ")

    return result


# ---------------------------------------------------------------------------
# Summary, tier roll-up, regression detection
# ---------------------------------------------------------------------------

def summarize(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    counts = Counter(r["state"] for r in results)
    tier_counts: Dict[str, Dict[str, Any]] = {}

    for tier in TIER_ORDER:
        tier_results = [r for r in results if r["tier"] == tier]
        total = len(tier_results)
        passed = sum(1 for r in tier_results if r["state"] == "PASS")
        rate = passed / total if total else 0.0
        gate = TIER_GATES.get(tier)
        met = gate is None or rate >= gate
        tier_counts[tier] = {
            "total": total,
            "passed": passed,
            "pass_rate": round(rate, 6),
            "gate": gate,
            "gate_met": met,
        }

    return {
        "total": len(results),
        "counts": {state: counts.get(state, 0) for state in RESULT_STATES},
        "tier_summary": tier_counts,
    }


def find_previous_run(reports_dir: Path) -> Optional[Path]:
    """Return the most recent results-*.json file in reports_dir."""
    files = sorted(reports_dir.glob("results-*.json"))
    return files[-1] if files else None


def detect_regressions(
    current_results: List[Dict[str, Any]],
    previous_path: Optional[Path],
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    """
    Compare current results to a previous run.

    Returns (regressions, improvements).
    A regression is any criterion whose state strictly worsened:
      PASS -> non-PASS, or FAIL/ERROR/TIMEOUT -> ERROR/TIMEOUT/GATED/UNIMPLEMENTED.
    An improvement is the inverse.
    """
    if previous_path is None:
        return [], []

    previous_record = load_json(previous_path)
    previous_results = previous_record.get("results", previous_record)
    previous = {r["id"]: r for r in previous_results}
    state_rank = {s: i for i, s in enumerate(["PASS", "FAIL", "ERROR", "TIMEOUT", "SKIP", "UNIMPLEMENTED", "GATED"])}

    regressions: List[Dict[str, Any]] = []
    improvements: List[Dict[str, Any]] = []

    for cur in current_results:
        cid = cur["id"]
        prev = previous.get(cid)
        if prev is None:
            continue
        prev_state = prev["state"]
        cur_state = cur["state"]
        if state_rank[cur_state] > state_rank[prev_state]:
            regressions.append({
                "id": cid,
                "previous": prev_state,
                "current": cur_state,
            })
        elif state_rank[cur_state] < state_rank[prev_state]:
            improvements.append({
                "id": cid,
                "previous": prev_state,
                "current": cur_state,
            })

    return regressions, improvements


def write_summary_text(
    path: Path,
    summary: Dict[str, Any],
    regressions: List[Dict[str, Any]],
    improvements: List[Dict[str, Any]],
    gates_met: bool,
) -> None:
    lines = [
        "Definition-of-Done Conformance Summary",
        "======================================",
        f"Total criteria: {summary['total']}",
        "",
        "State counts:",
    ]
    for state in RESULT_STATES:
        n = summary["counts"][state]
        lines.append(f"  {state:15s} {n:4d}")

    lines.append("")
    lines.append("Tier roll-up:")
    for tier in TIER_ORDER:
        ts = summary["tier_summary"][tier]
        gate_str = f" (gate >= {ts['gate']*100:.0f}%)" if ts["gate"] else " (best effort)"
        status = "MET" if ts["gate_met"] else "MISSED"
        lines.append(
            f"  {tier}: {ts['passed']}/{ts['total']} = {ts['pass_rate']*100:.2f}% "
            f"{gate_str} [{status}]"
        )

    lines.append("")
    if regressions:
        lines.append(f"REGRESSIONS ({len(regressions)}):")
        for r in regressions:
            lines.append(f"  {r['id']}: {r['previous']} -> {r['current']}")
    else:
        lines.append("REGRESSIONS: 0")

    if improvements:
        lines.append("")
        lines.append(f"IMPROVEMENTS ({len(improvements)}):")
        for r in improvements:
            lines.append(f"  {r['id']}: {r['previous']} -> {r['current']}")

    lines.append("")
    lines.append(f"GATES_MET: {gates_met}")

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv: List[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Definition-of-Done conformance runner for ntpers",
    )
    p.add_argument(
        "--criteria",
        type=Path,
        default=project_root() / "tests" / "conformance" / "criteria.json",
        help="Path to criteria.json",
    )
    p.add_argument(
        "--registry",
        type=Path,
        default=project_root() / "tests" / "harness" / "registry.yaml",
        help="Path to registry.yaml",
    )
    p.add_argument(
        "--reports-dir",
        type=Path,
        default=project_root() / "reports",
        help="Directory for result JSON files",
    )
    p.add_argument(
        "--previous-run",
        type=Path,
        default=None,
        help="Specific previous result file for regression detection",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=f"Default per-test timeout in seconds (default {DEFAULT_TIMEOUT})",
    )
    p.add_argument(
        "--no-gate-exit",
        action="store_true",
        help="Do not exit non-zero if tier gates are missed",
    )
    return p.parse_args(argv)


def canary_command(exit_code: int) -> List[str]:
    """Command that exits with exit_code, using the interpreter running the harness."""
    return [sys.executable, "-c", f"import sys; sys.exit({int(exit_code)})"]


def verify_positive_controls(env: Dict[str, str], timeout: float, cwd: Path) -> Tuple[bool, str]:
    """
    Positive-control: run a known-PASS (true) and known-FAIL (false) through
    the same run_command path that real criteria use. If the harness maps
    either incorrectly, the entire run is untrustworthy.
    Returns (ok, message).
    """
    # The canaries use the running interpreter, not true/false/sh: those are
    # POSIX binaries that are missing from PATH on Windows hosts (ERROR, rc 125).
    # The interpreter is always resolvable, and the exit codes exercised are
    # the same: 0 -> PASS, 1 -> FAIL, 2 -> UNIMPLEMENTED.
    pass_res = run_command(canary_command(0), timeout, cwd, env)
    fail_res = run_command(canary_command(1), timeout, cwd, env)
    # also test UNIMPLEMENTED path (exit 2)
    unimpl_res = run_command(canary_command(2), timeout, cwd, env)
    errors = []
    if pass_res["state"] != "PASS":
        errors.append(f"CANARY-PASS: 'true' returned {pass_res['state']} (exit {pass_res['exit_code']}) expected PASS")
    if fail_res["state"] != "FAIL":
        errors.append(f"CANARY-FAIL: 'false' returned {fail_res['state']} (exit {fail_res['exit_code']}) expected FAIL")
    if unimpl_res["state"] != "UNIMPLEMENTED":
        errors.append(f"CANARY-UNIMPL: 'exit 2' returned {unimpl_res['state']} expected UNIMPLEMENTED")
    if errors:
        return False, "; ".join(errors)
    return True, "positive controls PASS/FAIL/UNIMPLEMENTED verified"


def main(argv: List[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])

    root = project_root()
    reports_dir = args.reports_dir
    reports_dir.mkdir(parents=True, exist_ok=True)

    # Structural integrity lint - refuse to run if harness has been loosened
    lint_path = root / "tests" / "harness" / "lint_integrity.py"
    if lint_path.exists():
        proc = subprocess.run([sys.executable, str(lint_path)], capture_output=True, text=True)
        if proc.returncode != 0:
            print("INTEGRITY LINT FAILED - dodrun REFUSES TO RUN (suite can be quietly loosened is not an authority)", file=sys.stderr)
            if proc.stdout:
                print(proc.stdout, file=sys.stderr)
            if proc.stderr:
                print(proc.stderr, file=sys.stderr)
            # Still write a run record marking integrity failure so evidence preserved
            timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            result_file = reports_dir / f"results-{timestamp}.json"
            run_record = {
                "run_id": timestamp,
                "project_root": str(root),
                "criteria_file": str(args.criteria),
                "registry_file": str(args.registry),
                "previous_run": None,
                "summary": summarize([]),
                "regressions": [],
                "improvements": [],
                "gates_met": False,
                "harness_integrity": {"positive_control": "FAIL", "reason": "integrity lint failed", "lint_stdout": proc.stdout, "lint_stderr": proc.stderr},
                "results": [],
            }
            try:
                with open(result_file, "w", encoding="utf-8") as fh:
                    json.dump(run_record, fh, indent=2, ensure_ascii=False)
            except Exception:
                pass
            return 1

    criteria = load_json(args.criteria)
    registry = load_registry(args.registry)
    blocked = golden_blocked_map(registry)

    env = os.environ.copy()
    env["DODRUN_PROJECT_ROOT"] = str(root)
    env["DODRUN_REPORTS_DIR"] = str(reports_dir)

    # Positive-control check: harness must correctly report PASS for a known-passing
    # and FAIL for a known-failing input. If this fails, the harness itself is broken
    # (e.g. stale-read returning old PASS for a failing input). Absence of this is why
    # the stale-read bug survived.
    canary_ok, canary_msg = verify_positive_controls(env, 5.0, root)
    if not canary_ok:
        print(f"HARNESS INTEGRITY FAILURE: {canary_msg}", file=sys.stderr)
        print("Aborting run: harness positive-control failed - results would be untrustworthy.", file=sys.stderr)
        # still write a run record marking harness broken so evidence is preserved
        timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        result_file = reports_dir / f"results-{timestamp}.json"
        run_record = {
            "run_id": timestamp,
            "project_root": str(root),
            "criteria_file": str(args.criteria),
            "registry_file": str(args.registry),
            "previous_run": None,
            "summary": summarize([]),
            "regressions": [],
            "improvements": [],
            "gates_met": False,
            "harness_integrity": {"positive_control": "FAIL", "reason": canary_msg},
            "results": [],
        }
        with open(result_file, "w", encoding="utf-8") as fh:
            json.dump(run_record, fh, indent=2, ensure_ascii=False)
        return 125

    results: List[Dict[str, Any]] = []
    for criterion in criteria:
        result = evaluate_criterion(criterion, registry, args.timeout, env, blocked=blocked)
        results.append(result)

    summary = summarize(results)
    previous_path = args.previous_run or find_previous_run(reports_dir)
    regressions, improvements = detect_regressions(results, previous_path)
    gates_met = all(ts["gate_met"] for ts in summary["tier_summary"].values())

    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    result_file = reports_dir / f"results-{timestamp}.json"

    run_record = {
        "run_id": timestamp,
        "project_root": str(root),
        "criteria_file": str(args.criteria),
        "registry_file": str(args.registry),
        "previous_run": str(previous_path) if previous_path else None,
        "summary": summary,
        "regressions": regressions,
        "improvements": improvements,
        "gates_met": gates_met,
        "harness_integrity": {"positive_control": "PASS", "reason": canary_msg},
        "results": results,
    }

    with open(result_file, "w", encoding="utf-8") as fh:
        json.dump(run_record, fh, indent=2, ensure_ascii=False)

    summary_file = reports_dir / f"summary-{timestamp}.txt"
    write_summary_text(summary_file, summary, regressions, improvements, gates_met)

    # Human-readable summary to stdout.
    with open(summary_file, "r", encoding="utf-8") as fh:
        print(fh.read())

    print(f"Detailed results written to: {result_file}")

    if regressions:
        print(f"\nFAIL: {len(regressions)} regression(s) detected.")
        return 1
    if not gates_met and not args.no_gate_exit:
        print("\nFAIL: one or more tier gates missed.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
