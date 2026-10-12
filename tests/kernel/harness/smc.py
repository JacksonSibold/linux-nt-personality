#!/usr/bin/env python3
"""smc.py - host side for SMC probe (DOD-ARCH-B002 SMC sub-criterion) - P1 nonce fix"""
import json, subprocess, sys, pathlib, os, uuid, datetime, hashlib, shutil
ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "harness"))
from _paths import wsl_repo_root
VMTEST = ROOT / "tests" / "harness" / "vmtest.py"
GUEST = "/mnt/share/tests/kernel/guest/run_smc.sh"
RESULT = ROOT / ".dodresult" / "kernel_smc.json"
NONCE_FILE = ROOT / ".dodresult" / "run_id_smc"

def _freshness_ok() -> bool:
    try:
        import platform as _pf, pathlib as _pl
        root = _pl.Path(__file__).resolve().parents[3]
        fresh = root / "vm" / "freshness.sh"
        if not fresh.exists():
            return True
        import subprocess as _sp, sys as _sys
        if _pf.system() == "Windows":
            wsl_root = wsl_repo_root()
            cmd = ["wsl", "-d", "Ubuntu", "--", "bash", f"{wsl_root}/vm/freshness.sh", "check"]
            proc = _sp.run(cmd, capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=_sys.stderr); print(proc.stderr, file=_sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=_sys.stderr); print("Run: vm/kbuild.sh", file=_sys.stderr)
                return False
            return True
        else:
            proc = _sp.run(["bash", str(fresh), "check"], capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=_sys.stderr); print(proc.stderr, file=_sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=_sys.stderr); print("Run: vm/kbuild.sh", file=_sys.stderr)
                return False
            return True
    except Exception as e:
        print(f"freshness check failed: {e}", file=sys.stderr); return False

