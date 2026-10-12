#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_handles"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_handles.json"
LOG="$RESULT_DIR/kernel_handles.log"
mkdir -p "$RESULT_DIR"
echo "=== HANDLES in-VM harness ===" | tee "$LOG"
PE="/tests_kernel/handles_probe.exe"
if [ ! -x "$PE" ]; then PE="/tests_kernel/minimal0.exe"; echo "fallback $PE" | tee -a "$LOG"; fi
if [ ! -x "$PE" ]; then
  cat > "$OUT" <<EOF
{"test":"handles","run_id":"$RUN_ID","status":"ERROR","reason":"no PE"}
EOF
  exit 1
fi
echo "Running $PE" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
echo "RC=$RC" | tee -a "$LOG"
DMESG_NOTIMPL=$(dmesg 2>/dev/null | grep -c "STATUS_NOT_IMPLEMENTED" 2>/dev/null | tr -d ' \n' || echo 0)
if [ -z "$DMESG_NOTIMPL" ]; then DMESG_NOTIMPL=0; fi
echo "dmesg STATUS_NOT_IMPLEMENTED=$DMESG_NOTIMPL" | tee -a "$LOG"
STATUS="FAIL"
MISSING=""
if [ "$RC" -eq 0 ]; then
  STATUS="PASS"
  MISSING=""
else
  STATUS="FAIL"
  if [ "$DMESG_NOTIMPL" -gt 0 ]; then
    DMESG_DETAIL=$(dmesg 2>/dev/null | grep "STATUS_NOT_IMPLEMENTED" | head -1 | tr -d '\n' | cut -c1-200 || echo "")
    MISSING="STATUS_NOT_IMPLEMENTED via dmesg: $DMESG_DETAIL"
  else
    MISSING="RC=$RC no STATUS_NOT_IMPLEMENTED in dmesg"
  fi
fi
HITS=$(dmesg 2>/dev/null | grep -c "ObCreateHandle\|NtCreateEvent" 2>/dev/null | tr -d ' \n' || echo 0)
if [ -z "$HITS" ]; then HITS=0; fi
MISSING_ESC=$(echo "$MISSING" | sed 's/"/\\"/g')
cat > "$OUT" <<EOF
{
  "test": "handles",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING_ESC",
  "dmesg_notimpl": $DMESG_NOTIMPL,
  "dmesg_hits": $HITS,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
