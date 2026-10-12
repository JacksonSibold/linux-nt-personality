#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_smc"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_smc.json"
LOG="$RESULT_DIR/kernel_smc.log"
mkdir -p "$RESULT_DIR"
echo "=== SMC probe in-VM ===" | tee "$LOG"
PE="/tests_kernel/smc_probe.exe"
if [ ! -x "$PE" ]; then
  echo "PE not found $PE" | tee -a "$LOG"
  cat > "$OUT" <<EOF
{"test":"smc","run_id":"$RUN_ID","status":"ERROR","reason":"PE missing","pe":"$PE"}
EOF
  exit 1
fi
echo "Running $PE" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
echo "SMC PE exit RC=$RC" | tee -a "$LOG"
# RC mapping: 0 success,1 missing_service (STATUS_NOT_IMPLEMENTED),2 alloc fail,3 byte mismatch,4 exec wrong,5 byte2
STATUS="FAIL"
SMCPASS=0
MISSING=""
NOTE="XOR-decrypt-in-place then execute, then read own bytes back"
if [ "$RC" -eq 0 ]; then
  STATUS="PASS"
  SMCPASS=1
elif [ "$RC" -eq 1 ]; then
  MISSING="NtAllocateVirtualMemory (STATUS_NOT_IMPLEMENTED)"
  STATUS="FAIL"
else
  STATUS="FAIL"
fi
# also dump dmesg for binfmt evidence
BINFMT=$(dmesg 2>/dev/null | grep -c "binfmt_pe: entry" 2>/dev/null | tr -d ' \n' || echo 0)
if [ -z "$BINFMT" ]; then BINFMT=0; fi
cat > "$OUT" <<EOF
{
  "test": "smc",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "smc_pass": $SMCPASS,
  "missing_service": "$MISSING",
  "note": "$NOTE",
  "binfmt_hits": $BINFMT,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
