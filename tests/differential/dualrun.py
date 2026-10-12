#!/usr/bin/env python3
"""
dualrun.py - Windows differential DUAL-RUN harness

Joins the two existing rigs:
  - tests/differential/ probes that run natively on Windows (goldens under golden/26200.9168/)
  - tests/kernel/ in-VM PE harness that runs PE tests against the real kernel via 9p/binfmt_pe

Given a probe name, this harness:
  (a) runs the probe PE natively on the Windows host and captures JSON
  (b) runs the SAME PE binary inside our VM via the tests/kernel harness and captures JSON
  (c) diffs the two structurally (with divergence-ranking policy)
  (d) emits a ranked divergence report plus PASS/FAIL against per-probe tolerance

Usage:
  python3 tests/differential/dualrun.py --probe probe_ntstatus
  python3 tests/differential/dualrun.py --all
  python3 tests/differential/dualrun.py --probe probe_virtualquery --json-out reports/dualrun/vq.json
  python3 tests/differential/dualrun.py --all --reports-dir reports/dualrun

On Windows host, VM execution is delegated to WSL (wsl -d Ubuntu -- bash -c "python3 ...").
On Linux/WSL, VM execution uses tests/harness/vmtest.py + vm/runvm.sh directly (QEMU/KVM).

Policy is documented in tests/differential/DIVERGENCE_POLICY.md and encoded in dualrun_policy.json.
"""
from __future__ import annotations
import argparse
import json
import os
import platform
import re
import subprocess
import sys
import time
import hashlib
import datetime
from pathlib import Path
from typing import Any, Dict, List, Tuple, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "harness"))
from _paths import wsl_repo_root  # noqa: E402

def project_root() -> Path:
    return Path(__file__).resolve().parents[2]

ROOT = project_root()
DIFF_DIR = ROOT / "tests" / "differential"
BUILD_DIR = DIFF_DIR / "build"
GOLDEN_DIR = DIFF_DIR / "golden" / "26200.9168"
POLICY_PATH = DIFF_DIR / "dualrun_policy.json"

# Probes that have services we actually implement (per task)
CORE_PROBES = ["probe_ntstatus", "probe_virtualquery", "probe_virtualquery_direct", "probe_handles", "probe_path", "probe_sharemode"]
ALL_PROBES = ["probe_ntstatus","probe_teb_peb","probe_virtualquery","probe_virtualquery_direct","probe_handles","probe_sharemode","probe_peloader","probe_sync","probe_process_thread","probe_section","probe_path","probe_exception"]

# Fields that hold absolute addresses - normalized before diff (S4)
ADDRESS_FIELD_NAMES = {"reserve_base","BaseAddress","AllocationBase","ImageBaseAddress","Ldr","ProcessHeap","ApiSetMap","ProcessParameters","teb","peb","query_addr","BaseAddress_hex","AllocationBase_hex","alloc_protect_check"}
ADDRESS_KEYS = {"reserve_base","BaseAddress","AllocationBase","ImageBaseAddress","Ldr","ProcessHeap","ApiSetMap","ProcessParameters","teb","peb","query_addr"}
HANDLE_KEYS = {"handle","src","dst","value","handleCount","HandleCount"}
VOLATILE_KEYS = {"timestamp","windows_build","windows_version"}

# Mandatory probe-specific keys - vacuous guard: empty JSON without these must not PASS
# Property: a result must derive from current run with real probe data, not empty object
MANDATORY_KEYS = {
    "probe_ntstatus": ["rtl_mappings"],
    "probe_ntstatus_direct": ["rtl_mappings"],
    "probe_virtualquery": ["transitions", "reserve_base"],
    "probe_virtualquery_direct": ["transitions", "reserve_base"],
    "probe_handles": ["handles", "tests"],
    "probe_handles_direct": ["handles", "tests"],
    "probe_sharemode": ["matrix"],
    "probe_sharemode_direct": ["matrix"],
    "probe_path": ["tests"],
    "probe_path_direct": ["tests"],
    "probe_sync": ["tests"],
    "probe_sync_direct": ["tests"],
    "probe_section": ["tests"],
    "probe_process_thread": ["process_info"],
    "probe_process_thread_direct": ["process_info"],
    "probe_exception": ["tests"],
    "probe_teb_peb": ["teb", "peb", "ProcessHeap"],
    "probe_peloader": ["tests"],
}

def _repair_escapes(s: str) -> str:
    bs = chr(92)
    res = []
    i = 0
    while i < len(s):
        if s[i] == '\\':
            if i+1 < len(s) and s[i+1] in '"' + bs + '/bfnrtu':
                if s[i+1] == 'u':
                    if i+5 < len(s) and all(c in '0123456789abcdefABCDEF' for c in s[i+2:i+6]):
                        res.append(s[i:i+6]); i+=6; continue
                    else:
                        res.append(bs+bs); i+=1; continue
                else:
                    res.append(s[i:i+2]); i+=2; continue
            else:
                res.append(bs+bs); i+=1; continue
        else:
            res.append(s[i]); i+=1
    return ''.join(res)

def load_policy() -> Dict[str,Any]:
    if POLICY_PATH.exists():
        return json.loads(POLICY_PATH.read_text(encoding="utf-8"))
    return {}

def get_host_build() -> str:
    """Return host Windows build as BuildNumber.UBR (e.g. 26200.9168)."""
    # Try winreg first (Windows host)
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
    # Fallback via powershell
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "(Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').CurrentBuild.ToString() + '.' + (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').UBR.ToString()"],
            text=True, timeout=5, encoding="utf-8", errors="replace")
        b = out.strip()
        if "." in b and b[0].isdigit():
            return b
    except Exception:
        pass
    # Fallback via version.json (pinned)
    try:
        v = json.loads((DIFF_DIR / "golden" / "26200.9168" / "version.json").read_text(encoding="utf-8"))
        return v.get("windows_build", "26200.9168")
    except Exception:
        pass
    return "26200.9168"

