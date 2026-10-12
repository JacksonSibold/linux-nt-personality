#!/usr/bin/env python3
"""provenance.py - provenance scan harness for DOD-ARCH-A001 / B002 / B006 - P1 nonce fix"""
import json, subprocess, sys, pathlib, os, hashlib, datetime, shutil, uuid
ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "harness"))
from _paths import wsl_repo_root
VMTEST = ROOT / "tests" / "harness" / "vmtest.py"
GUEST = "/mnt/share/tests/kernel/guest/run_provenance.sh"
RESULT = ROOT / ".dodresult" / "kernel_provenance.json"
NONCE_FILE = ROOT / ".dodresult" / "run_id_provenance"

def run_host_scan():
    forbidden = ["wine","wineserver","qemu","box64","fex","dosemu"]
    hits=[]
    for rootdir, dirs, files in os.walk(ROOT):
        if ".git" in rootdir or ".dodresult" in rootdir or "reports" in rootdir:
            continue
        for f in files:
            if f.endswith((".c",".h",".py",".sh","Makefile")):
                pth = pathlib.Path(rootdir)/f
                try:
                    txt = pth.read_text(errors="ignore")
                    low = txt.lower()
                    for pat in forbidden:
                        if pat in low:
                            if "provenance" in low and "forbidden" in low:
                                continue
                            if f"include.*{pat}" in low or f"import.*{pat}" in low:
                                hits.append(f"{pth}:{pat}")
                except Exception as e:
                    print(f"warn scan {pth}: {e}", file=sys.stderr)
    bin_dir = ROOT / "tests" / "kernel" / "bin"
    if bin_dir.exists():
        for pe in bin_dir.glob("*.exe"):
            try:
                data = pe.read_bytes()
                low = data.lower()
                for pat in [b"wine", b"qemu", b"box64", b"fex"]:
                    if pat in low:
                        hits.append(f"{pe.name}:{pat.decode()}")
            except Exception as e:
                print(f"warn scan pe {pe}: {e}", file=sys.stderr)
    return hits

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
    host_hits = run_host_scan()
    print(f"Host provenance scan hits: {len(host_hits)} {host_hits[:5]}")
    # freshness gate after host scan but before VM
    # (still must gate VM runs, but allow host-only scan to report)
    if not _freshness_ok():
        print("provenance: STALE BUILD - VM part will be refused", file=sys.stderr)
        # we still proceed to WSL delegation which will itself refuse; but early exit with 125
        return 125
    if sys.platform == "win32":
        wsl_cmd = ["wsl", "-d", "Ubuntu", "--", "bash", "-c", "cd ~/winapi-native-os && python3 tests/kernel/harness/provenance.py"]
        rc = subprocess.run(wsl_cmd).returncode
        try:
            subprocess.run(["wsl","-d","Ubuntu","--","bash","-c",
                f"cat ~/winapi-native-os/.dodresult/kernel_provenance.json 2>/dev/null | tr -d '\\r' > '{wsl_repo_root()}/.dodresult/kernel_provenance.json' || true"], timeout=5)
            subprocess.run(["wsl","-d","Ubuntu","--","bash","-c",
                f"cat ~/winapi-native-os/.dodresult/kernel_provenance.log 2>/dev/null | tr -d '\\r' > '{wsl_repo_root()}/.dodresult/kernel_provenance.log' || true"], timeout=5)
        except Exception as e:
            print(f"[WSL] copy failed: {e}", file=sys.stderr)
        if RESULT.exists():
            try:
                data=json.loads(RESULT.read_text(encoding="utf-8"))
                import time as _time
                if _time.time() - RESULT.stat().st_mtime > 300:
                    print(f"[WSL] ERROR stale provenance result", file=sys.stderr)
                    return 125
                if not data.get("run_id") or data.get("run_id") == "missing":
                    print(f"[WSL] ERROR missing run_id", file=sys.stderr)
                    return 125
                print(f"[WSL] Provenance VM hits {data.get('hits')} status {data.get('status')} host_hits {len(host_hits)} run_id {str(data.get('run_id',''))[:8]}")
                total_hits = data.get('hits',0) + len(host_hits)
                status = "PASS" if total_hits==0 else "FAIL"
                print(f"Combined provenance: {status} hits={total_hits}")
                if rc==124:
                    return 124
                if rc==125:
                    return 125
                return 0 if status=="PASS" else 1
            except Exception as e:
                print(f"[WSL] parse fail: {e}", file=sys.stderr)
                return 125
        return rc
    run_id = uuid.uuid4().hex
    for stale in [RESULT, ROOT / ".dodresult" / "kernel_provenance.log", NONCE_FILE, ROOT / ".dodresult" / "result.exitcode"]:
        try:
            if stale.exists():
                stale.unlink()
        except Exception as e:
            print(f"warn unlink: {e}", file=sys.stderr)
    # Invalidate evidence bundles per-run
    for crit in ["DOD-ARCH-A001","DOD-ARCH-B002-PROVENANCE","DOD-ARCH-B006"]:
        evid_dir = ROOT / "reports" / "evidence" / crit
        if evid_dir.exists():
            for old in list(evid_dir.iterdir()):
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
                except Exception: pass
            except Exception:
                try: RESULT.unlink()
                except Exception: pass
        timeout_data={"test":"provenance","status":"TIMEOUT","run_id":run_id,"hits":99,"note":f"VM timeout rc={rc}","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(timeout_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write timeout failed: {e}", file=sys.stderr)
        print(f"Provenance TIMEOUT after {rc}", file=sys.stderr)
        return 124
    vm_data={}
    if RESULT.exists():
        try:
            vm_data=json.loads(RESULT.read_text())
        except Exception as e:
            print(f"parse fail: {e}", file=sys.stderr)
            return 125
    else:
        print(f"ERROR: missing result {RESULT} rc={rc}", file=sys.stderr)
        err_data={"test":"provenance","status":"ERROR","run_id":run_id,"hits":99,"note":"missing result file","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write err failed: {e}", file=sys.stderr)
        return 125
    if vm_data.get("run_id") != run_id:
        print(f"ERROR run_id mismatch {run_id[:8]} vs {str(vm_data.get('run_id',''))[:8]}", file=sys.stderr)
        err_data={"test":"provenance","status":"ERROR","run_id":run_id,"expected_run_id":run_id,"found_run_id":vm_data.get("run_id"),"note":"run_id mismatch","timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00","Z")}
        try: RESULT.write_text(json.dumps(err_data, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"write err failed: {e}", file=sys.stderr)
        return 125
    print(f"VM provenance: {vm_data.get('status')} hits {vm_data.get('hits')} details {vm_data.get('details')}")
    total_hits = vm_data.get('hits',99) + len(host_hits)
    status = "PASS" if total_hits==0 else "FAIL"
    print(f"Combined host+VM provenance: {status} total_hits={total_hits} host={len(host_hits)} vm={vm_data.get('hits')}")
    for crit in ["DOD-ARCH-A001","DOD-ARCH-B002-PROVENANCE","DOD-ARCH-B006"]:
        evid_dir = ROOT / "reports" / "evidence" / crit
        evid_dir.mkdir(parents=True, exist_ok=True)
        combined = {"vm": vm_data, "host_hits": host_hits, "total_hits": total_hits, "status": status, "run_id": run_id}
        try: (evid_dir / "combined.json").write_text(json.dumps(combined, indent=2))
        except Exception as e:
            print(f"write combined failed: {e}", file=sys.stderr)
        if RESULT.exists():
            try: shutil.copy(RESULT, evid_dir / "kernel_provenance.json")
            except Exception as e:
                print(f"copy result failed: {e}", file=sys.stderr)
            log_path = ROOT / ".dodresult" / "kernel_provenance.log"
            if log_path.exists():
                try: shutil.copy(log_path, evid_dir / "kernel_provenance.log")
                except Exception: pass
        try: (evid_dir / "host_hits.json").write_text(json.dumps(host_hits, indent=2))
        except Exception as e:
            print(f"write host_hits failed: {e}", file=sys.stderr)
        # hardened: serial log recency check
        serial_path = None
        serials = sorted((ROOT / ".dodresult").glob("serial-*.log"), key=lambda p: p.stat().st_mtime)
        if serials:
            import time as _time2
            latest = serials[-1]
            if _time2.time() - latest.stat().st_mtime < 120:
                serial_path = latest
        if serial_path:
            try: shutil.copy(serial_path, evid_dir / "serial.log")
            except Exception: pass
        else:
            try: (evid_dir / "serial.log").unlink()
            except Exception: pass
        try:
            # hash covers ALL bundle files + run_id
            hasher = hashlib.sha256()
            for fname in ["combined.json", "kernel_provenance.json", "kernel_provenance.log", "host_hits.json", "serial.log"]:
                fpath = evid_dir / fname
                if fpath.exists():
                    hasher.update(fpath.read_bytes())
            hasher.update(run_id.encode())
            h = hasher.hexdigest()
            manifest = {}
            for fname in ["combined.json", "kernel_provenance.json", "kernel_provenance.log", "host_hits.json", "serial.log"]:
                fpath = evid_dir / fname
                if fpath.exists():
                    manifest[fname] = hashlib.sha256(fpath.read_bytes()).hexdigest()
            (evid_dir / "hash.txt").write_text(h)
            (evid_dir / "run_id.txt").write_text(run_id)
            (evid_dir / "evidence.json").write_text(json.dumps({"criterion":crit,"result":combined,"hash":h,"manifest":manifest,"run_id":run_id,"timestamp":datetime.datetime.utcnow().isoformat()+"Z"}, indent=2))
        except Exception as e:
            print(f"evidence hash failed: {e}", file=sys.stderr)
        print(f"evidence bundle {crit} at {evid_dir} status {status} hash {h[:16] if 'h' in locals() else 'none'}")
    return 0 if status=="PASS" else 1

if __name__=="__main__":
    sys.exit(main())
