#!/bin/sh
set -eu
echo "=== Validation: exact N increments ==="
mkdir -p /sys/kernel/debug
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then mount -t debugfs none /sys/kernel/debug || true; fi
read_counter() {
  _p="$1"
  _v=$(cat "$_p" 2>/dev/null | tr -d ' \n\r' || echo 0)
  if [ -z "$_v" ]; then _v=0; fi
  case "$_v" in ''|*[!0-9]*) _v=0;; esac
  echo "$_v"
}
DISPATCH="/sys/kernel/debug/ntpers/dispatch_count"
HOST="/sys/kernel/debug/ntpers/host_count"
PER_SSN="/sys/kernel/debug/ntpers/dispatch_per_ssn"
echo "Initial dispatch: $(read_counter $DISPATCH) host: $(read_counter $HOST)"
# Test 1 launch
echo "--- Test 1 launch minimal0 (expected delta 1) ---"
BEFORE=$(read_counter $DISPATCH)
set +e
/tests_kernel/minimal0.exe 2>/dev/null
RC=$?
set -e
AFTER=$(read_counter $DISPATCH)
DELTA=$((AFTER - BEFORE))
echo "RC=$RC BEFORE=$BEFORE AFTER=$AFTER DELTA=$DELTA"
if [ "$DELTA" -eq 1 ]; then echo "PASS 1 launch"; else echo "FAIL 1 launch expected 1 got $DELTA"; fi
# Test 5 launches
echo "--- Test 5 launches minimal0 (expected delta 5) ---"
BEFORE=$(read_counter $DISPATCH)
i=1
set +e
while [ "$i" -le 5 ]; do /tests_kernel/minimal0.exe 2>/dev/null; i=$((i+1)); done
set -e
AFTER=$(read_counter $DISPATCH)
DELTA=$((AFTER - BEFORE))
echo "BEFORE=$BEFORE AFTER=$AFTER DELTA=$DELTA"
if [ "$DELTA" -eq 5 ]; then echo "PASS 5 launches"; else echo "FAIL 5 launches expected 5 got $DELTA"; fi
# Test 10 launches
echo "--- Test 10 launches minimal0 (expected delta 10) ---"
BEFORE=$(read_counter $DISPATCH)
i=1
set +e
while [ "$i" -le 10 ]; do /tests_kernel/minimal0.exe 2>/dev/null; i=$((i+1)); done
set -e
AFTER=$(read_counter $DISPATCH)
DELTA=$((AFTER - BEFORE))
echo "BEFORE=$BEFORE AFTER=$AFTER DELTA=$DELTA"
if [ "$DELTA" -eq 10 ]; then echo "PASS 10 launches"; else echo "FAIL 10 launches expected 10 got $DELTA"; fi
# Test host counter remains 0
echo "--- Host counter check (should be 0) ---"
HOST_VAL=$(read_counter $HOST)
echo "HOST=$HOST_VAL"
if [ "$HOST_VAL" -eq 0 ]; then echo "PASS host 0"; else echo "FAIL host not 0: $HOST_VAL"; fi
# Per-SSN check
echo "--- Per-SSN after tests ---"
cat "$PER_SSN" 2>/dev/null | head -n 50
# 100 launches for ratio
echo "--- 100 launches for ratio (like run_arch) ---"
BEFORE_NT=$(read_counter $DISPATCH)
BEFORE_HOST=$(read_counter $HOST)
i=1
SUCCESS=0
set +e
while [ "$i" -le 100 ]; do /tests_kernel/minimal0.exe 2>/dev/null; RC=$?; if [ "$RC" -eq 42 ] || [ "$RC" -eq 0 ]; then SUCCESS=$((SUCCESS+1)); fi; i=$((i+1)); done
set -e
AFTER_NT=$(read_counter $DISPATCH)
AFTER_HOST=$(read_counter $HOST)
NT=$((AFTER_NT - BEFORE_NT))
HOST2=$((AFTER_HOST - BEFORE_HOST))
echo "100 launches NT=$NT HOST=$HOST2 SUCCESS=$SUCCESS"
if [ "$NT" -gt 0 ]; then RATIO=$(awk "BEGIN{printf \"%.4f\", $HOST2/$NT}"); echo "RATIO=$RATIO"; else RATIO="0.00"; echo "RATIO=0.00"; fi
echo "Per launch NT: $(awk "BEGIN{printf \"%.2f\", $NT/100}")"
# Write json
mkdir -p /mnt/share/.dodresult
cat > /mnt/share/.dodresult/validation.json <<EOF
{
  "test": "validation",
  "delta_1": $DELTA,
  "nt_100": $NT,
  "host_100": $HOST2,
  "ratio": "$RATIO",
  "success": $SUCCESS,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat /mnt/share/.dodresult/validation.json
echo "=== Validation done ==="
exit 0