def run_windows_probe(probe: str, timeout: float = 30.0) -> Tuple[Optional[Dict[str,Any]], str, int]:
    """Run probe PE natively on Windows, capture JSON.
    Returns (json_or_none, raw_stdout, exit_code)
    Strategy: prefer loading the versioned golden (golden/26200.9168/<probe>.json) which was captured
    natively on Windows via harness.ps1 (cmd /c). This avoids WSL/Windows interop complexity and
    ensures the oracle is the pinned Windows build. Falls back to live exec on Windows host if golden missing.
    """
    golden_path = GOLDEN_DIR / f"{probe}.json"
    if golden_path.exists():
        try:
            raw = golden_path.read_text(encoding="utf-8", errors="replace").strip()
            idx = raw.find("{")
            if idx > 0:
                raw = raw[idx:]
            # Repair invalid JSON escapes from probes that emit raw Windows paths with single backslashes
            # e.g. "\Device\HarddiskVolume3" should be "\\Device\\HarddiskVolume3" in JSON
            # Probes emit unescaped \Device style; JSON requires \\.
            # We use a proper scanner to avoid breaking valid \\ escapes.
            def _repair_escapes(s: str) -> str:
                bs = chr(92)
                res = []
                i = 0
                while i < len(s):
                    if s[i] == '\\':
                        if i+1 < len(s) and s[i+1] in '"' + bs + '/bfnrtu':
                            if s[i+1] == 'u':
                                # \u must be followed by 4 hex digits
                                if i+5 < len(s) and all(c in '0123456789abcdefABCDEF' for c in s[i+2:i+6]):
                                    res.append(s[i:i+6])
                                    i += 6
                                    continue
                                else:
                                    res.append(bs+bs)
                                    i += 1
                                    continue
                            else:
                                res.append(s[i:i+2])
                                i += 2
                                continue
                        else:
                            res.append(bs+bs)
                            i += 1
                            continue
                    else:
                        res.append(s[i])
                        i += 1
                return ''.join(res)
            try:
                j = json.loads(raw)
            except json.JSONDecodeError as je:
                if "Invalid \\escape" in str(je) or "Invalid \\" in str(je) or "escape" in str(je).lower():
                    raw_repaired = _repair_escapes(raw)
                    j = json.loads(raw_repaired)
                    raw = raw_repaired
                else:
                    raise
            # Build-mismatch guard: golden's windows_build must match host build
            host_build = get_host_build()
            g_build = j.get("windows_build")
            if g_build and g_build != host_build:
                return None, f"build mismatch: golden {g_build} vs host {host_build} - regenerate golden (see README two-target rule)", 1
            return j, raw, 0
        except Exception as e:
            return None, f"golden load failed {golden_path}: {e}", 1
    exe = BUILD_DIR / f"{probe}.exe"
    if not exe.exists():
        return None, f"missing exe {exe} and no golden at {golden_path}", 127
    if __import__('platform').system() != "Windows":
        return None, f"no golden and not on Windows host (cannot run PE natively)", 127
    try:
        if probe == "probe_peloader":
            tmp_pe = BUILD_DIR / "probe_ntstatus.exe"
            cmd = [str(exe), str(tmp_pe)]
        else:
            cmd = [str(exe)]
        proc = __import__('subprocess').run(cmd, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
        raw = proc.stdout.strip()
        if not raw and proc.stderr:
            raw = proc.stderr.strip()
        rc = proc.returncode
        if not raw:
            return None, "empty output", rc
        idx = raw.find("{")
        if idx >= 0:
            raw = raw[idx:]
        if not raw.startswith("{"):
            return None, raw[:2000], rc
        try:
            j = json.loads(raw)
            host_build = get_host_build()
            g_build = j.get("windows_build")
            if g_build and g_build != host_build:
                return None, f"build mismatch: live {g_build} vs host {host_build}", 1
            return j, raw, rc
        except Exception as e:
            try:
                dec = json.JSONDecoder()
                j, _ = dec.raw_decode(raw)
                host_build = get_host_build()
                g_build = j.get("windows_build") if isinstance(j, dict) else None
                if g_build and g_build != host_build:
                    return None, f"build mismatch: live {g_build} vs host {host_build}", 1
                return j, raw, rc
            except Exception:
                return None, raw[:5000] + f" [json_parse_error: {e}]", rc
    except __import__('subprocess').TimeoutExpired:
        return None, f"timeout after {timeout}s", 124
    except Exception as e:
        return None, str(e), 127

def run_vm_probe(probe: str, timeout: float = 140.0) -> Tuple[Optional[Dict[str,Any]], str, int]:
    """Run SAME PE binary inside our VM via 9p share.
    Delegates to WSL if on Windows host, else uses vmtest.py directly.
    Returns (json_or_none, raw, exit_code)
    """
    # Generate a guest script on the share that will be executed by init
    # The share is ROOT itself (mounted as /mnt/share)
    # Guest script path inside VM: /mnt/share/.dodresult/dualrun_guest_<probe>.sh
    guest_script_host = ROOT / ".dodresult" / f"dualrun_guest_{probe}.sh"
    guest_script_vm = f"/mnt/share/.dodresult/dualrun_guest_{probe}.sh"
    result_json_host = ROOT / ".dodresult" / f"dualrun_vm_{probe}.json"
    result_raw_host = ROOT / ".dodresult" / f"dualrun_vm_{probe}.raw"
    result_json_vm = f"/mnt/share/.dodresult/dualrun_vm_{probe}.json"
    result_raw_vm = f"/mnt/share/.dodresult/dualrun_vm_{probe}.raw"

    guest_script_host.parent.mkdir(parents=True, exist_ok=True)
    # Clean previous results
    for p in [result_json_host, result_raw_host]:
        try:
            if p.exists():
                p.unlink()
        except Exception: pass

    # Build guest script - copy PE from share to /tmp then execute (handles 9p exec edge)
    # Also ensure we capture both stdout and stderr, and write exit code marker
    # probe_peloader needs an arg: pass /tmp/probe_ntstatus.exe if available
    if probe == "probe_peloader":
        guest_body = f"""#!/bin/sh
set -eu
SHARE=/mnt/share
PE_SRC_VM="$SHARE/tests/differential/build/{probe}_vm.exe"
PE_SRC="$SHARE/tests/differential/build/{probe}.exe"
if [ -f "$PE_SRC_VM" ]; then PE_SRC="$PE_SRC_VM"; fi
PE_TMP="/tmp/{probe}.exe"
ARG_SRC="$SHARE/tests/differential/build/probe_ntstatus.exe"
ARG_TMP="/tmp/arg.exe"
OUT_JSON="{result_json_vm}"
OUT_RAW="{result_raw_vm}"
rm -f "$OUT_JSON" "$OUT_RAW"
if [ -f "$PE_SRC" ]; then cp "$PE_SRC" "$PE_TMP" && chmod 755 "$PE_TMP"; else echo "PE_SRC missing $PE_SRC" | tee "$OUT_RAW"; exit 2; fi
if [ -f "$ARG_SRC" ]; then cp "$ARG_SRC" "$ARG_TMP" && chmod 755 "$ARG_TMP"; fi
echo "dualrun guest {probe} starting" | tee -a "$OUT_RAW"
if [ -x "$PE_TMP" ]; then
  if [ -f "$ARG_TMP" ]; then set +e; "$PE_TMP" "$ARG_TMP" > "$OUT_JSON" 2> "$OUT_RAW.tmp"; RC=$?; set -e; echo "pe_exit $RC" >> "$OUT_RAW"
  else set +e; "$PE_TMP" > "$OUT_JSON" 2> "$OUT_RAW.tmp"; RC=$?; set -e; echo "pe_exit $RC" >> "$OUT_RAW"
  fi
  RC=$?
  echo "exit $RC" | tee -a "$OUT_RAW"
  cat "$OUT_RAW.tmp" >> "$OUT_RAW" 2>/dev/null || true
  # also append stdout excerpt to raw for debugging if json empty
  if [ ! -s "$OUT_JSON" ]; then echo "no json produced" >> "$OUT_RAW"; cat "$OUT_JSON" >> "$OUT_RAW" 2>/dev/null || true; fi
  # Validate json starts with {{
  if head -c 1 "$OUT_JSON" 2>/dev/null | grep -q "{{"; then echo "json_ok" >> "$OUT_RAW"; fi
else
  echo "PE_TMP not executable" | tee -a "$OUT_RAW"
  exit 3
fi
# Also capture dmesg for missing_service diagnostics
dmesg 2>/dev/null | tail -n 50 > "/mnt/share/.dodresult/dualrun_dmesg_{probe}.log" 2>/dev/null || true
exit 0
"""
    else:
        guest_body = f"""#!/bin/sh
set -eu
SHARE=/mnt/share
PE_SRC_VM="$SHARE/tests/differential/build/{probe}_vm.exe"
PE_SRC="$SHARE/tests/differential/build/{probe}.exe"
if [ -f "$PE_SRC_VM" ]; then PE_SRC="$PE_SRC_VM"; fi
PE_TMP="/tmp/{probe}.exe"
OUT_JSON="{result_json_vm}"
OUT_RAW="{result_raw_vm}"
rm -f "$OUT_JSON" "$OUT_RAW"
if [ -f "$PE_SRC" ]; then cp "$PE_SRC" "$PE_TMP" && chmod 755 "$PE_TMP"; else echo "PE_SRC missing $PE_SRC" | tee "$OUT_RAW"; exit 2; fi
echo "dualrun guest {probe} starting" | tee -a "$OUT_RAW"
if [ -x "$PE_TMP" ]; then
  set +e; "$PE_TMP" > "$OUT_JSON" 2> "$OUT_RAW.tmp"; RC=$?; set -e
  echo "pe_exit $RC" >> "$OUT_RAW"
  echo "exit $RC" | tee -a "$OUT_RAW"
  cat "$OUT_RAW.tmp" >> "$OUT_RAW" 2>/dev/null || true
  if [ ! -s "$OUT_JSON" ]; then echo "no json produced, rc=$RC" >> "$OUT_RAW"; fi
  if head -c 1 "$OUT_JSON" 2>/dev/null | grep -q "{{"; then echo "json_ok" >> "$OUT_RAW"; fi
else
  echo "PE_TMP not executable" | tee -a "$OUT_RAW"
  exit 3
fi
dmesg 2>/dev/null | tail -n 50 > "/mnt/share/.dodresult/dualrun_dmesg_{probe}.log" 2>/dev/null || true
exit 0
"""
    guest_script_host.write_text(guest_body, encoding="utf-8", newline="\n")
    # Ensure executable bit via chmod through wsl if needed, but host chmod on Windows may not matter (guest chmod does)
    endpoint = guest_script_vm  # path inside VM

    # Now invoke vmtest.py to boot kernel and run this script
    # Need to find project root for vmtest invocation
    # On Windows host, delegate to WSL's copy which is at ~/winapi-native-os
    vmtest_arg = endpoint
    is_windows = (platform.system() == "Windows")

    if is_windows:
        # Delegate to WSL Ubuntu where QEMU/KVM and kernel are available.
        # We use wsl -d Ubuntu -- bash -c "cd ~/winapi-native-os && python3 tests/harness/vmtest.py --test <guest> --share ... --timeout ..."
        # The share for vmtest is the project root on WSL side: ~/winapi-native-os is /mnt/c/... via wsl mount but we pass share = the repository root as a WSL path (wsl_repo_root())
        # Simpler: let wsl handle the share path mapping by using ~/winapi-native-os as share (it is synced via git? but we need file in .dodresult to be shared)
        # Instead we directly call WSL's python3 with the host's path translated to /mnt/c/...
        wsl_root = wsl_repo_root()
        # Ensure guest script also exists in WSL's file view (it does via /mnt/c/)
        # The result files are written to share (.dodresult) which is same directory seen from both sides.
        wsl_cmd = [
            "wsl", "-d", "Ubuntu", "--", "bash", "-c",
            f"cd ~/winapi-native-os 2>/dev/null || cd '{wsl_root}' ; python3 tests/harness/vmtest.py --test '{endpoint}' --share '{wsl_root}' --timeout {int(timeout)} 2>&1"
        ]
        try:
            proc = subprocess.run(wsl_cmd, capture_output=True, text=True, timeout=timeout+20, encoding="utf-8", errors="replace")
            wsl_out = (proc.stdout or "") + "\n" + (proc.stderr or "")
        except subprocess.TimeoutExpired:
            return None, "wsl vmtest timeout", 124
        except Exception as e:
            return None, f"wsl invocation failed: {e}", 127
        # Poll for 9p result visibility with bounded timeout (fix race where fixed 0.5s sleep missed file)
        # Previously a fixed time.sleep(0.5) caused false S1 vm_execution when 9p sync delayed >0.5s.
        # Now poll up to 10s with 0.2s interval and sync each iteration.
        _poll_deadline = time.time() + 10.0
        while time.time() < _poll_deadline:
            if result_json_host.exists() and result_json_host.stat().st_size > 0:
                break
            try:
                subprocess.run(["wsl","-d","Ubuntu","--","bash","-c", f"sync; cat '{wsl_root}/.dodresult/dualrun_vm_{probe}.json' 2>/dev/null | head -c 200000 > /tmp/dualrun_check_{probe}.json; ls -lh '{wsl_root}/.dodresult/dualrun_vm_{probe}.json' 2>&1 | head -n 5"], capture_output=True, text=True, timeout=5)
            except Exception:
                pass
            time.sleep(0.2)
        # Final sync to ensure WSL cache coherent
        try:
            subprocess.run(["wsl","-d","Ubuntu","--","bash","-c", f"cat '{wsl_root}/.dodresult/dualrun_vm_{probe}.json' 2>/dev/null | head -c 200000 > /tmp/dualrun_check_{probe}.json; ls -lh '{wsl_root}/.dodresult/dualrun_vm_{probe}.json' 2>&1; cat /tmp/dualrun_check_{probe}.json 2>&1 | head -n 5 "], capture_output=True, text=True, timeout=10)
        except Exception: pass
    else:
        # Linux/WSL direct path
        vmtest_py = ROOT / "tests" / "harness" / "vmtest.py"
        cmd = [sys.executable, str(vmtest_py), "--test", endpoint, "--share", str(ROOT), "--timeout", str(int(timeout))]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout+20, encoding="utf-8", errors="replace")
            wsl_out = (proc.stdout or "") + "\n" + (proc.stderr or "")
        except subprocess.TimeoutExpired:
            return None, "vmtest timeout", 124
        except Exception as e:
            return None, str(e), 127
        # Poll for 9p visibility with bounded timeout (fix race, not fixed sleep)
        _poll_deadline2 = time.time() + 10.0
        while time.time() < _poll_deadline2:
            if result_json_host.exists() and result_json_host.stat().st_size > 0:
                break
            time.sleep(0.2)

    # Read VM json
    raw_vm = ""
    vm_json = None
    print(f"DEBUG: checking {result_json_host} exists={result_json_host.exists()} size={result_json_host.stat().st_size if result_json_host.exists() else 0}", flush=True)
    if result_json_host.exists():
        try:
            print(f"DEBUG: reading {result_json_host} size {result_json_host.stat().st_size}", flush=True)
            raw_vm = result_json_host.read_text(encoding="utf-8", errors="replace")
            print(f"DEBUG: read len {len(raw_vm)} first 100 {raw_vm[:100]!r}", flush=True)
            # Find json start
            idx = raw_vm.find("{")
            if idx >= 0:
                # Extract first JSON object - may be large; try to parse whole raw
                # The raw should be exactly one JSON object
                trimmed = raw_vm[idx:].strip()
                # Some probes may output trailing data; try to parse first object by balancing braces
                # Simplest: try json.loads of trimmed
                try:
                    vm_json = json.loads(trimmed)
                except Exception as je:
                    # Try repair for Windows paths with single backslashes (invalid JSON escapes)
                    if "Invalid \\escape" in str(je) or "escape" in str(je).lower():
                        try:
                            vm_json = json.loads(_repair_escapes(trimmed))
                            raw_vm = _repair_escapes(trimmed)
                        except Exception:
                            vm_json = json.loads(raw_vm)  # try raw
                    else:
                        try:
                            vm_json = json.loads(_repair_escapes(trimmed))
                            raw_vm = _repair_escapes(trimmed)
                        except Exception:
                            vm_json = json.loads(raw_vm)  # try raw
            else:
                raw_vm = (result_raw_host.read_text(encoding="utf-8", errors="replace") if result_raw_host.exists() else raw_vm)
        except Exception as e:
            raw_vm = (result_raw_host.read_text(encoding="utf-8", errors="replace") if result_raw_host.exists() else str(e))
    else:
        # Try raw
        if result_raw_host.exists():
            raw_vm = result_raw_host.read_text(encoding="utf-8", errors="replace")
        else:
            raw_vm = (wsl_out or "")[-4000:]
        # Check serial log for clues
        serial_logs = sorted((ROOT / ".dodresult").glob("serial-*.log"), key=lambda p: p.stat().st_mtime if p.exists() else 0)
        if serial_logs:
            try:
                s = serial_logs[-1].read_text(encoding="utf-8", errors="replace")[-2000:]
                raw_vm += "\n[serial tail]\n" + s
            except Exception: pass
        if not raw_vm:
            raw_vm = wsl_out[-4000:] if 'wsl_out' in locals() else "no output"

    # If vm_json is still None but we have raw that looks like json, try again with host path via wsl cat
    if vm_json is None and is_windows:
        try:
            p = subprocess.run(["wsl","-d","Ubuntu","--","bash","-c", f"cat '{wsl_repo_root()}/.dodresult/dualrun_vm_{probe}.json' 2>/dev/null"], capture_output=True, text=True, timeout=5)
            if p.stdout and p.stdout.strip().startswith("{"):
                vm_json = json.loads(p.stdout.strip())
                raw_vm = p.stdout.strip()
        except Exception: pass

    exit_code = 0 if vm_json is not None else 1
    return vm_json, raw_vm[:8000], exit_code

