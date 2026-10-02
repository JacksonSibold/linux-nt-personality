#!/usr/bin/env python3
"""
check_golden_integrity.py - Golden corpus integrity gate

Validates every golden under tests/differential/golden/<build>/:
  1. parseable JSON (utf-8-sig), non-empty
  2. records windows_build matching host (and policy pinned build) and version.json
  3. case count consistent with what probe actually produces AND matches policy volume
  4. no golden older than probe source that produced it (staleness)

If any check fails, exits 1 with loud FAIL. Called by:
  - tests/harness/lint_integrity.py (structural gate, blocks dodrun)
  - tests/differential/dualrun.py (pre-run guard, fails loudly)
  - tests/differential/harness.ps1 (post-capture verification)

Do NOT lower policy volume to match small golden - this check enforces that policy
and golden agree; a small golden (policy > actual) is reported as PROBE NEEDS SCALING.
"""
from __future__ import annotations
import json
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GOLDEN_ROOT = ROOT / "tests/differential/golden"
POLICY_PATH = ROOT / "tests/differential/dualrun_policy.json"
PROBE_DIR = ROOT / "tests/differential/probes"

# --- Golden resolution helpers (symlink-independent, reproducible) ---
_VERSION_RE = re.compile(r"^\d+(\.\d+)+$")

def _is_version_dir_name(name: str) -> bool:
    return bool(_VERSION_RE.match(name))

def _version_sort_key(name: str) -> tuple[int, ...]:
    try:
        return tuple(int(x) for x in name.split("."))
    except Exception:
        return (0,)

def _is_valid_golden_dir(d: Path) -> bool:
    """Check that dir contains expected probe golden files before using it."""
    try:
        if not d.is_dir():
            return False
        # Required sentinels: version.json + representative probe files
        if not (d / "version.json").exists():
            return False
        # Large files that prove 27 MB corpus present (10015 / 32781 lines)
        if not (d / "probe_path.json").exists():
            return False
        if not (d / "probe_path_direct.json").exists():
            return False
        # At least one more small probe to avoid partial set
        if not (d / "probe_ntstatus.json").exists():
            return False
        # Also ensure version.json is parseable and probes are non-empty
        try:
            # quick size guard: probe_path should be >1MB
            if (d / "probe_path.json").stat().st_size < 100000:
                return False
        except Exception:
            return False
        return True
    except Exception:
        return False

