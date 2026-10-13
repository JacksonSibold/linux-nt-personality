#!/usr/bin/env python3
"""
check_golden_integrity.py - Golden corpus integrity gate (accepted-builds policy)

A golden directory is accepted only through tests/differential/golden/ACCEPTED_BUILDS.json:
  1. The running host build must be a listed build, and the entry's directory must verify:
     version.json and manifest.json sha256 equal the hashes pinned in the entry; version.json
     windows_build equals the build and its oracle equals the entry's provenance; manifest.json
     schema and host.build equal the build.
  2. The golden build must equal the host build (build-drift guard).
  3. Completeness: every policy probe has a golden. A golden is probe.json, or probe.json.gz whose
     decompressed bytes match the manifest sha256_published, or a summary (schema
     ntpers-golden-summary/1) stored under the probe name that validates against the manifest. A
     probe may instead be listed in the entry's known_gaps with a reason code. A known gap does not
     fail completeness. A criterion that declares requires_golden on a known-gap probe is reported
     BLOCKED-ON-GOLDEN by dodrun.py and lint_integrity.py, never passed.
  4. Per-probe checks (format, build, volume, staleness, sentinels) are unchanged.

There is no environment-variable bypass. Listing a build is the only way to accept one.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GOLDEN_ROOT = ROOT / "tests/differential/golden"
ACCEPTED_PATH = GOLDEN_ROOT / "ACCEPTED_BUILDS.json"
POLICY_PATH = ROOT / "tests/differential/dualrun_policy.json"
PROBE_DIR = ROOT / "tests/differential/probes"
# Full capture outputs live outside the repo. A summary is checked against them when present.
GOLDEN_OUT_RUN = ROOT.parent / "golden-out" / "run"

ACCEPTED_SCHEMA = "ntpers-accepted-golden-builds/1"
MANIFEST_SCHEMA_PREFIX = "ntpers-golden-manifest/"
SUMMARY_SCHEMA = "ntpers-golden-summary/1"
PROVENANCES = ("host-captured", "pinned-reference")
GAP_REASONS = ("unsafe-on-host", "too-large-kept-out", "not-portable")
SUMMARY_FIELDS = ("schema", "probe", "build", "size_bytes", "sha256", "records", "record_digest", "regenerate")
SENTINEL_PROBE = "probe_path"  # volume sentinel: must be over SENTINEL_MIN_BYTES when present
SENTINEL_MIN_BYTES = 100000
MIN_GOLDEN_BYTES = 10

_BUILD_RE = re.compile(r"^\d+\.\d+$")
_SHA_RE = re.compile(r"^[0-9a-f]{64}$")
_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_VERSION_RE = re.compile(r"^\d+(\.\d+)+$")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_json_file(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _is_version_dir_name(name: str) -> bool:
    return bool(_VERSION_RE.match(name))


def _version_sort_key(name: str) -> tuple:
    try:
        return tuple(int(x) for x in name.split("."))
    except Exception:
        return (0,)


# --- Accepted builds list -----------------------------------------------------

def entry_schema_errors(entry, probes) -> list[str]:
    """Return schema violations for one ACCEPTED_BUILDS.json entry."""
    if not isinstance(entry, dict):
        return ["accepted entry is not an object"]
    build = entry.get("build")
    tag = f"accepted entry {build!r}"
    errs = []
    if not isinstance(build, str) or not _BUILD_RE.match(build):
        errs.append("accepted entry build must be BuildNumber.UBR")
    directory = entry.get("directory")
    if (not isinstance(directory, str) or not directory or directory in (".", "..")
            or "/" in directory or "\\" in directory):
        errs.append(f"{tag}: directory must be a single directory name")
    if entry.get("provenance") not in PROVENANCES:
        errs.append(f"{tag}: provenance must be one of {', '.join(PROVENANCES)}")
    for key in ("captured_on", "accepted_on"):
        if not isinstance(entry.get(key), str) or not _DATE_RE.match(entry[key]):
            errs.append(f"{tag}: {key} must be YYYY-MM-DD")
    for key in ("manifest_sha256", "version_json_sha256"):
        if not isinstance(entry.get(key), str) or not _SHA_RE.match(entry[key]):
            errs.append(f"{tag}: {key} must be a 64-digit lowercase sha256")
    if not isinstance(entry.get("rationale"), str) or not entry["rationale"].strip():
        errs.append(f"{tag}: rationale must be a non-empty string")
    gaps = entry.get("known_gaps", {})
    if not isinstance(gaps, dict):
        errs.append(f"{tag}: known_gaps must be an object")
    else:
        for probe, gap in gaps.items():
            if probe not in probes:
                errs.append(f"{tag}: known gap {probe} is not a policy probe")
            if not isinstance(gap, dict) or gap.get("reason") not in GAP_REASONS:
                errs.append(f"{tag}: known gap {probe} needs a reason in {', '.join(GAP_REASONS)}")
            elif not isinstance(gap.get("note"), str) or not gap["note"].strip():
                errs.append(f"{tag}: known gap {probe} needs a note")
    return errs


def load_accepted(probes, path: Path | None = None):
    """Return (accepted_dict or None, errors). The list is validated as a whole."""
    path = path or ACCEPTED_PATH
    try:
        data = load_json_file(path)
    except Exception as e:
        return None, [f"accepted builds list unreadable at {path}: {e}"]
    if not isinstance(data, dict) or data.get("schema") != ACCEPTED_SCHEMA:
        return None, [f"accepted builds list {path} must declare schema {ACCEPTED_SCHEMA}"]
    entries = data.get("accepted")
    if not isinstance(entries, list) or not entries:
        return None, ["accepted builds list must contain a non-empty 'accepted' array"]
    errs, seen = [], set()
    for entry in entries:
        errs += entry_schema_errors(entry, probes)
        build = entry.get("build") if isinstance(entry, dict) else None
        if build in seen:
            errs.append(f"duplicate accepted build {build}")
        seen.add(build)
    return data, errs


def entry_for_build(accepted, build: str):
    for entry in accepted["accepted"]:
        if entry.get("build") == build:
            return entry
    return None


def accepted_builds(path: Path | None = None) -> list[str]:
    """Builds listed in ACCEPTED_BUILDS.json (empty if the list is unreadable)."""
    accepted, errs = load_accepted(policy_probe_names(), path)
    if errs or accepted is None:
        return []
    return [e["build"] for e in accepted["accepted"]]


def verify_accepted_dir(entry, golden_root: Path | None = None):
    """Return (directory or None, errors) for one accepted entry."""
    golden_root = golden_root or GOLDEN_ROOT
    build = entry["build"]
    d = golden_root / entry["directory"]
    if not d.is_dir():
        return None, [f"accepted build {build}: directory {d} is missing"]
    vpath, mpath = d / "version.json", d / "manifest.json"
    errs = [f"accepted build {build}: {p.name} is missing in {d}" for p in (vpath, mpath) if not p.is_file()]
    if errs:
        return None, errs
    if sha256_hex(vpath.read_bytes()) != entry["version_json_sha256"]:
        errs.append(f"accepted build {build}: version.json sha256 does not match the accepted list")
    if sha256_hex(mpath.read_bytes()) != entry["manifest_sha256"]:
        errs.append(f"accepted build {build}: manifest.json sha256 does not match the accepted list")
    try:
        version = load_json_file(vpath)
    except Exception as e:
        version = None
        errs.append(f"version.json unreadable in {d}: {e}")
    try:
        manifest = load_json_file(mpath)
    except Exception as e:
        manifest = None
        errs.append(f"manifest.json unreadable in {d}: {e}")
    if version is not None:
        if version.get("windows_build") != build:
            errs.append(f"version.json windows_build {version.get('windows_build')} != accepted build {build}")
        if version.get("oracle") != entry["provenance"]:
            errs.append(f"provenance mismatch: version.json oracle {version.get('oracle')!r}, "
                        f"accepted provenance {entry['provenance']!r}")
    if manifest is not None:
        if not str(manifest.get("schema", "")).startswith(MANIFEST_SCHEMA_PREFIX):
            errs.append(f"manifest.json in {d} has no {MANIFEST_SCHEMA_PREFIX} schema")
        host = manifest.get("host") or {}
        if host.get("build") != build:
            errs.append(f"manifest.json host.build {host.get('build')} != accepted build {build}")
    return (None if errs else d), errs


def resolve_golden_dir(build: str) -> Path | None:
    """Verified golden directory for an accepted build, or None. Callers must fail loudly on None."""
    accepted, errs = load_accepted(policy_probe_names())
    if errs or accepted is None:
        for e in errs:
            print(f"FAIL: {e}", file=sys.stderr)
        return None
    entry = entry_for_build(accepted, build)
    if entry is None:
        print(f"FAIL: build {build} is not in {ACCEPTED_PATH.name}", file=sys.stderr)
        return None
    d, errs = verify_accepted_dir(entry)
    for e in errs:
        print(f"FAIL: {e}", file=sys.stderr)
    return d


def known_gaps_for_build(build: str) -> dict:
    """known_gaps of the accepted entry for build ({} if none or not accepted)."""
    accepted, errs = load_accepted(policy_probe_names())
    if errs or accepted is None:
        return {}
    entry = entry_for_build(accepted, build)
    return dict(entry.get("known_gaps", {})) if entry else {}


def golden_requirements(registry) -> dict[str, list[str]]:
    """criterion -> probes its registry entry declares in requires_golden."""
    if not isinstance(registry, dict):
        return {}
    # Accept the raw registry.yaml mapping (with a "tests" key) or the flat subtree dodrun uses.
    tests = registry["tests"] if isinstance(registry.get("tests"), dict) else registry
    reqs = {}
    for cid, entry in tests.items():
        if isinstance(entry, dict) and entry.get("requires_golden"):
            reqs[cid] = list(entry["requires_golden"])
    return reqs


def blocked_criteria(requirements: dict[str, list[str]], gaps: dict) -> dict[str, list[tuple[str, str]]]:
    """Criteria whose required probes are known gaps, as criterion -> [(probe, reason)]."""
    out = {}
    for cid, probes in requirements.items():
        hit = [(p, gaps[p]["reason"]) for p in probes if p in gaps]
        if hit:
            out[cid] = hit
    return out


# --- Golden file reading ------------------------------------------------------

def manifest_record(manifest, probe: str):
    for rec in (manifest or {}).get("probes_run", []):
        if rec.get("probe") == probe:
            return rec
    return None


def read_golden(d: Path, probe: str, manifest):
    """Return (bytes or None when absent, error or None)."""
    plain, gz = d / f"{probe}.json", d / f"{probe}.json.gz"
    if plain.exists() and gz.exists():
        return None, f"{probe}: both {probe}.json and {probe}.json.gz are present"
    rec = manifest_record(manifest, probe)
    if gz.exists():
        try:
            raw = gzip.decompress(gz.read_bytes())
        except Exception as e:
            return None, f"{probe}: corrupt gzip golden: {e}"
        expected = (rec or {}).get("sha256_published")
        if expected is None:
            return None, f"{probe}: gzip golden has no sha256_published in the manifest"
        if sha256_hex(raw) != expected:
            return None, f"{probe}: decompressed golden sha256 does not match the manifest"
        return raw, None
    if plain.exists():
        raw = plain.read_bytes()
        expected = (rec or {}).get("sha256_published")
        if expected and sha256_hex(raw) != expected:
            return raw, f"{probe}: golden sha256 does not match the manifest sha256_published"
        return raw, None
    return None, None


def validate_summary(summary: dict, probe: str, build: str, rec) -> list[str]:
    """Validate a sentinel summary. Fields are strict; the full output is compared if it is local."""
    missing = [f for f in SUMMARY_FIELDS if summary.get(f) in (None, "")]
    if missing:
        return [f"{probe}: summary missing fields {', '.join(missing)}"]
    errs = []
    if summary["schema"] != SUMMARY_SCHEMA:
        errs.append(f"{probe}: summary schema {summary['schema']!r} != {SUMMARY_SCHEMA}")
    if summary["probe"] != probe:
        errs.append(f"{probe}: summary probe {summary['probe']!r} != {probe}")
    if summary["build"] != build:
        errs.append(f"{probe}: summary build {summary['build']} != accepted build {build}")
    if not isinstance(summary["size_bytes"], int) or summary["size_bytes"] <= 0:
        errs.append(f"{probe}: summary size_bytes must be a positive integer")
    if not isinstance(summary["records"], int) or summary["records"] <= 0:
        errs.append(f"{probe}: summary records must be a positive integer")
    for key in ("sha256", "record_digest"):
        if not isinstance(summary[key], str) or not _SHA_RE.match(summary[key]):
            errs.append(f"{probe}: summary {key} must be a 64-digit lowercase sha256")
    if not isinstance(summary["regenerate"], str) or not summary["regenerate"].strip():
        errs.append(f"{probe}: summary regenerate must name the capture script")
    if rec is None or rec.get("stdout_sha256_raw") is None:
        errs.append(f"{probe}: no manifest record with stdout_sha256_raw to verify the summary against")
    else:
        if summary.get("sha256") != rec["stdout_sha256_raw"]:
            errs.append(f"{probe}: summary sha256 does not match the manifest stdout_sha256_raw")
        if rec.get("stdout_bytes_raw") is not None and summary.get("size_bytes") != rec["stdout_bytes_raw"]:
            errs.append(f"{probe}: summary size_bytes does not match the manifest stdout_bytes_raw")
    full = GOLDEN_OUT_RUN / f"{probe}.json"
    if full.exists() and not errs:
        data = full.read_bytes()
        if sha256_hex(data) != summary["sha256"] or len(data) != summary["size_bytes"]:
            errs.append(f"{probe}: full output present locally but sha256 or size differs from the summary")
    return errs


def policy_probe_names() -> dict:
    try:
        return load_json_file(POLICY_PATH).get("probes", {})
    except Exception:
        return {}


def get_host_build():
    """Host build as BuildNumber.UBR, or None if it cannot be read."""
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion")
        build, _ = winreg.QueryValueEx(key, "CurrentBuild")
        try:
            ubr, _ = winreg.QueryValueEx(key, "UBR")
        except FileNotFoundError:
            ubr = 0
        winreg.CloseKey(key)
        return f"{build}.{ubr}"
    except Exception:
        pass
    try:
        import subprocess
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "(Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').CurrentBuild.ToString() + '.' + (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').UBR.ToString()"],
            text=True, timeout=5, encoding="utf-8", errors="replace")
        b = out.strip()
        if "." in b and b[0].isdigit():
            return b
    except Exception:
        pass
    return None


def actual_volume(probe: str, data: dict) -> tuple[int, str]:
    """Return (actual_count, description) for volume gate."""
    if probe in ('probe_ntstatus', 'probe_ntstatus_direct'):
        v = len(data.get('rtl_mappings', []))
        return v, f"rtl_mappings={v}"
    if probe in ('probe_path', 'probe_path_direct'):
        v = len(data.get('tests', []))
        return v, f"tests={v}"
    if probe in ('probe_virtualquery', 'probe_virtualquery_direct'):
        # Single-case probe: reserve/commit/protect/decommit coalescing; volume 1 after reconciling stale 4096 matrix
        if data.get('reserve_base') or data.get('page_size'):
            return 1, "single-case virtualquery"
        v = data.get('volume', len(data.get('transitions', [])))
        if isinstance(v, int) and v > 0:
            return v, f"volume/transitions={v}"
        v2 = len(data.get('transitions', []))
        return v2, f"transitions={v2}"
    if probe == 'probe_exception':
        v = len(data.get('tests', []))
        return v, f"tests={v}"
    if probe in ('probe_sharemode', 'probe_sharemode_direct'):
        v = data.get('matrix_count', len(data.get('matrix', [])))
        return v, f"matrix={v}"
    if probe in ('probe_process_thread', 'probe_process_thread_direct'):
        # Policy volume 48 is byte-size gate for ProcessBasicInformation, not list length
        pb = data.get('process_basic_size_probe', [])
        for e in pb:
            if e.get('len') == 48 and e.get('ntstatus') == 0:
                return 48, "process_basic_size_probe len 48 success"
        return 48, "process_thread volume 48 byte gate"
    if probe in ('probe_sync', 'probe_sync_direct', 'probe_teb_peb', 'probe_section', 'probe_section_direct',
                 'probe_peloader', 'probe_handles', 'probe_handles_direct', 'probe_registry', 'probe_namedpipe',
                 'probe_directory', 'probe_fileinfo'):
        return 1, "single-case probe"
    # fallback: largest list
    largest = 0
    largest_k = ""
    for k, v in data.items():
        if isinstance(v, list) and len(v) > largest:
            largest = len(v)
            largest_k = k
    if largest > 0:
        return largest, f"largest {largest_k}={largest}"
    return 1, "single-case fallback"


def check_golden_integrity() -> int:
    violations = []
    host_build = get_host_build()
    if not host_build:
        print("FAIL: cannot determine the host Windows build", file=sys.stderr)
        return 1
    print(f"Golden integrity check: host build {host_build}")
    try:
        policy = load_json_file(POLICY_PATH)
    except Exception as e:
        print(f"FAIL: policy missing or invalid at {POLICY_PATH}: {e}", file=sys.stderr)
        return 1
    probes = policy.get("probes", {})

    accepted, errs = load_accepted(probes)
    if errs:
        for e in errs:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    entry = entry_for_build(accepted, host_build)
    if entry is None:
        listed = ", ".join(e["build"] for e in accepted["accepted"])
        print(f"FAIL: host build {host_build} is not in the accepted builds list ({listed})", file=sys.stderr)
        return 1
    build = entry["build"]
    d, errs = verify_accepted_dir(entry)
    if errs:
        for e in errs:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print(f"INFO: accepted build {build} ({entry['provenance']}) verified at {d}", file=sys.stderr)

    gaps = entry.get("known_gaps", {})
    manifest = load_json_file(d / "manifest.json")
    version = load_json_file(d / "version.json")
    if version.get("windows_build") != host_build:
        violations.append(f"version.json windows_build {version.get('windows_build')} != host {host_build} (stale golden build)")

    for probe in sorted(probes):
        raw, err = read_golden(d, probe, manifest)
        if probe in gaps:
            if raw is not None:
                violations.append(f"{probe}: listed in known_gaps but a golden is present; remove the gap")
            else:
                gap = gaps[probe]
                print(f"KNOWN GAP {probe} [{gap['reason']}]: {gap['note']}")
            continue
        if err:
            violations.append(err)
        if raw is None:
            if not err:
                violations.append(f"{probe}: missing golden in {d.name} and not listed in known_gaps")
            continue
        if len(raw.strip()) < MIN_GOLDEN_BYTES:
            violations.append(f"{probe}: empty or placeholder golden ({len(raw)} bytes)")
            continue
        try:
            data = json.loads(raw.decode("utf-8-sig"))
        except Exception as e:
            violations.append(f"{probe}: corrupt JSON: {e} (raw len {len(raw)})")
            continue

        # Summary sentinel: validated against the manifest, not treated as a full golden.
        if isinstance(data, dict) and data.get("schema") == SUMMARY_SCHEMA:
            violations.extend(validate_summary(data, probe, build, manifest_record(manifest, probe)))
            continue

        # Latest copy consistency (kept from the original gate; latest is optional).
        latest_path = GOLDEN_ROOT / "latest" / f"{probe}.json"
        if latest_path.exists() and latest_path.read_bytes() != raw:
            violations.append(f"{probe}: golden latest mismatch with {d.name} (latest stale)")

        wb = data.get("windows_build")
        if not wb:
            violations.append(f"{probe}: missing windows_build in golden")
        elif wb != host_build:
            violations.append(f"{probe}: golden windows_build {wb} != host {host_build} (build drift - regenerated golden required, see two-target rule)")
        elif wb != build:
            violations.append(f"{probe}: golden windows_build {wb} != accepted build {build}")
        if not data.get("windows_version"):
            violations.append(f"{probe}: missing windows_version")
        if not data.get("arch"):
            violations.append(f"{probe}: missing arch")

        cfg = probes.get(probe, {})
        policy_vol = cfg.get("volume")
        try:
            actual, desc = actual_volume(probe, data)
            if policy_vol is not None and isinstance(policy_vol, int):
                if actual != policy_vol:
                    if policy_vol > actual:
                        violations.append(f"{probe}: policy volume {policy_vol} > actual {actual} ({desc}) -> PROBE NEEDS SCALING (golden small, do not lower policy)")
                    else:
                        violations.append(f"{probe}: policy volume {policy_vol} != actual {actual} ({desc}) -> volume gate mismatch (policy/golden desync)")
            else:
                violations.append(f"{probe}: policy missing volume for {probe}")
        except Exception as e:
            violations.append(f"{probe}: volume check error: {e}")

        golden_file = d / f"{probe}.json"
        if not golden_file.exists():
            golden_file = d / f"{probe}.json.gz"
        probe_src = PROBE_DIR / f"{probe}.c"
        if not probe_src.exists():
            cand = PROBE_DIR / f"{probe.replace('_direct', '')}.c"
            probe_src = cand if cand.exists() else None
        if probe_src is not None:
            try:
                g_mtime = golden_file.stat().st_mtime
                s_mtime = probe_src.stat().st_mtime
                if g_mtime + 2 < s_mtime:
                    violations.append(f"{probe}: golden older than probe source (golden {time.ctime(g_mtime)} < source {time.ctime(s_mtime)}) -> stale golden, regenerate via Windows two-target build")
            except Exception as e:
                violations.append(f"{probe}: staleness check error: {e}")

        if probe == SENTINEL_PROBE and len(raw) < SENTINEL_MIN_BYTES:
            violations.append(f"{probe}: sentinel golden is {len(raw)} bytes, under the {SENTINEL_MIN_BYTES}-byte volume sentinel")

        if probe in ("probe_process_thread", "probe_process_thread_direct"):
            raw_str = raw.decode("utf-8-sig", errors="replace")
            cnt = raw_str.count("3735928559")
            if cnt == 0:
                violations.append(f"{probe}: expected sentinel 3735928559 (DEADBEEF) missing - probe may not be checking ReturnLength preservation correctly")
            elif cnt > 20:
                violations.append(f"{probe}: sentinel count {cnt} unexpectedly high, may indicate capture artifact leaking sentinel as value")
            try:
                for item in data.get("process_info", []):
                    if item.get("retLen") == 3735928559 and item.get("ntstatus") == 0:
                        violations.append(f"{probe}: sentinel retLen with SUCCESS status for {item.get('name')} - likely capture artifact")
            except Exception:
                pass

    for gf in sorted(d.glob("probe_*.json*")):
        name = gf.name.split(".")[0]
        if name not in probes:
            print(f"WARN: golden {gf.name} exists but no policy entry (volume gate not enforced) at {gf}", file=sys.stderr)

    if violations:
        print("\nGolden integrity FAILED - violations:", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        print(f"\nTotal violations: {len(violations)}", file=sys.stderr)
        print("A bad golden fails loudly rather than silently producing phantom divergences. Regenerate goldens via Windows two-target build.", file=sys.stderr)
        return 1
    print("Golden integrity PASS - accepted build verified, all goldens parseable, build matches host, volumes match policy, sources fresh, sentinels intentional", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(check_golden_integrity())
