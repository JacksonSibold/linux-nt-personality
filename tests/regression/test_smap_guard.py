#!/usr/bin/env python3
"""
regression guard for S2: ensure SMAP is not disabled via kernel cmdline or QEMU flags.
Fails if `nosmap`, `clearcpuid=244`, or `-smap` appears in vm/runvm.sh.
This prevents the correctness shortcut from silently returning.
"""
import pathlib
import sys
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNVM = ROOT / "vm" / "runvm.sh"

def main():
    if not RUNVM.exists():
        print(f"FAIL: {RUNVM} not found", file=sys.stderr)
        return 1
    text = RUNVM.read_text(encoding="utf-8", errors="replace")
    # Check for forbidden patterns
    forbidden = [
        (r"\bnosmap\b", "nosmap kernel cmdline disables SMAP"),
        (r"clearcpuid\s*=\s*244", "clearcpuid=244 disables SMAP CPUID bit 20"),
        (r"-cpu\s+[^\n]*-smap", "QEMU -smap disables SMAP for guest"),
        (r"clearcpuid", "any clearcpuid disables CPU features (SMAP)"),
    ]
    found = []
    for pat, desc in forbidden:
        if re.search(pat, text):
            found.append((pat, desc))
            print(f"FAIL: forbidden pattern {pat!r} found ({desc})", file=sys.stderr)
            # show context
            for i, line in enumerate(text.splitlines(), 1):
                if re.search(pat, line):
                    print(f"  line {i}: {line.strip()}")
    if found:
        print(f"\nRegression guard FAILED: {len(found)} forbidden SMAP-disable patterns found in {RUNVM}", file=sys.stderr)
        print("SMAP must remain enabled. All NT syscalls must use copy_from_user/copy_to_user.", file=sys.stderr)
        return 1
    # Also check that SMAP is not disabled via kernel config? Check for CONFIG_X86_SMAP
    # Ensure that the kernel was built with SMAP support (check .config if available)
    # This is informational, not fatal.
    print(f"PASS: No SMAP-disable patterns found in {RUNVM}")
    print("Checked patterns: nosmap, clearcpuid=244, -smap")
    return 0

if __name__ == "__main__":
    sys.exit(main())
