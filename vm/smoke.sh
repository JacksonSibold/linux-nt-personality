#!/bin/bash
# vm/smoke.sh - first real end-to-end test for ntpers (host side).
#
# Boots the current ~/ntpers/linux-6.18 bzImage under QEMU/KVM via
# vm/runvm.sh + vm/mkinitramfs.sh and verifies:
#   1. kernel comes up (guest init reaches user script)
#   2. `uname -a` matches the built version (contains "6.18")
#   3. guest can read AND write the 9p share (marker round-trip)
#
# This proves the rig works before any NT code exists.
# It is NOT a BOOTVM DoD criterion (those require install-from-ISO,
# multiple firmware modes, SSH login) - so it is not registered in
# registry.yaml as a criterion test.  It is the prerequisite for all
# future VM-based criteria.
#
# Usage: ./vm/smoke.sh [--timeout 90]
# Exit: 0 on PASS, 1 on FAIL, 124 on TIMEOUT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TIMEOUT=90

while [[ $# -gt 0 ]]; do
  case "$1" in
    --timeout) TIMEOUT="$2"; shift 2 ;;
    -h|--help) echo "Usage: $0 [--timeout SECS]"; exit 0 ;;
    *) echo "ERROR: unknown arg $1" >&2; exit 2 ;;
  esac
done

BZIMAGE="${BZIMAGE:-$HOME/ntpers/linux-6.18/arch/x86/boot/bzImage}"
INITRD="${INITRD:-$PROJECT_ROOT/vm/initramfs.cpio.gz}"

if [[ ! -f "$BZIMAGE" ]]; then
  echo "FATAL: bzImage not found at $BZIMAGE" >&2
  exit 1
fi

# Ensure initramfs exists (built by mkinitramfs.sh, no host initramfs-tools)
if [[ ! -f "$INITRD" ]]; then
  echo "initramfs missing, building ..."
  bash "$PROJECT_ROOT/vm/mkinitramfs.sh"
fi

echo "=== vm/smoke.sh: host smoke (timeout ${TIMEOUT}s) ==="
echo "kernel: $BZIMAGE"
echo "initrd: $INITRD"
echo "share:  $PROJECT_ROOT"

# Delegate to the canonical VM runner.  It hard-kills the VM on timeout,
# captures full serial console to .dodresult/serial-*.log, and propagates
# the guest result via the RESULT file on the 9p share (never scrapes console).
set +e
bash "$PROJECT_ROOT/vm/runvm.sh" --test /mnt/share/vmtests/smoke.sh --share "$PROJECT_ROOT" --timeout "$TIMEOUT"
RC=$?
set -e

# runvm already propagated the guest exit code; RC==0 means guest SMOKE-PASS
if [[ $RC -eq 0 ]]; then
  echo "=== vm/smoke.sh: PASS (kernel booted under KVM, uname matched 6.18, 9p R/W verified) ==="
  # Show the guest-written marker as proof of R/W
  if [[ -f "$PROJECT_ROOT/.dodresult/smoke.marker" ]]; then
    echo "marker: $(cat "$PROJECT_ROOT/.dodresult/smoke.marker")"
  fi
else
  echo "=== vm/smoke.sh: FAIL rc=$RC ===" >&2
fi

exit $RC