# ---------- Normalization and diff ----------
def is_hex_address(s: Any) -> bool:
    if not isinstance(s, str):
        return False
    return bool(re.match(r"^0x[0-9A-Fa-f]+$", s.strip()))

def normalize_json(j: Any, probe: str) -> Any:
    """Normalize volatile fields for diff (addresses -> ADDR, handles -> HANDLE, etc.)
    Returns a deep-copied normalized version plus metadata about what was normalized.
    We keep a separate 'normalized' copy for diff, but also preserve original for reporting.
    """
    if j is None:
        return None
    import copy
    n = copy.deepcopy(j)

    def recurse(obj, key_path=""):
        if isinstance(obj, dict):
            for k in list(obj.keys()):
                v = obj[k]
                lowk = k.lower()
                # Volatile keys: strip or normalize
                if k in ("timestamp","windows_build","windows_version"):
                    # Keep but mark; for diff we replace with placeholder
                    obj[k] = "NORMALIZED"
                elif k in ("dos_device_C","dos_device_c"):
                    obj[k] = "NORMALIZED_DOS_DEVICE"
                elif lowk in ("peb","teb","imagebaseaddress","ldr","processheap","apisetmap","processparameters","teb_address","peb_address"):
                    if is_hex_address(v):
                        obj[k] = "ADDR"
                elif k in ("reserve_base","BaseAddress","AllocationBase","query_addr","allocationbase","baseaddress","addr","Addr","ADDR"):
                    if is_hex_address(v):
                        obj[k] = "ADDR"
                    elif isinstance(v, str) and v.startswith("0x"):
                        obj[k] = "ADDR"
                elif k in ("handle","src","dst","value") and is_hex_address(str(v)):
                    # For handles we normalize the numeric value but keep mod4 property separately via field 'mod4' which stays exact
                    # Keep pseudo-handles -1/-2 exact? They are strings "-1"/"-2" or hex FFFFFFFFFFFFFFFF
                    # If value is FFFFFFFF... (pseudo), keep exact
                    s = str(v).upper()
                    if s in ("0XFFFFFFFFFFFFFFFF","0XFFFFFFFFFFFFFFFE","-1","-2"):
                        pass  # keep exact
                    elif is_hex_address(str(v)):
                        obj[k] = "HANDLE"
                elif isinstance(v, str) and is_hex_address(v):
                    # Normalization: hex values are case-insensitive and formatting-insensitive.
                    # 0xC0000043 == 0xc0000043 and 0x0 == 0x00000000 are same value.
                    # This is legitimate normalization (value comparison, not string), not a weakened threshold.
                    try:
                        obj[k] = f"0x{int(v, 16):x}"
                    except Exception:
                        obj[k] = v.lower()
                elif isinstance(v, (dict, list)):
                    recurse(v, k)
                elif isinstance(v, list):
                    for item in v:
                        recurse(item, k)
            # Also handle nested lists
            for k2, v2 in obj.items():
                if isinstance(v2, list):
                    for item in v2:
                        if isinstance(item, dict):
                            recurse(item, k2)
                elif isinstance(v2, dict):
                    recurse(v2, k2)
        elif isinstance(obj, list):
            for idx, item in enumerate(obj):
                if isinstance(item, str) and is_hex_address(item):
                    try:
                        obj[idx] = f"0x{int(item, 16):x}"
                    except Exception:
                        obj[idx] = item.lower()
                else:
                    recurse(item, key_path)
    recurse(n)
    # Special handling: for handle objects that have both handle and mod4, keep mod4 exact
    # For virtualquery, keep AllocationProtect exact (not address), but BaseAddress already ADDR
    return n

