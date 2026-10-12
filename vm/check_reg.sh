#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNVM="$PROJECT_ROOT/vm/runvm.sh"
TEST_SCRIPT="/mnt/share/vm/test_reg.sh"
SHARE_DIR="$PROJECT_ROOT"
TIMEOUT_SEC="${VM_TIMEOUT:-90}"
RESULT_FILE="$PROJECT_ROOT/.dodresult/result.exitcode"
SERIAL_LOG="$PROJECT_ROOT/.dodresult/reg_serial.log"
OUT_JSON="$PROJECT_ROOT/.dodresult/test_reg.json"
echo "=== REG gate: booting kernel and running reg_probe.exe ==="
echo "PROJECT_ROOT=$PROJECT_ROOT"
echo "TEST_SCRIPT=$TEST_SCRIPT"
echo "TIMEOUT=${TIMEOUT_SEC}s"
echo "SERIAL_LOG=$SERIAL_LOG"
if [ -x "$PROJECT_ROOT/vm/mkinitramfs.sh" ]; then
  echo " rebuilding initramfs..."
  "$PROJECT_ROOT/vm/mkinitramfs.sh" >/dev/null 2>&1 || echo " mkinitramfs warning"
fi
mkdir -p "$PROJECT_ROOT/.dodresult"
rm -f "$RESULT_FILE" "$OUT_JSON"
set +e
bash "$RUNVM" --test "$TEST_SCRIPT" --share "$SHARE_DIR" --timeout "$TIMEOUT_SEC" --log "$SERIAL_LOG"
RUNVM_RC=$?
set -e
if [ ! -f "$RESULT_FILE" ]; then
  echo "REG gate FAIL: result file missing ($RESULT_FILE) runvm_rc=$RUNVM_RC" >&2
  tail -n 100 "$SERIAL_LOG" >&2 || true
  exit 1
fi
GUEST_RC="$(cat "$RESULT_FILE" | tr -d '[:space:]')"
echo "REG gate: guest exit code $GUEST_RC (runvm $RUNVM_RC)"
if [ -f "$OUT_JSON" ]; then cat "$OUT_JSON"; fi
if [ "$GUEST_RC" = "0" ]; then
  echo "=== REG gate PASS ==="
  exit 0
else
  echo "=== REG gate FAIL expected 0 got $GUEST_RC ===" >&2
  cat "$SERIAL_LOG" >&2 || true
  cat "$OUT_JSON" >&2 || true
  exit 1
fi
