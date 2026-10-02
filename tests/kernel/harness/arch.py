#!/usr/bin/env python3
"""arch.py - host side for ARCH in-VM test (DOD-ARCH-B001 / A002) - with staleness fix (P1 nonce)"""
import json, subprocess, sys, pathlib, os, uuid, datetime, hashlib, shutil
ROOT = pathlib.Path(__file__).resolve().parents[3]
VMTEST = ROOT / "tests" / "harness" / "vmtest.py"
GUEST = "/mnt/share/tests/kernel/guest/run_arch.sh"
RESULT = ROOT / ".dodresult" / "kernel_arch.json"
NONCE_FILE = ROOT / ".dodresult" / "run_id_arch"

def is_wsl_available():
    return sys.platform == "win32" and os.system("wsl -d Ubuntu -- bash -c 'echo wsl_ok' >NUL 2>&1") == 0

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
    # Windows delegation - delegate and copy back, but validation happens inside WSL
    if sys.platform == "win32":
        wsl_cmd = ["wsl", "-d", "Ubuntu", "--", "bash", "-c", "cd ~/winapi-native-os && python3 tests/kernel/harness/arch.py"]
        rc = subprocess.run(wsl_cmd).returncode
        try:
            subprocess.run(["wsl", "-d", "Ubuntu", "--", "bash", "-c",
                "cat ~/winapi-native-os/.dodresult/kernel_arch.json 2>/dev/null | tr -d '\\r' > '/path/to/ntpers-research/.dodresult/kernel_arch.json' || true"],
                timeout=5)
            subprocess.run(["wsl", "-d", "Ubuntu", "--", "bash", "-c",
                "cat ~/winapi-native-os/.dodresult/kernel_arch.log 2>/dev/null | tr -d '\\r' > '/path/to/ntpers-research/.dodresult/kernel_arch.log' || true"],
                timeout=5)
        except Exception as e:
            print(f"[WSL] copy back failed: {e}", file=sys.stderr)
        if RESULT.exists():
            try:
                data = json.loads(RESULT.read_text(encoding="utf-8"))
                # Validate run_id present and recent (mtime within 5 min) to avoid stale fallback
                import time
                mtime = RESULT.stat().st_mtime
                age = time.time() - mtime
                if age > 300:
                    print(f"[WSL] ERROR: result file stale (age {age:.0f}s) - rejecting", file=sys.stderr)
                    return 125
                if not data.get("run_id") or data.get("run_id") == "missing":
                    print(f"[WSL] ERROR: result missing run_id - rejecting", file=sys.stderr)
                    return 125
                print(f"[WSL] ARCH result: {data.get('status')} launches {data.get('success')}/{data.get('total_launches')} ratio {data.get('ratio_host_per_nt')} run_id {data.get('run_id','')[:8]}")
                # Map harness rc to dodrun states: 124 timeout, 125 error
                if rc == 124:
                    return 124
                if rc == 125:
                    return 125
                return 0 if data.get("status") == "PASS" else 1
            except Exception as e:
                print(f"[WSL] parse fail after copy: {e}", file=sys.stderr)
                return 125
        return rc

    # Linux/WSL path: direct execution with nonce
    run_id = uuid.uuid4().hex
    # Delete stale files BEFORE starting VM - do not swallow errors silently
    for stale in [RESULT, RESULT.with_suffix('.log'), NONCE_FILE]:
        try:
            if stale.exists():
                stale.unlink()
        except Exception as e:
            print(f"warn unlink {stale}: {e}", file=sys.stderr)
    # Also delete generic result.exitcode stale
    generic = ROOT / ".dodresult" / "result.exitcode"
    try:
        if generic.exists():
            generic.unlink()
    except Exception as e:
        print(f"warn unlink {generic}: {e}", file=sys.stderr)
    # Invalidate evidence bundles for this criterion (cannot be left over from previous run)
    for crit in ["DOD-ARCH-A002", "DOD-ARCH-B001"]:
        evid_dir = ROOT / "reports" / "evidence" / crit
        if evid_dir.exists():
            # Remove old bundle files so a TIMEOUT/error run cannot leave stale PASS bundle
            for old in list(evid_dir.iterdir()):
                try:
                    if old.is_file():
                        old.unlink()
                except Exception:
                    pass
    # Write nonce for guest to consume
    try:
        NONCE_FILE.write_text(run_id, encoding="utf-8")
    except Exception as e:
        print(f"failed to write nonce: {e}", file=sys.stderr)
        return 125

    cmd = [sys.executable, str(VMTEST), "--test", GUEST, "--share", str(ROOT), "--timeout", "120"]
    rc = subprocess.run(cmd).returncode

    # TIMEOUT must never fallback to stale file
    if rc in (124, 137):
        # Check for stale file that might have been left (should have been deleted already, but if exists with wrong run_id, reject)
        if RESULT.exists():
            try:
                data = json.loads(RESULT.read_text())
                if data.get("run_id") != run_id:
                    print(f"TIMEOUT: stale result rejected (expected {run_id[:8]}, got {data.get('run_id','')[:8]})", file=sys.stderr)
                else:
                    print(f"TIMEOUT: result exists but VM timed out (run_id matches)", file=sys.stderr)
                # Remove stale to prevent future fallback
                try: RESULT.unlink()
                except Exception: pass
            except Exception:
                try: RESULT.unlink()
                except Exception: pass
        # Write explicit TIMEOUT result with correct run_id
        timeout_data = {
            "test": "arch",
            "status": "TIMEOUT",
            "run_id": run_id,
            "total_launches": 0,
            "success": 0,
            "note": f"VM timed out after 120s (rc={rc})",
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")
        }
        try:
            RESULT.write_text(json.dumps(timeout_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write timeout data failed: {e}", file=sys.stderr)
        print(f"ARCH TIMEOUT after {rc}s", file=sys.stderr)
        return 124

    # Check existence
    if not RESULT.exists():
        print(f"ERROR: missing result file {RESULT} (rc={rc})", file=sys.stderr)
        err_data = {"test":"arch","status":"ERROR","run_id":run_id,"note":"missing result file","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write error data failed: {e}", file=sys.stderr)
        return 125

    try:
        data = json.loads(RESULT.read_text())
    except Exception as e:
        print(f"failed to parse result: {e}", file=sys.stderr)
        return 125

    # Validate run_id
    if data.get("run_id") != run_id:
        print(f"ERROR: run_id mismatch - stale result rejected (expected {run_id[:8]}, got {data.get('run_id','')[:8]})", file=sys.stderr)
        # Treat as error (crash/missing guest) - do not fallback
        err_data = {"test":"arch","status":"ERROR","run_id":run_id,"expected_run_id":run_id,"found_run_id":data.get("run_id"),"note":"run_id mismatch - stale file","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        # Overwrite stale with error
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write err data failed: {e}", file=sys.stderr)
        return 125

    print(f"ARCH result: {data.get('status')} launches {data.get('success')}/{data.get('total_launches')} ratio {data.get('ratio_host_per_nt')}")
    print(f"  nt_syscalls={data.get('nt_syscalls_dmesg')} host={data.get('host_syscalls')} forbidden_fds={data.get('forbidden_fds')} b001_pass={data.get('b001_pass')} a002_pass={data.get('a002_pass')}")
    # evidence bundles for both criteria - hardened: hash covers ALL bundle files, include serial log + run_id, invalidate stale
    try:
        for crit in ["DOD-ARCH-A002", "DOD-ARCH-B001"]:
            evid_dir = ROOT / "reports" / "evidence" / crit
            evid_dir.mkdir(parents=True, exist_ok=True)
            # serial log is selected as most recent but must be correlated to this run.
            # We capture it by mtime but also verify it's recent (<60s old) otherwise treat as missing.
            serial_path = None
            serials = sorted((ROOT / ".dodresult").glob("serial-*.log"), key=lambda p: p.stat().st_mtime)
            if serials:
                import time
                latest = serials[-1]
                age = time.time() - latest.stat().st_mtime
                if age < 120:
                    serial_path = latest
                else:
                    print(f"warn: serial log {latest} age {age:.0f}s too old for run {run_id[:8]}, not including as stale", file=sys.stderr)
            # Copy files
            shutil.copy(RESULT, evid_dir / "kernel_arch.json")
            log_path = ROOT / ".dodresult" / "kernel_arch.log"
            if log_path.exists():
                shutil.copy(log_path, evid_dir / "kernel_arch.log")
            if serial_path:
                shutil.copy(serial_path, evid_dir / "serial.log")
            else:
                # Ensure no stale serial.log remains
                try: (evid_dir / "serial.log").unlink()
                except Exception: pass
            # Hash covers ALL files in bundle (kernel_arch.json + kernel_arch.log + serial.log)
            hasher = hashlib.sha256()
            for fname in ["kernel_arch.json", "kernel_arch.log", "serial.log"]:
                fpath = evid_dir / fname
                if fpath.exists():
                    hasher.update(fpath.read_bytes())
            # Also mix run_id to bind bundle to this run
            hasher.update(run_id.encode())
            h = hasher.hexdigest()
            (evid_dir / "hash.txt").write_text(h)
            # evidence.json includes manifest of what was hashed
            manifest = {}
            for fname in ["kernel_arch.json", "kernel_arch.log", "serial.log"]:
                fpath = evid_dir / fname
                if fpath.exists():
                    manifest[fname] = hashlib.sha256(fpath.read_bytes()).hexdigest()
            (evid_dir / "evidence.json").write_text(json.dumps({"criterion":crit,"result":data,"hash":h,"manifest":manifest,"run_id":run_id,"timestamp":datetime.datetime.now(datetime.timezone.utc).isoformat()+"Z"}, indent=2))
            # Also write run_id file for per-run invalidation check
            (evid_dir / "run_id.txt").write_text(run_id)
            print(f"evidence bundle {crit} at {evid_dir} hash {h[:16]}")
    except Exception as e:
        print(f"evidence bundle fail: {e}", file=sys.stderr)
    if data.get("status") == "PASS":
        return 0
    else:
        print(f"ARCH FAIL: {data}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