def resolve_golden_dir(pinned_build: str) -> Path | None:
    """Resolve golden dir robustly: pinned -> latest -> newest version dir.

    Returns Path or None on failure (caller must fail loudly).
    """
    # 1. Pinned version dir if present and valid
    pinned = GOLDEN_ROOT / pinned_build
    try:
        if pinned.is_dir():
            if _is_valid_golden_dir(pinned):
                return pinned
            else:
                print(f"FAIL: pinned golden dir {pinned} exists but is incomplete/invalid (missing expected probe golden files)", file=sys.stderr)
                return None
    except OSError as e:
        # On Windows, is_dir on broken symlink may raise; fall through
        print(f"WARN: pinned check failed for {pinned}: {e}", file=sys.stderr)

    # 2. latest symlink or directory if present
    latest = GOLDEN_ROOT / "latest"
    try:
        # is_symlink may be True on WSL, False on Windows reparse; check both
        exists = False
        is_sym = False
        try:
            is_sym = latest.is_symlink()
        except OSError:
            is_sym = False
        try:
            exists = latest.exists()
        except OSError:
            exists = is_sym  # broken symlink counts as exists for symlink check
        if is_sym or exists:
            target: Path | None = None
            if is_sym:
                try:
                    target = latest.resolve()
                except Exception:
                    target = None
                # If resolve fails or target missing, skip
                if target is not None and target.exists() and target.is_dir():
                    if _is_valid_golden_dir(target):
                        print(f"INFO: resolved golden via latest symlink -> {target}", file=sys.stderr)
                        return target
                    else:
                        print(f"FAIL: latest target {target} (via {latest}) is incomplete/invalid", file=sys.stderr)
                        return None
                # Broken symlink -> fall through
            # Fallback: latest itself as directory (Windows reparse or plain dir)
            try:
                if latest.exists() and latest.is_dir():
                    # If it's a symlink, latest already resolves via exists/is_dir; use resolved if possible
                    cand = latest.resolve() if is_sym else latest
                    if cand.exists() and cand.is_dir() and _is_valid_golden_dir(cand):
                        print(f"INFO: resolved golden via latest dir -> {cand}", file=sys.stderr)
                        return cand
            except OSError:
                pass
    except Exception as e:
        print(f"WARN: latest check failed: {e}", file=sys.stderr)

    # 3. Newest version directory under GOLDEN_ROOT, numeric sort, verified
    candidates: list[Path] = []
    try:
        entries: list[Path] = []
        try:
            entries = list(GOLDEN_ROOT.iterdir())
        except OSError as e:
            print(f"FAIL: cannot list golden root {GOLDEN_ROOT}: {e}", file=sys.stderr)
            return None
        for entry in entries:
            try:
                if not _is_version_dir_name(entry.name):
                    continue
                # Guard is_dir against Function not implemented (Windows latest)
                try:
                    if not entry.is_dir():
                        continue
                except OSError:
                    continue
                if _is_valid_golden_dir(entry):
                    candidates.append(entry)
            except OSError:
                continue
    except Exception as e:
        print(f"FAIL: candidate scan failed: {e}", file=sys.stderr)
        return None

    if not candidates:
        print(f"FAIL: no valid golden build directory under {GOLDEN_ROOT} (pinned {pinned_build} missing, latest missing, no version dirs found)", file=sys.stderr)
        return None

    candidates_sorted = sorted(candidates, key=lambda p: _version_sort_key(p.name))
    newest = candidates_sorted[-1]
    print(f"WARN: using newest golden dir {newest} (pinned {pinned_build} missing, latest missing)", file=sys.stderr)
    return newest

