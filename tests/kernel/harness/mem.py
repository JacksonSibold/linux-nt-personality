#!/usr/bin/env python3
"""mem.py - host side for MEM VAD 4096 matrix (DOD-MEM-A001/B001/B002) - P1 nonce fix"""
import json, subprocess, sys, pathlib, hashlib, datetime, shutil, os, uuid
ROOT = pathlib.Path(__file__).resolve().parents[3]
VMTEST = ROOT / "tests" / "harness" / "vmtest.py"
GUEST = "/mnt/share/tests/kernel/guest/run_mem.sh"
RESULT = ROOT / ".dodresult" / "kernel_mem.json"
NONCE_FILE = ROOT / ".dodresult" / "run_id_mem"

def _freshness_ok() -> bool:
    try:
        import platform as _pf, pathlib as _pl
        root = _pl.Path(__file__).resolve().parents[3]
        fresh = root / "vm" / "freshness.sh"
        if not fresh.exists():
            return True
        import subprocess as _sp
        if _pf.system() == "Windows":
            wsl_root = "/path/to/ntpers-research"
            cmd = ["wsl", "-d", "Ubuntu", "--", "bash", f"{wsl_root}/vm/freshness.sh", "check"]
            proc = _sp.run(cmd, capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr); print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=sys.stderr); print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
        else:
            proc = _sp.run(["bash", str(fresh), "check"], capture_output=True, text=True, timeout=30)
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
    if sys.platform == "win32":
        wsl_cmd = ["wsl", "-d", "Ubuntu", "--", "bash", "-c", "cd ~/winapi-native-os && python3 tests/kernel/harness/mem.py"]
        rc = subprocess.run(wsl_cmd).returncode
        try:
            subprocess.run(["wsl","-d","Ubuntu","--","bash","-c",
                "cat ~/winapi-native-os/.dodresult/kernel_mem.json 2>/dev/null | tr -d '\\r' > '/path/to/ntpers-research/.dodresult/kernel_mem.json' || true"], timeout=5)
            subprocess.run(["wsl","-d","Ubuntu","--","bash","-c",
                "cat ~/winapi-native-os/.dodresult/kernel_mem.log 2>/dev/null | tr -d '\\r' > '/path/to/ntpers-research/.dodresult/kernel_mem.log' || true"], timeout=5)
        except Exception as e:
            print(f"[WSL] copy failed: {e}", file=sys.stderr)
        if RESULT.exists():
            try:
                data=json.loads(RESULT.read_text(encoding="utf-8"))
                import time as _time
                if _time.time() - RESULT.stat().st_mtime > 300:
                    print(f"[WSL] ERROR stale mem result age >300s", file=sys.stderr)
                    return 125
                if not data.get("run_id") or data.get("run_id") == "missing":
                    print(f"[WSL] ERROR missing run_id", file=sys.stderr)
                    return 125
                print(f"[WSL] MEM result: {data.get('status')} volume {data.get('volume')} run_id {str(data.get('run_id',''))[:8]}")
                if rc == 124:
                    return 124
                if rc == 125:
                    return 125
                # Enforce volume check on Windows side too (don't fabricate)
                if data.get("status")=="PASS" and str(data.get("volume"))!="4096":
                    print(f"[WSL] FAIL volume {data.get('volume')} !=4096", file=sys.stderr)
                    return 1
                return 0 if data.get("status")=="PASS" else 1
            except Exception as e:
                print(f"[WSL] parse fail: {e}", file=sys.stderr)
                return 125
        return rc

    run_id = uuid.uuid4().hex
    for stale in [RESULT, ROOT / ".dodresult" / "kernel_mem.log", NONCE_FILE, ROOT / ".dodresult" / "result.exitcode", ROOT / ".dodresult" / "mem_vad_diag.json"]:
        try:
            if stale.exists():
                stale.unlink()
        except Exception as e:
            print(f"warn unlink {stale}: {e}", file=sys.stderr)
    # Invalidate evidence bundles per-run
    for crit in ["DOD-MEM-A001", "DOD-MEM-B001", "DOD-MEM-B002"]:
        evid_dir = ROOT / "reports" / "evidence" / crit
        if evid_dir.exists():
            for old in list(evid_dir.iterdir()):
                try:
                    if old.is_file(): old.unlink()
                except Exception: pass
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
                    print(f"TIMEOUT: stale result rejected (expected {run_id[:8]}, got {str(data.get('run_id',''))[:8]})", file=sys.stderr)
                try: RESULT.unlink()
                except Exception: pass
            except Exception:
                try: RESULT.unlink()
                except Exception: pass
        timeout_data={"test":"mem","status":"TIMEOUT","run_id":run_id,"volume":"0","note":f"VM timed out after 120s (rc={rc})","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(timeout_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write timeout failed: {e}", file=sys.stderr)
        print(f"MEM TIMEOUT after {rc}", file=sys.stderr)
        return 124

    if not RESULT.exists():
        print(f"ERROR: missing result file {RESULT} (rc={rc})", file=sys.stderr)
        err_data={"test":"mem","status":"ERROR","run_id":run_id,"note":"missing result file","volume":"0","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write error failed: {e}", file=sys.stderr)
        return 125

    try:
        data = json.loads(RESULT.read_text())
    except Exception as e:
        print(f"failed to parse result: {e}", file=sys.stderr)
        return 125

    if data.get("run_id") != run_id:
        print(f"ERROR: run_id mismatch - stale result rejected (expected {run_id[:8]}, got {str(data.get('run_id',''))[:8]})", file=sys.stderr)
        err_data={"test":"mem","status":"ERROR","run_id":run_id,"expected_run_id":run_id,"found_run_id":data.get("run_id"),"note":"run_id mismatch - stale file","volume":"0","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write err failed: {e}", file=sys.stderr)
        return 125

    print(f"MEM result: {data.get('status')} volume {data.get('volume')}/{data.get('threshold_volume')} missing={data.get('missing_service')}")
    # Hardened evidence bundle: hash covers all files + run_id, serial log recency checked, old bundles invalidated above
    for crit in ["DOD-MEM-A001", "DOD-MEM-B001", "DOD-MEM-B002"]:
        evid_dir = ROOT / "reports" / "evidence" / crit
        evid_dir.mkdir(parents=True, exist_ok=True)
        serial_path = None
        serials = sorted((ROOT / ".dodresult").glob("serial-*.log"), key=lambda p: p.stat().st_mtime)
        if serials:
            import time
            latest = serials[-1]
            if time.time() - latest.stat().st_mtime < 120:
                serial_path = latest
        try: shutil.copy(RESULT, evid_dir / "kernel_mem.json")
        except Exception as e:
            print(f"copy mem result failed: {e}", file=sys.stderr)
        log_path = ROOT / ".dodresult" / "kernel_mem.log"
        if log_path.exists():
            try: shutil.copy(log_path, evid_dir / "kernel_mem.log")
            except Exception: pass
        if serial_path:
            try: shutil.copy(serial_path, evid_dir / "serial.log")
            except Exception: pass
        else:
            try: (evid_dir / "serial.log").unlink()
            except Exception: pass
        try:
            hasher = hashlib.sha256()
            for fname in ["kernel_mem.json", "kernel_mem.log", "serial.log"]:
                fpath = evid_dir / fname
                if fpath.exists():
                    hasher.update(fpath.read_bytes())
            hasher.update(run_id.encode())
            h = hasher.hexdigest()
            manifest = {}
            for fname in ["kernel_mem.json", "kernel_mem.log", "serial.log"]:
                fpath = evid_dir / fname
                if fpath.exists():
                    manifest[fname] = hashlib.sha256(fpath.read_bytes()).hexdigest()
            (evid_dir / "hash.txt").write_text(h)
            (evid_dir / "run_id.txt").write_text(run_id)
            (evid_dir / "evidence.json").write_text(json.dumps({"criterion":crit,"result":data,"hash":h,"manifest":manifest,"run_id":run_id,"timestamp":datetime.datetime.now(datetime.timezone.utc).isoformat()+"Z"}, indent=2))
        except Exception as e:
            print(f"evidence bundle fail {crit}: {e}", file=sys.stderr)
        print(f"evidence bundle {crit} at {evid_dir} status {data.get('status')} hash {h[:16] if 'h' in locals() else 'none'}")
    if data.get("status") == "PASS":
        if str(data.get("volume"))=="4096":
            print("MEM 4096 PASS at FULL volume, in-kernel")
            return 0
        else:
            print(f"MEM volume mismatch: {data.get('volume')} !=4096", file=sys.stderr)
            return 1
    else:
        print(f"MEM FAIL: {data}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