def main():
    if not _freshness_ok():
        return 125
    if sys.platform == "win32":
        wsl_cmd = ["wsl", "-d", "Ubuntu", "--", "bash", "-c", "cd ~/winapi-native-os && python3 tests/kernel/harness/smc.py"]
        rc = subprocess.run(wsl_cmd).returncode
        try:
            subprocess.run(["wsl", "-d", "Ubuntu", "--", "bash", "-c",
                f"cat ~/winapi-native-os/.dodresult/kernel_smc.json 2>/dev/null | tr -d '\\r' > '{wsl_repo_root()}/.dodresult/kernel_smc.json' || true"], timeout=5)
            subprocess.run(["wsl", "-d", "Ubuntu", "--", "bash", "-c",
                f"cat ~/winapi-native-os/.dodresult/kernel_smc.log 2>/dev/null | tr -d '\\r' > '{wsl_repo_root()}/.dodresult/kernel_smc.log' || true"], timeout=5)
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        if RESULT.exists():
            try:
                data=json.loads(RESULT.read_text(encoding="utf-8"))
                print(f"[WSL] SMC result: {data.get('status')} exit {data.get('exit_code')} smc_pass {data.get('smc_pass')} run_id {str(data.get('run_id',''))[:8]}")
                if rc==124:
                    return 124
                if rc==125:
                    return 125
                return 0 if data.get("status")=="PASS" else 1
            except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        return rc
    run_id = uuid.uuid4().hex
    for stale in [RESULT, ROOT / ".dodresult" / "kernel_smc.log", NONCE_FILE, ROOT / ".dodresult" / "result.exitcode"]:
        try:
            if stale.exists():
                stale.unlink()
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
    # Invalidate evidence bundle per-run
    evid_dir_tmp = ROOT / "reports" / "evidence" / "DOD-ARCH-B002-SMC"
    if evid_dir_tmp.exists():
        for old in list(evid_dir_tmp.iterdir()):
            try:
                if old.is_file(): old.unlink()
            except Exception: pass
    try: NONCE_FILE.write_text(run_id, encoding="utf-8")
    except Exception as e:
        print(f"failed to write nonce: {e}", file=sys.stderr)
        return 125
    cmd=[sys.executable, str(VMTEST), "--test", GUEST, "--share", str(ROOT), "--timeout", "120"]
    rc=subprocess.run(cmd).returncode
    if rc in (124,137):
        if RESULT.exists():
            try:
                data=json.loads(RESULT.read_text())
                if data.get("run_id")!=run_id:
                    print(f"TIMEOUT stale rejected {run_id[:8]} vs {str(data.get('run_id',''))[:8]}", file=sys.stderr)
                try: RESULT.unlink()
                except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
            except:
                try: RESULT.unlink()
                except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        timeout_data={"test":"smc","status":"TIMEOUT","run_id":run_id,"note":f"VM timeout rc={rc}","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(timeout_data, indent=2), encoding="utf-8")
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        print(f"SMC TIMEOUT after {rc}", file=sys.stderr)
        return 124
    if not RESULT.exists():
        print(f"missing result {RESULT} rc={rc}", file=sys.stderr)
        err_data={"test":"smc","status":"ERROR","run_id":run_id,"note":"missing result file","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        return 125
    try:
        data=json.loads(RESULT.read_text())
    except Exception as e:
        print(f"parse fail: {e}", file=sys.stderr)
        return 125
    if data.get("run_id") != run_id:
        print(f"ERROR run_id mismatch {run_id[:8]} vs {str(data.get('run_id',''))[:8]}", file=sys.stderr)
        err_data={"test":"smc","status":"ERROR","run_id":run_id,"expected_run_id":run_id,"found_run_id":data.get("run_id"),"note":"run_id mismatch","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as _e: print(f"warn: {_e}", file=sys.stderr)
        return 125
    print(f"SMC result: {data.get('status')} exit {data.get('exit_code')} smc_pass {data.get('smc_pass')}")
    try:
        evid_dir = ROOT / "reports" / "evidence" / "DOD-ARCH-B002-SMC"
        evid_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy(RESULT, evid_dir / "kernel_smc.json")
        log_path = ROOT / ".dodresult" / "kernel_smc.log"
        if log_path.exists():
            shutil.copy(log_path, evid_dir / "kernel_smc.log")
        serial_path = None
        serials = sorted((ROOT / ".dodresult").glob("serial-*.log"), key=lambda p: p.stat().st_mtime)
        if serials:
            import time as _timesmc
            latest = serials[-1]
            if _timesmc.time() - latest.stat().st_mtime < 120:
                serial_path = latest
        if serial_path:
            shutil.copy(serial_path, evid_dir / "serial.log")
        else:
            try: (evid_dir / "serial.log").unlink()
            except Exception: pass
        hasher = hashlib.sha256()
        for fname in ["kernel_smc.json", "kernel_smc.log", "serial.log"]:
            fpath = evid_dir / fname
            if fpath.exists():
                hasher.update(fpath.read_bytes())
        hasher.update(run_id.encode())
        h = hasher.hexdigest()
        manifest = {}
        for fname in ["kernel_smc.json", "kernel_smc.log", "serial.log"]:
            fpath = evid_dir / fname
            if fpath.exists():
                manifest[fname] = hashlib.sha256(fpath.read_bytes()).hexdigest()
        (evid_dir / "hash.txt").write_text(h)
        (evid_dir / "run_id.txt").write_text(run_id)
        (evid_dir / "evidence.json").write_text(json.dumps({"criterion":"DOD-ARCH-B002-SMC","result":data,"hash":h,"manifest":manifest,"run_id":run_id,"timestamp":datetime.datetime.utcnow().isoformat()+"Z"}, indent=2))
        print(f"evidence bundle at {evid_dir} hash {h[:16]}")
    except Exception as e:
        print(f"evidence bundle failed: {e}", file=sys.stderr)
    return 0 if data.get("status")=="PASS" else 1

if __name__=="__main__":
    sys.exit(main())