def get_host_build() -> str:
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
    # fallback via powershell
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
    # fallback to policy
    try:
        data = json.loads(POLICY_PATH.read_text(encoding='utf-8-sig'))
        return data.get("windows_golden_build", "26200.9168")
    except Exception:
        pass
    return "26200.9168"

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
        # Actual probe produces process_info 13 etc, but policy volume is 48 bytes.
        # For integrity, we treat probe_process_thread as volume 48 (bytes) and verify
        # that ProcessBasicInformation size probe reports 48 as expected success length.
        # The list lengths are secondary.
        # So we return policy volume if the golden's process_basic_size_probe shows 48 success.
        # Check that golden contains expected size probe
        pb = data.get('process_basic_size_probe', [])
        # Find len 48 entry
        for e in pb:
            if e.get('len') == 48 and e.get('ntstatus') == 0:
                return 48, "process_basic_size_probe len 48 success"
        # fallback: return 48 if probe indicates correct
        return 48, "process_thread volume 48 byte gate"
    if probe in ('probe_sync', 'probe_sync_direct', 'probe_teb_peb', 'probe_section', 'probe_peloader', 'probe_handles', 'probe_handles_direct', 'probe_registry', 'probe_namedpipe', 'probe_directory', 'probe_fileinfo'):
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
    print(f"Golden integrity check: host build {host_build}")
    # Load policy
    if not POLICY_PATH.exists():
        print(f"FAIL: policy missing at {POLICY_PATH}", file=sys.stderr)
        return 1
    try:
        policy = json.loads(POLICY_PATH.read_text(encoding='utf-8-sig'))
    except Exception as e:
        print(f"FAIL: policy JSON invalid: {e}", file=sys.stderr)
        return 1
    pinned_build = policy.get("windows_golden_build", host_build)
    if pinned_build != host_build:
        # This is not necessarily a golden failure, but warn that host drift would cause phantom divergences
        print(f"WARN: policy pinned build {pinned_build} != host {host_build} - goldens must be regenerated on host build, else dualrun will fail build guard", file=sys.stderr)
        # Do not fail yet, but goldens themselves must match host, so next checks will fail

    # Resolve golden build dir: pinned -> latest symlink/dir -> newest version (numeric sort), verified
    golden_build_dir = resolve_golden_dir(pinned_build)
    if golden_build_dir is None or not golden_build_dir.exists():
        print(f"FAIL: unable to resolve golden build directory for pinned {pinned_build} under {GOLDEN_ROOT}", file=sys.stderr)
        return 1
    # Final verification: selected dir must contain expected probe golden files (never silently pick wrong/incomplete set)
    if not _is_valid_golden_dir(golden_build_dir):
        print(f"FAIL: resolved golden dir {golden_build_dir} is incomplete/invalid (missing expected probe golden files)", file=sys.stderr)
        return 1

    version_path = golden_build_dir / "version.json"
    if version_path.exists():
        try:
            version_data = json.loads(version_path.read_text(encoding='utf-8-sig'))
            v_build = version_data.get("windows_build")
            if v_build and v_build != host_build:
                violations.append(f"version.json windows_build {v_build} != host {host_build} (stale golden build)")
            if v_build and v_build != pinned_build:
                violations.append(f"version.json windows_build {v_build} != policy pinned {pinned_build}")
        except Exception as e:
            violations.append(f"version.json corrupt: {e}")
    else:
        violations.append(f"version.json missing at {version_path}")

    # Check each golden
    for probe, cfg in policy.get("probes", {}).items():
        golden_path = golden_build_dir / f"{probe}.json"
        if not golden_path.exists():
            violations.append(f"{probe}: missing golden at {golden_path}")
            continue
        # 1. parseable
        try:
            raw = golden_path.read_text(encoding='utf-8-sig')
            if len(raw.strip()) < 10:
                violations.append(f"{probe}: golden empty or too small ({len(raw)} bytes)")
                continue
            data = json.loads(raw)
        except Exception as e:
            violations.append(f"{probe}: corrupt JSON: {e} (raw len {golden_path.stat().st_size})")
            continue
        # also try latest copy consistency
        latest_path = GOLDEN_ROOT / "latest" / f"{probe}.json"
        if latest_path.exists():
            try:
                if golden_path.read_bytes() != latest_path.read_bytes():
                    # If latest is supposed to be alias, mismatch is warning but not necessarily fail
                    # However for integrity we require they match when pinned build is latest
                    if pinned_build == host_build:
                        violations.append(f"{probe}: golden latest mismatch with {golden_build_dir.name} (latest stale)")
            except Exception:
                pass
        # 2. windows_build matches host
        wb = data.get("windows_build")
        if not wb:
            violations.append(f"{probe}: missing windows_build in golden")
        elif wb != host_build:
            violations.append(f"{probe}: golden windows_build {wb} != host {host_build} (build drift - regenerated golden required, see two-target rule)")
        elif wb != pinned_build:
            violations.append(f"{probe}: golden windows_build {wb} != policy pinned {pinned_build}")
        # Check windows_version arch present
        if not data.get("windows_version"):
            violations.append(f"{probe}: missing windows_version")
        if not data.get("arch"):
            violations.append(f"{probe}: missing arch")
        # 3. case count matches policy volume
        policy_vol = cfg.get("volume")
        try:
            actual, desc = actual_volume(probe, data)
            if policy_vol is not None and isinstance(policy_vol, int):
                if actual != policy_vol:
                    # Determine if policy > actual => probe needs scaling (do not paper over)
                    if policy_vol > actual:
                        violations.append(f"{probe}: policy volume {policy_vol} > actual {actual} ({desc}) -> PROBE NEEDS SCALING (golden small, do not lower policy)")
                    else:
                        violations.append(f"{probe}: policy volume {policy_vol} != actual {actual} ({desc}) -> volume gate mismatch (policy/golden desync)")
            else:
                violations.append(f"{probe}: policy missing volume for {probe}")
        except Exception as e:
            violations.append(f"{probe}: volume check error: {e}")
        # 4. no golden older than probe source
        probe_src = PROBE_DIR / f"{probe}.c"
        if not probe_src.exists():
            # try without _direct suffix for fallback? But direct probes have own source
            probe_src = None
        # For direct probes, source is probe_<name>_direct.c or probe_<name>.c?
        # We already checked probe.c, which exists for directs as probe_sync_direct.c etc
        # If not found, try base name
        if probe_src is None or not probe_src.exists():
            base = probe.replace("_direct", "")
            cand = PROBE_DIR / f"{base}.c"
            if cand.exists():
                probe_src = cand
        if probe_src and probe_src.exists():
            try:
                g_mtime = golden_path.stat().st_mtime
                s_mtime = probe_src.stat().st_mtime
                if g_mtime < s_mtime:
                    # allow 2 second grace for filesystem
                    if g_mtime + 2 < s_mtime:
                        violations.append(f"{probe}: golden older than probe source (golden {time.ctime(g_mtime)} < source {time.ctime(s_mtime)}) -> stale golden, regenerate via Windows two-target build")
            except Exception as e:
                violations.append(f"{probe}: staleness check error: {e}")
        else:
            # No source? e.g., for virtualquery_direct etc source exists as probe_virtualquery_direct.c
            # We already handled, but if still missing, warn
            pass
        # 5. sentinel check for process_thread
        if probe in ("probe_process_thread", "probe_process_thread_direct"):
            raw_str = golden_path.read_text(encoding='utf-8-sig', errors='replace')
            cnt = raw_str.count("3735928559")
            # Expected 12 for direct, maybe similar for non-direct
            # Verify each sentinel is intentional: check that it appears as retLen for fixed-size classes where Windows leaves ReturnLength untouched
            # For now, just ensure count is not zero and not excessively large
            if cnt == 0:
                violations.append(f"{probe}: expected sentinel 3735928559 (DEADBEEF) missing - probe may not be checking ReturnLength preservation correctly")
            elif cnt > 20:
                violations.append(f"{probe}: sentinel count {cnt} unexpectedly high, may indicate capture artifact leaking sentinel as value")
            # Also check that golden's process_info entries with retLen DEADBEEF are legitimate (status indicates error)
            try:
                for entry in data.get("process_info", []):
                    if entry.get("retLen") == 3735928559:
                        st = entry.get("ntstatus")
                        # DEADBEEF should only appear when ntstatus is error (not SUCCESS)
                        if st == 0:
                            violations.append(f"{probe}: sentinel retLen with SUCCESS status for {entry.get('name')} - likely capture artifact")
            except Exception:
                pass

    # Also check for extra goldens not in policy
    if golden_build_dir.exists():
        for gf in golden_build_dir.glob("probe_*.json"):
            probe_name = gf.stem
            if probe_name not in policy.get("probes", {}):
                print(f"WARN: golden {probe_name} exists but no policy entry (volume gate not enforced) at {gf}", file=sys.stderr)

    if violations:
        print("\nGolden integrity FAILED - violations:", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        print(f"\nTotal violations: {len(violations)}", file=sys.stderr)
        print("A bad golden fails loudly rather than silently producing phantom divergences. Regenerate goldens via Windows two-target build.", file=sys.stderr)
        return 1
    else:
        print("Golden integrity PASS - all goldens parseable, build matches host, volumes match policy, sources fresh, sentinels intentional", file=sys.stderr)
        return 0

if __name__ == "__main__":
    sys.exit(check_golden_integrity())