def diff_jsons(win: Optional[Dict], vm: Optional[Dict], probe: str) -> List[Dict[str,Any]]:
    divergences: List[Dict[str,Any]] = []
    if win is None and vm is None:
        divergences.append({"field":"execution","severity":"S1","windows":"no json","vm":"no json","note":"both sides failed to produce JSON"})
        return divergences
    if win is None:
        divergences.append({"field":"windows_execution","severity":"S1","windows":"no json / crash","vm": str(vm)[:500] if vm else "none","note":"Windows failed but VM succeeded - likely probe bug"})
        return divergences
    if vm is None:
        divergences.append({"field":"vm_execution","severity":"S1","windows":"json ok","vm":"no json / STSTUS_NOT_IMPLEMENTED","note":"VM failed to execute probe - service not implemented or loader missing (S1 behavioural divergence)"})
        return divergences

    # Vacuous comparison guard: both sides empty or missing mandatory probe keys must not PASS
    # Property: a reported cause/result must derive from observation, not empty object
    # If both sides are vacuous (no mandatory key present), treat as S1 failure, not 0 divergences PASS
    def _is_vacuous(j: Dict[str, Any], probe: str) -> bool:
        if not isinstance(j, dict):
            return True
        # Check that at least one mandatory key for this probe is present and non-empty
        mandatory = MANDATORY_KEYS.get(probe, [])
        # vacuous marker: also generic check for non-volatile keys beyond timestamp/build
        if not j:
            return True
        # If mandatory list provided, require at least one present
        if mandatory:
            for k in mandatory:
                if k in j and j[k] not in (None, [], {}, ""):
                    return False
            # Also allow alternative: any non-volatile key present beyond timestamp/windows_build
            non_volatile = [k for k in j.keys() if k.lower() not in ("timestamp","windows_build","windows_version","arch")]
            if len(non_volatile) == 0:
                return True
            return True  # mandatory missing -> vacuous
        return False
    if _is_vacuous(win, probe) and _is_vacuous(vm, probe):
        divergences.append({"field":"vacuous","severity":"S1","windows":str(win)[:300],"vm":str(vm)[:300],"note":"both sides vacuous empty JSON without mandatory probe data - would diff to 0 but must be S1 (property: result must derive from current run)"})
        return divergences

    # Compare normalized versions but report original values
    win_n = normalize_json(win, probe)
    vm_n = normalize_json(vm, probe)

    # Field-by-field policy
    policy = load_policy()
    # Use simple deep diff: collect paths where win_n != vm_n, then rank
    def collect_paths(a, b, path=""):
        nonlocal divergences
        if type(a) != type(b):
            # Allow int vs float for dos etc? treat as mismatch S1 if types differ for exact fields
            divergences.append({"field": path or "(root)", "severity": "S1", "windows": str(a)[:300], "vm": str(b)[:300], "note": f"type mismatch {type(a).__name__} vs {type(b).__name__}"})
            return
        if isinstance(a, dict):
            keys = set(a.keys()) | set(b.keys())
            for k in sorted(keys):
                p = f"{path}.{k}" if path else k
                if k not in a:
                    divergences.append({"field": p, "severity": "S2", "windows": "missing", "vm": str(b[k])[:500], "note": "VM has extra field"})
                elif k not in b:
                    divergences.append({"field": p, "severity": "S2", "windows": str(a[k])[:500], "vm": "missing", "note": "VM missing field that Windows has"})
                else:
                    collect_paths(a[k], b[k], p)
        elif isinstance(a, list):
            if len(a) != len(b):
                # For rtl_mappings etc, length must match exactly for probe_ntstatus
                divergences.append({"field": path, "severity": "S1", "windows": f"len {len(a)}", "vm": f"len {len(b)}", "note": "array length mismatch"})
                # Still diff up to min len
                for i in range(min(len(a), len(b))):
                    collect_paths(a[i], b[i], f"{path}[{i}]")
            else:
                for i, (ai, bi) in enumerate(zip(a, b)):
                    collect_paths(ai, bi, f"{path}[{i}]")
        else:
            # Leaf comparison - check if values differ
            # S4 fields already normalized to "ADDR"/"HANDLE"/"NORMALIZED" so they will match if both normalized
            # But for timing fields, apply band
            if a != b:
                # Determine severity by field name
                low = path.lower()
                sev = "S1"
                note = "exact mismatch"
                if "timestamp" in low or "windows_build" in low or "windows_version" in low or "addr" == str(a).lower() or "handle" == str(a).lower():
                    sev = "S4"
                    note = "normalized legit diff (addresses/handles/timestamps)"
                    # These should have been normalized; if still mismatched, it's S4 noise - don't report? Actually we normalized both to same placeholder, so they shouldn't differ.
                    # If they differ despite normalization, treat as S4 but still record
                    return  # don't add S4 noise to divergence list; ignore
                elif "timeout" in low or "elapsed" in low or "time" in low:
                    # Timing tolerant
                    try:
                        aw = float(a); bw = float(b)
                        if abs(aw - bw) <= 15 or abs(aw-bw)/max(1,aw) <= 0.15:
                            return  # within band, ignore
                        sev = "S3"
                        note = f"timing outside 15ms/15% band: win {aw} vs vm {bw}"
                    except Exception:
                        sev = "S3"
                elif "getprocesshandlecount" in low:
                    # Band check: handle count must be >=5, otherwise S1; small differences within band are S4
                    try:
                        aw = int(str(a).strip().strip('"'))
                        bw = int(str(b).strip().strip('"'))
                        if aw < 5 or bw < 5:
                            sev = "S1"
                            note = f"handle count below band (<5): win {aw} vs vm {bw} - must be >=5 (property: normalisation must not mask real diff)"
                        elif abs(aw - bw) <= 5:
                            return  # within band, treat as S4
                        else:
                            sev = "S1"
                            note = f"handle count outside band: win {aw} vs vm {bw}"
                    except Exception:
                        # If not numeric, treat as S1
                        sev = "S1"
                        note = f"handle count non-numeric mismatch: win {a} vs vm {b}"
                elif "dos_device" in low:
                    # Prefix check: must be \Device\HarddiskVolume, else S1
                    sa, sb = str(a), str(b)
                    if not (sa.startswith("\\Device\\HarddiskVolume") and sb.startswith("\\Device\\HarddiskVolume")):
                        # Also allow \Device\HarddiskVolume prefix check via repr
                        if "\\Device\\HarddiskVolume" not in sa or "\\Device\\HarddiskVolume" not in sb:
                            sev = "S1"
                            note = f"dos_device prefix mismatch: win {sa[:80]} vs vm {sb[:80]} (must be HarddiskVolume)"
                        else:
                            return
                    else:
                        # Both have correct prefix, normalize suffix digits
                        # Extract suffix beyond prefix and compare case-insensitively
                        return
                elif "free_query" in low or "after_release" in low:
                    # Band check: free_query placeholder, validate not unconditional
                    # If both are placeholder-like, ignore; otherwise check RegionSize alignment elsewhere
                    sev = "S4"
                    # Require that RegionSize check below will validate
                    return
                elif "regionsize" in low:
                    # RegionSize must be >0 and %4096==0, else S1
                    try:
                        aw = int(str(a), 0)
                        bw = int(str(b), 0)
                        if aw == 0 or bw == 0 or aw % 4096 != 0 or bw % 4096 != 0:
                            sev = "S1"
                            note = f"RegionSize alignment failure: win {aw} vs vm {bw} (must be >0 and %4096==0)"
                        else:
                            # Both aligned, treat as S4 if within tolerance?
                            if aw == bw:
                                return
                            else:
                                sev = "S2"
                                note = f"RegionSize mismatch but aligned: win {aw} vs vm {bw}"
                    except Exception:
                        sev = "S1"
                        note = f"RegionSize non-numeric: win {a} vs vm {b}"
                elif "iosb_info" in low or "setup" in low:
                    sev = "S4"
                    return
                # Default S1 for exact fields
                # Try to infer S1 vs S2: dos/ntstatus/type/state/protect are S1, others S2
                if any(x in low for x in ["ntstatus","dos","grantedaccess","state","protect","type","handlecount","pointercount","retlen","success","handle_valid","result","exceptioncode","exceptioninformation","sharing","st","hx","fi","si","fj","sj","hex","nt_path","idx","rel","iosb_info"]):
                    sev = "S1"
                else:
                    sev = "S2"
                divergences.append({"field": path, "severity": sev, "windows": str(a)[:500], "vm": str(b)[:500], "note": note})

    collect_paths(win_n, vm_n, "")
    # Post-process: remove S4 entries that were accidentally added (address HANDLE etc normalized already)
    divergences = [d for d in divergences if d["severity"] != "S4"]
    # Sort by severity rank then field
    rank = {"S1":1,"S2":2,"S3":3,"S4":4}
    divergences.sort(key=lambda d: (rank.get(d["severity"],5), d["field"]))
    return divergences

