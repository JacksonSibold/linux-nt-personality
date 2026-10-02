#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_i2"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_i2.json"
LOG="$RESULT_DIR/kernel_i2.log"
mkdir -p "$RESULT_DIR"
echo "=== I2 in-VM harness (dup/query/signal/abandon/ntstatus) ===" | tee "$LOG"
PASS=0
FAIL=0
MISSING=""
RESULTS=""

run_pe() {
  PE="$1"
  NAME="$2"
  echo "--- Running $NAME: $PE ---" | tee -a "$LOG"
  if [ ! -x "$PE" ]; then
    echo "PE not found: $PE" | tee -a "$LOG"
    FAIL=$((FAIL+1))
    MISSING="$MISSING $NAME(missing PE)"
    RESULTS="$RESULTS{\"name\":\"$NAME\",\"status\":\"MISSING\",\"pe\":\"$PE\"},"
    return 1
  fi
  set +e
  "$PE"
  RC=$?
  set -e
  if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
  echo "RC=$RC for $NAME" | tee -a "$LOG"
  if [ "$RC" -eq 0 ]; then
    echo "PASS $NAME" | tee -a "$LOG"
    PASS=$((PASS+1))
    RESULTS="$RESULTS{\"name\":\"$NAME\",\"status\":\"PASS\",\"exit_code\":$RC},"
    return 0
  elif [ "$RC" -eq 2 ] || [ "$RC" -eq 1 ]; then
    echo "FAIL $NAME RC=$RC" | tee -a "$LOG"
    # Check dmesg for NOT_IMPLEMENTED
    if dmesg 2>/dev/null | grep -q "STATUS_NOT_IMPLEMENTED.*$NAME\|missing_service"; then
      MISSING="$MISSING $NAME"
    else
      MISSING="$MISSING $NAME(RC=$RC)"
    fi
    FAIL=$((FAIL+1))
    RESULTS="$RESULTS{\"name\":\"$NAME\",\"status\":\"FAIL\",\"exit_code\":$RC},"
    return 1
  else
    echo "UNKNOWN $NAME RC=$RC" | tee -a "$LOG"
    FAIL=$((FAIL+1))
    RESULTS="$RESULTS{\"name\":\"$NAME\",\"status\":\"FAIL\",\"exit_code\":$RC},"
    return 1
  fi
}

# Run each PE
run_pe "/tests_kernel/i2_dup.exe" "handle_duplication+query" || true
run_pe "/tests_kernel/i2_signal.exe" "event_mutant_semaphore" || true
run_pe "/tests_kernel/i2_abandon.exe" "mutant_abandonment" || true
run_pe "/tests_kernel/i2_ntstatus.exe" "ntstatus_dos_spot" || true

TOTAL=$((PASS+FAIL))
STATUS="FAIL"
if [ "$FAIL" -eq 0 ] && [ "$PASS" -eq 4 ]; then STATUS="PASS"; fi
if [ -z "$MISSING" ]; then MISSING=""; else MISSING=$(echo "$MISSING" | sed 's/^ *//'); fi

# Also check dmesg for NT dispatch hits
HITS=$(dmesg 2>/dev/null | grep -c "do_nt_syscall" 2>/dev/null | tr -d ' \n' || echo 0)
if [ -z "$HITS" ]; then HITS=0; fi

cat > "$OUT" <<EOF
{
  "test": "i2",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pass": $PASS,
  "fail": $FAIL,
  "total": $TOTAL,
  "missing_service": "$MISSING",
  "results": [${RESULTS%,}],
  "dmesg_hits": $HITS,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
