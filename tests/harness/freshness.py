#!/usr/bin/env python3
"""freshness.py - shared freshness gate for all harness entry points"""
import re, subprocess, sys, pathlib, platform

_DRIVE_RE = re.compile(r"^([A-Za-z]):[\\/](.*)$")

def windows_to_wsl_path(path: str) -> str:
    """Translate a Windows drive path to its WSL mount path (C:/x/y becomes /mnt/c/x/y).

    POSIX paths are returned with only separators normalised.
    """
    m = _DRIVE_RE.match(path)
    if m:
        rest = m.group(2).replace("\\", "/")
        return f"/mnt/{m.group(1).lower()}/{rest}"
    return path.replace("\\", "/")

def check_freshness() -> bool:
    """Check source-to-image freshness via vm/freshness.sh. Return True if fresh."""
    root = pathlib.Path(__file__).resolve().parents[2]
    freshness = root / "vm" / "freshness.sh"
    if not freshness.exists():
        return True
    try:
        if platform.system() == "Windows":
            wsl_root = windows_to_wsl_path(str(root))
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
            proc = subprocess.run(["bash", str(freshness), "check"], capture_output=True, text=True, timeout=30)
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

def require_fresh() -> bool:
    """If stale, exit with 125. Returns True if fresh, else exits."""
    if not check_freshness():
        sys.exit(125)
    return True