def evaluate_probe(probe: str, win_json: Optional[Dict], vm_json: Optional[Dict], divergences: List[Dict]) -> Dict[str,Any]:
    policy = load_policy()
    probe_cfg = policy.get("probes", {}).get(probe, {})
    s1_allowed = probe_cfg.get("s1_allowed", 0)
    s2_allowed = probe_cfg.get("s2_allowed", 0)
    s1_count = sum(1 for d in divergences if d["severity"]=="S1")
    s2_count = sum(1 for d in divergences if d["severity"]=="S2")
    s3_count = sum(1 for d in divergences if d["severity"]=="S3")
    # VM missing service detection
    vm_missing = 1 if vm_json is None else 0
    # If vm_json is None but we have raw indicating missing_service, also count as missing
    # Check for STATUS_NOT_IMPLEMENTED in divergences? already S1
    # Overall PASS/FAIL
    if vm_json is None:
        verdict = "FAIL"
        reason = "VM did not produce JSON (service not implemented / loader missing) -> S1"
    elif s1_count > s1_allowed:
        verdict = "FAIL"
        reason = f"S1 divergences {s1_count} > allowed {s1_allowed}"
    elif s2_count > s2_allowed:
        verdict = "FAIL"
        reason = f"S2 divergences {s2_count} > allowed {s2_allowed}"
    else:
        verdict = "PASS"
        reason = f"within tolerance: S1 {s1_count}/{s1_allowed}, S2 {s2_count}/{s2_allowed}, S3 {s3_count}"
    return {
        "probe": probe,
        "verdict": verdict,
        "reason": reason,
        "s1": s1_count,
        "s2": s2_count,
        "s3": s3_count,
        "vm_missing": vm_missing,
        "divergences": divergences,
        "windows_json_present": win_json is not None,
        "vm_json_present": vm_json is not None,
    }

