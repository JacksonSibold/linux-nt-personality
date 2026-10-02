#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/test_debugfs.json"
LOG="$RESULT_DIR/test_debugfs.log"
mkdir -p "$RESULT_DIR"
echo "=== debugfs selftest ===" | tee "$LOG"
# Check debugfs mounted
if [ ! -d /sys/kernel/debug ]; then
  mount -t debugfs none /sys/kernel/debug 2>&1 | tee -a "$LOG" || true
fi
ls -la /sys/kernel/debug/ntpers/ 2>&1 | tee -a "$LOG" || echo "no ntpers dir" | tee -a "$LOG"
if [ ! -e /sys/kernel/debug/ntpers/selftest ]; then
  echo "selftest not found" | tee -a "$LOG"
  cat > "$OUT" <<EOF
{"test":"debugfs","status":"FAIL","reason":"no selftest file","timestamp":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
EOF
  echo "1" > "$SHARE/.dodresult/result.exitcode"
  exit 1
fi
echo "Running debugfs selftest..." | tee -a "$LOG"
if echo run > /sys/kernel/debug/ntpers/selftest 2>&1 | tee -a "$LOG"; then
  echo "write ok" | tee -a "$LOG"
else
  echo "write failed" | tee -a "$LOG"
fi
cat /sys/kernel/debug/ntpers/selftest 2>&1 | tee -a "$LOG" | tee /tmp/selftest_out
dmesg | grep -i ntpers | tail -n 20 | tee -a "$LOG" || true
OUT_CONTENT=$(cat /sys/kernel/debug/ntpers/selftest 2>&1 || echo "fail")
echo "selftest output: $OUT_CONTENT" | tee -a "$LOG"
STATUS="FAIL"
if echo "$OUT_CONTENT" | grep -q "OVERALL:PASS"; then
  STATUS="PASS"
elif echo "$OUT_CONTENT" | grep -q "PASS"; then
  STATUS="PASS"
fi
cat > "$OUT" <<EOF
{
  "test": "debugfs",
  "status": "$STATUS",
  "output": "$(cat /sys/kernel/debug/ntpers/selftest 2>&1 | tr -d '\n' | sed 's/"/\\"/g')",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
# Write exit code for harness
if [ "$STATUS" = "PASS" ]; then echo "0" > "$SHARE/.dodresult/result.exitcode"; exit 0; else echo "1" > "$SHARE/.dodresult/result.exitcode"; exit 1; fi
