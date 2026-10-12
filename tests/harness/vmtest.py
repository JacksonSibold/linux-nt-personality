#!/usr/bin/env python3
"""
vmtest.py - run a Definition-of-Done test inside the ntpers VM.

This is the glue between dodrun.py and vm/runvm.sh.  It takes a guest
(test path inside the 9p share), ensures the VM rig is built, boots the
kernel under QEMU/KVM, runs the named test, and translates the guest
result into a host exit code.

Usage:
    python3 tests/harness/vmtest.py --test /mnt/share/vmtests/smoke.sh
    python3 tests/harness/vmtest.py --test /mnt/share/vmtests/mytest.sh --timeout 120

Exit codes:
    0   - guest test reported PASS
    1   - guest test reported FAIL
    124 - VM timed out
    125 - harness / runvm error
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _paths import wsl_repo_root  # noqa: E402


def project_root() -> Path:
    """Return the repository root from any invocation location."""
    return Path(__file__).resolve().parents[2]


def build_initramfs() -> None:
    """Ensure vm/initramfs.cpio.gz exists."""
    root = project_root()
    initramfs = root / "vm" / "initramfs.cpio.gz"
    if initramfs.exists():
        return
    mkinit = root / "vm" / "mkinitramfs.sh"
    if not mkinit.exists():
        print(f"ERROR: missing {mkinit}", file=sys.stderr)
        sys.exit(125)
    subprocess.run([str(mkinit)], cwd=root, check=True)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run a DoD test inside the ntpers VM",
    )
    p.add_argument(
        "--test",
        required=True,
        help="Guest test path inside the 9p share (e.g. /mnt/share/vmtests/smoke.sh)",
    )
    p.add_argument(
        "--share",
        type=Path,
        default=None,
        help="Host directory shared into the guest (default: project root)",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="Hard VM timeout in seconds (default: 120)",
    )
    p.add_argument(
        "--log",
        type=Path,
        default=None,
        help="Serial console log file (default: auto-generated)",
    )
    p.add_argument(
        "--run-id",
        type=str,
        default=None,
        help="Per-run nonce to correlate host and guest (for staleness protection)",
    )
    return p.parse_args(argv)


def _freshness_check(root: Path) -> bool:
    """Return True if fresh, else print error and return False."""
    freshness_sh = root / "vm" / "freshness.sh"
    if not freshness_sh.exists():
        return True
    # Delegates to freshness.sh check; handles WSL translation if on Windows
    import platform
    try:
        if platform.system() == "Windows":
            # On Windows, delegate freshness check via WSL where KDIR is visible
            wsl_root = wsl_repo_root()
            cmd = ["wsl", "-d", "Ubuntu", "--", "bash", f"{wsl_root}/vm/freshness.sh", "check"]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=sys.stderr)
                print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
        else:
            proc = subprocess.run(["bash", str(freshness_sh), "check"], capture_output=True, text=True, timeout=30)
            if proc.returncode != 0:
                print(proc.stdout, file=sys.stderr)
                print(proc.stderr, file=sys.stderr)
                print("ERROR: STALE BUILD DETECTED - refusing to run VM", file=sys.stderr)
                print("Run: vm/kbuild.sh", file=sys.stderr)
                return False
            return True
    except Exception as e:
        print(f"freshness check failed: {e}", file=sys.stderr)
        return False


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = project_root()
    share = args.share or root
    runvm = root / "vm" / "runvm.sh"

    if not runvm.exists():
        print(f"ERROR: missing {runvm}", file=sys.stderr)
        return 125

    # Freshness gate: refuse to produce result from stale image
    if not _freshness_check(root):
        return 125

    build_initramfs()

    # Delete stale result.exitcode BEFORE starting VM (staleness protection)
    stale_result = Path(share) / ".dodresult" / "result.exitcode"
    try:
        if stale_result.exists():
            stale_result.unlink()
    except Exception as e:
        print(f"warn: cannot unlink {stale_result}: {e}", file=sys.stderr)
    # Also delete any nonce file if run-id provided (guest will write new)
    if args.run_id:
        try:
            (Path(share) / ".dodresult" / f"run_id_{args.run_id[:8]}").unlink()
        except Exception as e:
            print(f"warn: cannot unlink nonce: {e}", file=sys.stderr)
        # Write nonce for guest to consume via kernel cmdline or file
        try:
            (Path(share) / ".dodresult" / "current_run_id").write_text(args.run_id, encoding="utf-8")
        except Exception as e:
            print(f"warn: cannot write current_run_id: {e}", file=sys.stderr)
            return 125

    cmd = [
        str(runvm),
        "--test", args.test,
        "--share", str(share),
        "--timeout", str(args.timeout),
    ]
    if args.log:
        cmd.extend(["--log", str(args.log)])
    if args.run_id:
        cmd.extend(["--run-id", args.run_id])

    env = os.environ.copy()
    env["DODRUN_PROJECT_ROOT"] = str(root)
    if args.run_id:
        env["DODRUN_RUN_ID"] = args.run_id

    proc = subprocess.run(cmd, cwd=root, env=env)
    # Poll for 9p result file visibility with bounded timeout (fix 0.5s fixed-sleep race)
    # runvm.sh writes .dodresult/result.exitcode via 9p; host may not see it immediately.
    # Poll up to 10s so a successful VM run is not misreported as S1 vm_execution / harness ERROR.
    result_file = Path(share) / ".dodresult" / "result.exitcode"
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if result_file.exists():
            break
        time.sleep(0.2)
    # If file now exists but proc returned non-zero due to earlier 9p miss, recover correct exit code
    if result_file.exists():
        try:
            guest_txt = result_file.read_text(encoding="utf-8", errors="replace").strip()
            # guest_txt should be integer exit code
            if guest_txt.lstrip("-").isdigit():
                guest_rc = int(guest_txt)
                if proc.returncode != guest_rc:
                    print(f"vmtest: 9p sync race recovered: proc rc {proc.returncode} -> guest rc {guest_rc}", file=sys.stderr)
                    return guest_rc
        except Exception:
            pass
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