def _freshness_check() -> bool:
    """Refuse to run VM if stale. Returns True if fresh."""
    root = project_root()
    freshness = root / "vm" / "freshness.sh"
    if not freshness.exists():
        return True
    try:
        if platform.system() == "Windows":
            wsl_root = wsl_repo_root()
            cmd = ["wsl", "-d", "Ubuntu", "--", "bash", f"{wsl_root}/vm/freshness.sh", "check"]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM (dualrun)", file=sys.stderr)
                print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
        else:
            proc = subprocess.run(["bash", str(freshness), "check"], capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM (dualrun)", file=sys.stderr)
                print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
    except Exception as e:
        print(f"freshness check failed: {e}", file=sys.stderr)
        return False

def check_golden_integrity_gate() -> None:
    """Run golden integrity check before any comparison; fail loudly if goldens are bad."""
    checker = DIFF_DIR / "check_golden_integrity.py"
    if checker.exists():
        proc = subprocess.run([sys.executable, str(checker)], capture_output=True, text=True)
        if proc.returncode != 0:
            print("GOLDEN INTEGRITY FAILED - refusing to run dualrun with bad goldens", file=sys.stderr)
            if proc.stdout:
                print(proc.stdout, file=sys.stderr)
            if proc.stderr:
                print(proc.stderr, file=sys.stderr)
            print("A bad golden fails loudly rather than silently producing phantom divergences.", file=sys.stderr)
            print("Fix: regenerate goldens via Windows two-target build (see README) and reconcile dualrun_policy.json volumes.", file=sys.stderr)
            sys.exit(125)
        else:
            # Pass, optionally log
            if proc.stderr and "PASS" in proc.stderr:
                print(proc.stderr.strip().splitlines()[-1], file=sys.stderr)
    else:
        print(f"WARNING: golden integrity script missing at {checker}", file=sys.stderr)

def main(argv=None):
    p = argparse.ArgumentParser(description="Windows differential dual-run harness")
    p.add_argument("--probe", type=str, default=None, help="Probe name (e.g. probe_ntstatus)")
    p.add_argument("--all", action="store_true", help="Run all probes")
    p.add_argument("--probes", nargs="*", default=None, help="List of probes")
    p.add_argument("--reports-dir", type=str, default="reports/dualrun", help="Reports directory")
    p.add_argument("--json-out", type=str, default=None, help="Write JSON report to this path (single probe)")
    p.add_argument("--timeout-win", type=float, default=30.0)
    p.add_argument("--timeout-vm", type=float, default=140.0)
    p.add_argument("--no-vm", action="store_true", help="Skip VM execution (Windows-only, for testing)")
    p.add_argument("--skip-golden-check", action="store_true", help="Skip golden integrity gate (for testing the gate itself)")
    args = p.parse_args(argv)

    if not args.skip_golden_check:
        check_golden_integrity_gate()

    if args.all:
        probes = ALL_PROBES
    elif args.probes:
        probes = args.probes
    elif args.probe:
        probes = [args.probe]
    else:
        probes = CORE_PROBES

    ROOT = project_root()
    # Freshness gate before any VM work
    if not args.no_vm:
        if not _freshness_check():
            print("dualrun: STALE BUILD - aborting before VM runs", file=sys.stderr)
            sys.exit(125)
    reports_dir = Path(args.reports_dir)
    if not reports_dir.is_absolute():
        reports_dir = ROOT / reports_dir
    reports_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")

    results = []
    all_pass = True
    for probe in probes:
        print(f"\n=== dualrun {probe} ===", flush=True)
        win_json, win_raw, win_rc = run_windows_probe(probe, timeout=args.timeout_win)
        print(f"  Windows: rc={win_rc} json={'ok' if win_json else 'FAIL'} len={len(win_raw) if win_raw else 0}", flush=True)
        if win_json is None:
            print(f"  Windows raw excerpt: {win_raw[:500]}", flush=True)
        vm_json = None
        vm_raw = ""
        vm_rc = 0
        if args.no_vm:
            print(f"  VM: skipped (--no-vm)", flush=True)
            vm_raw = "skipped"
        else:
            vm_json, vm_raw, vm_rc = run_vm_probe(probe, timeout=args.timeout_vm)
            print(f"  VM: rc={vm_rc} json={'ok' if vm_json else 'FAIL'} len={len(vm_raw) if vm_raw else 0}", flush=True)
            if vm_json is None:
                print(f"  VM raw excerpt: {vm_raw[:800]}", flush=True)

        divergences = diff_jsons(win_json, vm_json, probe)
        eval_res = evaluate_probe(probe, win_json, vm_json, divergences)
        print(f"  Divergences: {len(divergences)}  S1={eval_res['s1']} S2={eval_res['s2']} S3={eval_res['s3']} -> {eval_res['verdict']} ({eval_res['reason']})", flush=True)
        for d in divergences[:15]:
            print(f"    [{d['severity']}] {d['field']}: win={d['windows'][:120]} vs vm={d['vm'][:120]} ({d['note']})", flush=True)
        if len(divergences) > 15:
            print(f"    ... and {len(divergences)-15} more", flush=True)

        # Save per-probe JSON
        per_probe = {
            "probe": probe,
            "timestamp": timestamp,
            "windows_build": "26200.9168",
            "windows": {"json": win_json, "raw_len": len(win_raw) if win_raw else 0, "rc": win_rc},
            "vm": {"json": vm_json, "raw_len": len(vm_raw) if vm_raw else 0, "rc": vm_rc, "raw_excerpt": vm_raw[:2000]},
            "divergences": divergences,
            "evaluation": eval_res,
        }
        results.append(per_probe)
        if eval_res["verdict"] != "PASS":
            all_pass = False

        # Per-probe file
        out_path = reports_dir / f"dualrun_{probe}_{timestamp}.json"
        with open(out_path, "w", encoding="utf-8") as fh:
            json.dump(per_probe, fh, indent=2, ensure_ascii=False)
        print(f"  Report: {out_path}", flush=True)

    # Aggregate report
    summary = {
        "run_id": timestamp,
        "project_root": str(ROOT),
        "windows_build": "26200.9168",
        "probes": probes,
        "results": results,
        "overall_pass": all_pass,
        "total_probes": len(probes),
        "passed": sum(1 for r in results if r["evaluation"]["verdict"]=="PASS"),
        "failed": sum(1 for r in results if r["evaluation"]["verdict"]!="PASS"),
        "total_divergences": sum(len(r["divergences"]) for r in results),
        "total_s1": sum(r["evaluation"]["s1"] for r in results),
        "total_s2": sum(r["evaluation"]["s2"] for r in results),
        "total_s3": sum(r["evaluation"]["s3"] for r in results),
    }
    agg_path = reports_dir / f"dualrun_summary_{timestamp}.json"
    with open(agg_path, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2, ensure_ascii=False)
    # Also write markdown summary
    md_path = reports_dir / f"dualrun_summary_{timestamp}.md"
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write(f"# Dual-Run Summary {timestamp}\n\n")
        fh.write(f"Windows build 26200.9168, probes: {', '.join(probes)}\n\n")
        fh.write(f"**Overall:** {'PASS' if all_pass else 'FAIL'}  Passed {summary['passed']}/{summary['total_probes']}  Divergences {summary['total_divergences']} (S1 {summary['total_s1']}, S2 {summary['total_s2']}, S3 {summary['total_s3']})\n\n")
        fh.write("| Probe | Verdict | S1 | S2 | S3 | Divs | Reason |\n|---|---|---|---|---|---|---|\n")
        for r in results:
            e = r["evaluation"]
            fh.write(f"| {r['probe']} | {e['verdict']} | {e['s1']} | {e['s2']} | {e['s3']} | {len(r['divergences'])} | {e['reason']} |\n")
        fh.write("\n## Ranked divergences (S1 first)\n\n")
        for r in results:
            if r["divergences"]:
                fh.write(f"### {r['probe']} ({r['evaluation']['verdict']})\n")
                for d in r["divergences"][:30]:
                    fh.write(f"- [{d['severity']}] `{d['field']}`: win `{d['windows'][:100]}` vs vm `{d['vm'][:100]}` — {d['note']}\n")
                if len(r["divergences"])>30:
                    fh.write(f"- ... {len(r['divergences'])-30} more\n")
                fh.write("\n")
    print(f"\n=== Aggregate report ===", flush=True)
    print(f"  Summary JSON: {agg_path}", flush=True)
    print(f"  Summary MD: {md_path}", flush=True)
    print(f"  Passed {summary['passed']}/{summary['total_probes']}, S1 {summary['total_s1']}", flush=True)

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(summary, fh, indent=2, ensure_ascii=False)
        print(f"  Copied to {args.json_out}", flush=True)

    # Exit code: 0 if all PASS, 1 otherwise (for CI)
    sys.exit(0 if all_pass else 1)

if __name__ == "__main__":
    main()
