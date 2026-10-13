#!/usr/bin/env python3
"""
capture_golden.py - build and run the differential probes on a Windows host.

Builds each plain Win32/NT probe with an MSYS2 mingw gcc into an out directory
that lives OUTSIDE the repository, runs it with a 60 s timeout, and writes the
stdout, exit code and a manifest. It never writes into the repository itself.

Safety rules (see docs/status/GOLDEN.md, section 3):
  - a probe that issues raw syscall stubs outside an NTPERS_VM guard is skipped
  - a probe that writes or deletes outside %TEMP%, changes the registry, or
    feeds UNC/device paths to file APIs is skipped
  - blocked (Application Control), crashed, hung and build-failed probes are
    recorded as such; nothing is retried with a different binary or location

Usage:
  python capture_golden.py --out C:/Users/Jackson/repos/golden-out
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil

import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE_DIR = ROOT / "tests" / "differential" / "probes"
GCC = os.environ.get("NTPERS_GCC", "C:/msys64/ucrt64/bin/gcc.exe")
TIMEOUT_S = 60

# Probes that run unchanged on this host. Order matches the README catalogue.
RUN = [
    "probe_ntstatus",
    "probe_teb_peb",
    "probe_virtualquery",
    "probe_handles",
    "probe_sharemode",
    "probe_section",
    "probe_sync",
    "probe_process_thread",
    "probe_exception",
    "probe_peloader",
    "probe_namedpipe",
    "probe_directory",
    "probe_fileinfo",
    "probe_virtualquery_direct",
    "probe_sync_direct",
    "probe_process_thread_direct",
    "probe_section_direct",  # audited, GOLDEN.md section 7
    "probe_registry",  # audited, GOLDEN.md section 7
    # GOLDEN.md section 7.2 (parameterised, see ROOTED)
    "probe_handles_direct",
    "probe_ntstatus_direct",
    "probe_path",
    "probe_path_direct",
    "probe_sharemode_direct",
]

SKIP = {
    "probe_direct_simple": "not a portable Windows probe: the whole file is raw syscall stubs (NtWriteFile 0x08, NtTerminateProcess 0x2c) with no Win32 path and no NTPERS_VM guard, and the SSNs are specific to one Windows build. GOLDEN.md section 7.2",
}

# GOLDEN.md section 7.2 probes, parameterised: they write scratch files, so they
# run inside a fresh sandbox with --root pointing into it.
ROOTED = {
    "probe_handles_direct",
    "probe_ntstatus_direct",
    "probe_path",
    "probe_path_direct",
    "probe_sharemode_direct",
}
# Helper cap and timeout for probe_sharemode_direct: 8 helpers x 2 s bounds
# (ready and done) x 2 plus spawns stays under the 60 s limit.
EXTRA_ARGS = {
    "probe_sharemode_direct": ["--max-helpers", "8", "--helper-timeout-ms", "2000"],
}

EXTRA_LIBS = ["-ladvapi32", "-luser32"]


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def toolchain_env() -> dict:
    env = dict(os.environ)
    env["PATH"] = "C:/msys64/ucrt64/bin;C:/msys64/usr/bin;" + env.get("PATH", "")
    tmp = "C:/Users/Jackson/AppData/Local/Temp"
    env["TMP"] = env["TEMP"] = tmp
    env["TMPDIR"] = "/c/Users/Jackson/AppData/Local/Temp"
    return env


def host_info() -> dict:
    """Read-only host identity: winreg (HKLM read) and cmd ver."""
    info: dict = {}
    try:
        import winreg

        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion")
        for name in ("CurrentBuild", "UBR", "DisplayVersion", "ProductName", "EditionID"):
            try:
                val, _ = winreg.QueryValueEx(key, name)
                info[name] = val
            except FileNotFoundError:
                info[name] = None
        winreg.CloseKey(key)
    except OSError as e:
        info["registry_error"] = str(e)
    ver = subprocess.run(["cmd", "/c", "ver"], capture_output=True, text=True, timeout=20)
    info["cmd_ver"] = ver.stdout.strip()
    build = info.get("CurrentBuild")
    ubr = info.get("UBR")
    info["build"] = f"{build}.{ubr}" if build and ubr is not None else None
    return info


def toolchain_version() -> str:
    r = subprocess.run([GCC, "--version"], capture_output=True, text=True, env=toolchain_env(), timeout=30)
    return r.stdout.splitlines()[0] if r.stdout else "unknown"


def build_probe(name: str, out_bin: Path) -> dict:
    src = PROBE_DIR / f"{name}.c"
    cmd = [GCC, "-O2", "-m64", "-std=gnu11", "-fno-use-linker-plugin",
           "-o", str(out_bin), str(src), *EXTRA_LIBS]
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True, env=toolchain_env(), timeout=300)
    rec = {"compile_rc": r.returncode, "compile_seconds": round(time.monotonic() - t0, 2)}
    if r.returncode != 0:
        rec["compile_stderr_tail"] = r.stderr.strip().splitlines()[-15:]
    return rec


def run_probe(exe: Path, args: list[str], stdin_text: str | None,
              cwd: Path | None = None, env: dict | None = None) -> dict:
    rec: dict = {"status": None}
    t0 = time.monotonic()
    try:
        r = subprocess.run([str(exe), *args], input=(stdin_text.encode("utf-8") if stdin_text is not None else None), capture_output=True,
                           timeout=TIMEOUT_S, env=(env if env is not None else toolchain_env()), cwd=cwd)
        rc = r.returncode
        rec["rc"] = rc
        rec["rc_hex"] = f"0x{rc & 0xFFFFFFFF:08X}"
        rec["stdout_bytes"] = len(r.stdout)
        rec["stderr_tail"] = r.stderr.decode("utf-8", "replace").strip().splitlines()[-5:]
        rec["stdout"] = r.stdout
        if rc & 0xFFFFFFFF >= 0xC0000000:
            rec["status"] = "crashed"
        elif rc == 0:
            rec["status"] = "ran"
        else:
            rec["status"] = "ran_nonzero"
    except subprocess.TimeoutExpired as e:
        rec["status"] = "timeout"
        rec["stdout"] = e.stdout or b""
        rec["stdout_bytes"] = len(rec["stdout"])
    except PermissionError as e:
        rec["status"] = "blocked"
        rec["error"] = str(e)
    except OSError as e:
        winerr = getattr(e, "winerror", None)
        rec["winerror"] = winerr
        rec["error"] = str(e)
        rec["status"] = "blocked" if winerr in (1260, 4551, 225) else "launch_error"
    rec["seconds"] = round(time.monotonic() - t0, 2)
    return rec


def run_rooted(name: str, exe: Path) -> dict:
    """Run a probe that writes scratch files, inside the sandbox it was built in.

    exe lives in <out>/sandbox-<name>/. The probe gets --root <sandbox>/root;
    cwd and TEMP/TMP are <sandbox>/tmp. Afterwards every entry left under the
    root is recorded and the whole sandbox (binary included) is removed. A probe
    that leaves files behind is reported, not repaired.
    """
    sandbox = exe.parent
    root = sandbox / "root"
    tmp = sandbox / "tmp"
    root.mkdir()
    tmp.mkdir()
    env = toolchain_env()
    env["TMP"] = env["TEMP"] = str(tmp)
    env.pop("NTPERS_PROBE_ALLOW_UNC", None)
    env.pop("NTPERS_PROBE_ROOT", None)
    argv = ["--root", str(root), *EXTRA_ARGS.get(name, [])]
    try:
        rec = run_probe(exe, argv, None, cwd=sandbox, env=env)
        rec["leftover_under_root"] = sorted(
            str(p.relative_to(root)).replace("\\", "/") for p in root.rglob("*"))
    finally:
        shutil.rmtree(sandbox, ignore_errors=True)
    rec["argv"] = ["--root", "<sandbox>/root", *EXTRA_ARGS.get(name, [])]
    return rec


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory, must be outside the repository")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    if ROOT in out.parents or out == ROOT:
        print("refusing: output directory is inside the repository", file=sys.stderr)
        return 2
    (out / "build").mkdir(parents=True, exist_ok=True)
    (out / "run").mkdir(parents=True, exist_ok=True)

    host = host_info()
    tc = toolchain_version()
    print(f"host build {host.get('build')} toolchain {tc}")

    # Probe input for probe_peloader --batch: read-only system images, no code is run from them.
    pe_inputs = ["C:/Windows/System32/notepad.exe", "C:/Windows/System32/kernel32.dll",
                 "C:/Windows/System32/ntdll.dll"]
    pe_list = "\n".join(p for p in pe_inputs if Path(p).exists()) + "\n"

    results = []
    for name in RUN:
        if name in ROOTED:
            exe = out / f"sandbox-{name}" / f"{name}.exe"   # built inside its own sandbox
            exe.parent.mkdir(parents=True, exist_ok=False)
        else:
            exe = out / "build" / f"{name}.exe"
        b = build_probe(name, exe)
        entry: dict = {"probe": name, **b}
        if b["compile_rc"] != 0:
            if name in ROOTED:
                shutil.rmtree(exe.parent, ignore_errors=True)
            entry["status"] = "build_failed"
            results.append(entry)
            print(f"{name}: BUILD_FAILED")
            continue
        entry["binary_sha256"] = sha256_file(exe)
        stdin_text = None
        argv: list[str] = []
        if name == "probe_peloader":
            argv = ["--batch"]
            stdin_text = pe_list
            entry["input_paths"] = pe_inputs
        if name in ROOTED:
            r = run_rooted(name, exe)
        else:
            r = run_probe(exe, argv, stdin_text)
        stdout = r.pop("stdout", b"")
        entry.update(r)
        if isinstance(stdout, str):
            stdout = stdout.encode("utf-8")
        (out / "run" / f"{name}.json").write_bytes(stdout)
        entry["stdout_sha256"] = sha256_bytes(stdout)
        if stdout:
            try:
                json.loads(stdout.decode("utf-8-sig"))
                entry["stdout_is_json"] = True
            except (ValueError, UnicodeDecodeError):
                entry["stdout_is_json"] = False
        else:
            entry["stdout_is_json"] = False
        results.append(entry)
        print(f"{name}: {entry['status']} rc={entry.get('rc_hex')} bytes={len(stdout)}")

    skipped = [{"probe": k, "status": "skipped", "reason": v} for k, v in sorted(SKIP.items())]
    manifest = {
        "schema": "ntpers-golden-capture/1",
        "captured_utc": utc_now(),
        "host": host,
        "toolchain": {"gcc": GCC, "version": tc, "flags": "-O2 -m64 -std=gnu11 -fno-use-linker-plugin"},
        "timeout_seconds": TIMEOUT_S,
        "probes_run": results,
        "probes_skipped": skipped,
        "counts": {
            "run": sum(1 for r in results if r.get("status") in ("ran", "ran_nonzero")),
            "crashed": sum(1 for r in results if r.get("status") == "crashed"),
            "timeout": sum(1 for r in results if r.get("status") == "timeout"),
            "blocked": sum(1 for r in results if r.get("status") == "blocked"),
            "build_failed": sum(1 for r in results if r.get("status") == "build_failed"),
            "launch_error": sum(1 for r in results if r.get("status") == "launch_error"),
            "skipped": len(skipped),
        },
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest["counts"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
