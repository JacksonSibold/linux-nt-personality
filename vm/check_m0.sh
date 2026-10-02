#!/bin/bash
# check_m0.sh - M0 hard regression gate: boots current kernel, runs minimal.exe, requires exit 42
# This script is the durable fix for M0 having regressed twice without notice.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNVM="$PROJECT_ROOT/vm/runvm.sh"
TEST_SCRIPT="/mnt/share/vm/test_m0.sh"
SHARE_DIR="$PROJECT_ROOT"
TIMEOUT_SEC="${VM_TIMEOUT:-90}"
RESULT_FILE="$PROJECT_ROOT/.dodresult/result.exitcode"
SERIAL_LOG="$PROJECT_ROOT/.dodresult/m0_gate_serial.log"
M0_JSON="$PROJECT_ROOT/.dodresult/test_m0.json"

echo "=== M0 gate: booting kernel and running minimal.exe (expect 42) ==="
echo "PROJECT_ROOT=$PROJECT_ROOT"
echo "TEST_SCRIPT=$TEST_SCRIPT"
echo "TIMEOUT=${TIMEOUT_SEC}s"
echo "SERIAL_LOG=$SERIAL_LOG"

# Ensure initramfs is fresh (contains current ntdll and PEs)
if [ -x "$PROJECT_ROOT/vm/mkinitramfs.sh" ]; then
  echo " rebuilding initramfs..."
  "$PROJECT_ROOT/vm/mkinitramfs.sh" >/dev/null 2>&1 || echo " mkinitramfs warning: non-zero exit"
fi

mkdir -p "$PROJECT_ROOT/.dodresult"
rm -f "$RESULT_FILE" "$M0_JSON"

# Boot VM and run M0 test
set +e
"$RUNVM" --test "$TEST_SCRIPT" --share "$SHARE_DIR" --timeout "$TIMEOUT_SEC" --log "$SERIAL_LOG"
RUNVM_RC=$?
set -e

# runvm exits with guest RC, but we check result files
if [ ! -f "$RESULT_FILE" ]; then
  echo "M0 gate FAIL: result file missing ($RESULT_FILE) runvm_rc=$RUNVM_RC" >&2
  echo "--- serial log tail ---" >&2
  tail -n 100 "$SERIAL_LOG" >&2 || true
  exit 1
fi

GUEST_RC="$(cat "$RESULT_FILE" | tr -d '[:space:]')"
echo "M0 gate: guest exit code $GUEST_RC (runvm $RUNVM_RC)"

if [ -f "$M0_JSON" ]; then
  cat "$M0_JSON"
fi

# Also check serial log for NtTerminateProcess with 42 if available
if grep -q "exit 42\|status=2a\|code 42\|2a" "$SERIAL_LOG" 2>/dev/null; then
  echo " serial log contains 0x2a reference"
fi

# The init writes result.exitcode as the test script exit code (0 on PASS),
# while the PE exit code is in test_m0.json. Check JSON for truth.
if [ -f "$M0_JSON" ] && grep -q '"status": "PASS"' "$M0_JSON"; then
  JSON_RC=$(grep -o '"exit_code": [0-9]*' "$M0_JSON" | grep -o '[0-9]*' | head -1 || echo 0)
  JSON_MASKED=$(grep -o '"exit_masked": [0-9]*' "$M0_JSON" | grep -o '[0-9]*' | head -1 || echo 0)
  if [ "$JSON_RC" = "42" ] || [ "$JSON_MASKED" = "42" ]; then
    echo "=== M0 gate PASS: minimal.exe exited 42 (via JSON) ==="
    exit 0
  fi
fi

if [ "$GUEST_RC" = "42" ]; then
  echo "=== M0 gate PASS: minimal.exe exited 42 ==="
  exit 0
else
  echo "=== M0 gate FAIL: expected 42, got $GUEST_RC ===" >&2
  echo "--- serial log ---" >&2
  cat "$SERIAL_LOG" >&2 || true
  echo "--- test_m0.json ---" >&2
  cat "$M0_JSON" >&2 || true
  exit 1
fi
