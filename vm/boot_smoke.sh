#!/usr/bin/env bash
# vm/boot_smoke.sh - legacy entrypoint (improved to delegate to canonical rig).
#
# Original first-cut script built its own initramfs from shell.  This version
# delegates to the canonical components (vm/mkinitramfs.sh + vm/runvm.sh +
# vm/smoke.sh) so there is a single source of truth.  Kept for backwards
# compatibility; new callers should use vm/smoke.sh or vm/runvm.sh directly.
#
# Usage:
#   ./vm/boot_smoke.sh [guest_script]       # guest_script optional
#   If no guest_script is given, runs the default smoke (vmtests/smoke.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Back-compat: original script accepted a guest script as $1 and used
# ~/ntpers/vm/share as share via $WORK.  New rig uses project root as share
# and RESULT via .dodresult/result.exitcode.  Map old usage to new.
if [[ $# -gt 0 && -n "${1:-}" && -f "${1:-}" ]]; then
  # Legacy: caller passed a guest script file path
  # Copy it into share/vmtests/legacy_guest.sh and run via runvm
  GUEST_TMP="$PROJECT_ROOT/vmtests/legacy_guest.sh"
  cp "$1" "$GUEST_TMP"
  chmod +x "$GUEST_TMP"
  exec bash "$PROJECT_ROOT/vm/runvm.sh" --test /mnt/share/vmtests/legacy_guest.sh --share "$PROJECT_ROOT" --timeout "${TIMEOUT:-120}"
else
  # No arg -> default smoke (same as vm/smoke.sh)
  exec bash "$PROJECT_ROOT/vm/smoke.sh" --timeout "${TIMEOUT:-120}"
fi
