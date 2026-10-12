#!/usr/bin/env python3
""" handles.py - host side with P1 staleness fix (nonce + timeout handling)"""
import json, subprocess, sys, pathlib, uuid, datetime
ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "harness"))
from _paths import wsl_repo_root
VMTEST = ROOT / "tests" / "harness" / "vmtest.py"
GUEST = "/mnt/share/tests/kernel/guest/run_handles.sh"
RESULT = ROOT / ".dodresult" / "kernel_handles.json"
NONCE_FILE = ROOT / ".dodresult" / "run_id_handles"

def _freshness_ok() -> bool:
    try:
        import platform, pathlib
        root = pathlib.Path(__file__).resolve().parents[3]
        fresh = root / "vm" / "freshness.sh"
        if not fresh.exists():
            return True
        import subprocess, sys
        if platform.system() == "Windows":
            wsl_root = wsl_repo_root()
            cmd = ["wsl", "-d", "Ubuntu", "--", "bash", f"{wsl_root}/vm/freshness.sh", "check"]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr); print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=sys.stderr); print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
        else:
            proc = subprocess.run(["bash", str(fresh), "check"], capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr); print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=sys.stderr); print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
    except Exception as e:
        print(f"freshness check failed: {e}", file=sys.stderr); return False

def main():
    if not _freshness_ok():
        return 125
    run_id = uuid.uuid4().hex
    for stale in [RESULT, ROOT / ".dodresult" / "kernel_handles.log", NONCE_FILE, ROOT / ".dodresult" / "result.exitcode"]:
        try:
            if stale.exists():
                stale.unlink()
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
    try:
        NONCE_FILE.write_text(run_id, encoding="utf-8")
    except Exception as e:
        print(f"failed to write nonce: {e}", file=sys.stderr)
        return 125
    cmd = [sys.executable, str(VMTEST), "--test", GUEST, "--share", str(ROOT), "--timeout", "120"]
    rc = subprocess.run(cmd).returncode
    if rc in (124,137):
        if RESULT.exists():
            try:
                data=json.loads(RESULT.read_text())
                if data.get("run_id") != run_id:
                    print(f"TIMEOUT: stale rejected expected {run_id[:8]} got {str(data.get('run_id',''))[:8]}", file=sys.stderr)
                try: RESULT.unlink()
                except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
            except:
                try: RESULT.unlink()
                except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        timeout_data={"test":"handles","status":"TIMEOUT","run_id":run_id,"note":f"VM timed out after 120s (rc={rc})","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(timeout_data, indent=2), encoding="utf-8")
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        print(f"handles TIMEOUT after {rc}", file=sys.stderr)
        return 124
    if not RESULT.exists():
        print(f"ERROR: missing result file {RESULT} (rc={rc})", file=sys.stderr)
        err_data={"test":"handles","status":"ERROR","run_id":run_id,"note":"missing result file","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        return 125
    try:
        data = json.loads(RESULT.read_text())
    except Exception as e:
        print(f"failed to parse result: {e}", file=sys.stderr)
        return 125
    if data.get("run_id") != run_id:
        print(f"ERROR: run_id mismatch expected {run_id[:8]} got {str(data.get('run_id',''))[:8]}", file=sys.stderr)
        err_data={"test":"handles","status":"ERROR","run_id":run_id,"expected_run_id":run_id,"found_run_id":data.get("run_id"),"note":"run_id mismatch","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        return 125
    print(f"HANDLES result: {data.get('status')} missing={data.get('missing_service')}")
    return 0 if data.get("status")=="PASS" else 1
if __name__ == "__main__":
    sys.exit(main())
