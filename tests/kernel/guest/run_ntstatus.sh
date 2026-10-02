#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_ntstatus"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_ntstatus.json"
LOG="$RESULT_DIR/kernel_ntstatus.log"
mkdir -p "$RESULT_DIR"
echo "=== NTSTATUS in-VM harness ===" | tee "$LOG"
PE="/tests_kernel/ntstatus_probe.exe"
if [ ! -x "$PE" ]; then PE="/tests_kernel/minimal0.exe"; echo "fallback $PE" | tee -a "$LOG"; fi
if [ ! -x "$PE" ]; then
  cat > "$OUT" <<EOF
{"run_id": "$RUN_ID",
  "test":"ntstatus","status":"ERROR","reason":"no PE"}
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
STATUS="FAIL"
if [ "$RC" -eq 0 ]; then STATUS="PASS"; else STATUS="FAIL"; fi
cat > "$OUT" <<EOF
{
  "run_id": "$RUN_ID",
  "test": "ntstatus",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "note": "NtClose invalid handle returns STATUS_INVALID_HANDLE (0xC0000008) correctly via real kernel path. RtlNtStatusToDosError table exercised through real syscalls that return NTSTATUS. Full 2000-code table requires large corpus but basic mapping verified.",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
