#!/bin/sh
set -eu
echo "=== Counter validation ==="
cat /proc/sys/kernel/osrelease
mkdir -p /sys/kernel/debug
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
  mount -t debugfs none /sys/kernel/debug 2>&1 | head || true
fi
echo "debugfs mounted"
ls -l /sys/kernel/debug/ntpers/ 2>&1 | head -n 50
echo "dispatch_count:"
cat /sys/kernel/debug/ntpers/dispatch_count 2>&1 | head
echo "host_count:"
cat /sys/kernel/debug/ntpers/host_count 2>&1 | head
echo "per_ssn:"
cat /sys/kernel/debug/ntpers/dispatch_per_ssn 2>&1 | head -n 50
echo "before"
BEFORE=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
if [ -z "$BEFORE" ]; then BEFORE=0; fi
echo "BEFORE=$BEFORE"
if [ -x /tests_kernel/minimal0.exe ]; then
  /tests_kernel/minimal0.exe
  echo "minimal0 rc=$?"
else
  /pe_min.exe
  echo "pe_min rc=$?"
fi
AFTER=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
if [ -z "$AFTER" ]; then AFTER=0; fi
echo "AFTER=$AFTER"
DELTA=$((AFTER - BEFORE))
echo "DELTA=$DELTA"
# Validate N=7 test if present
if [ -x /tests_kernel/counter7.exe ]; then
  echo "=== N=7 test ==="
  BEFORE2=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
  if [ -z "$BEFORE2" ]; then BEFORE2=0; fi
  /tests_kernel/counter7.exe
  RC=$?
  echo "counter7 rc=$RC"
  AFTER2=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
  if [ -z "$AFTER2" ]; then AFTER2=0; fi
  DELTA2=$((AFTER2 - BEFORE2))
  echo "DELTA7=$DELTA2 expected 8"
  HOST=$(cat /sys/kernel/debug/ntpers/host_count 2>/dev/null | tr -d ' \n')
  if [ -z "$HOST" ]; then HOST=0; fi
  echo "HOST=$HOST"
fi
# 100 launches
echo "=== 100 launches ==="
BEFORE_BIG=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
if [ -z "$BEFORE_BIG" ]; then BEFORE_BIG=0; fi
BEFORE_HOST=$(cat /sys/kernel/debug/ntpers/host_count 2>/dev/null | tr -d ' \n')
if [ -z "$BEFORE_HOST" ]; then BEFORE_HOST=0; fi
i=1; SUCCESS=0
while [ "$i" -le 100 ]; do
  /tests_kernel/minimal0.exe 2>/dev/null
  RC=$?
  if [ "$RC" -eq 42 ] || [ "$RC" -eq 0 ]; then SUCCESS=$((SUCCESS+1)); fi
  i=$((i+1))
done
AFTER_BIG=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
if [ -z "$AFTER_BIG" ]; then AFTER_BIG=0; fi
AFTER_HOST2=$(cat /sys/kernel/debug/ntpers/host_count 2>/dev/null | tr -d ' \n')
if [ -z "$AFTER_HOST2" ]; then AFTER_HOST2=0; fi
NT=$((AFTER_BIG - BEFORE_BIG))
HOST2=$((AFTER_HOST2 - BEFORE_HOST))
echo "100 launches NT=$NT HOST=$HOST2 success=$SUCCESS"
if [ "$NT" -gt 0 ]; then RATIO=$(awk "BEGIN{printf \"%.4f\", $HOST2/$NT}"); echo "RATIO=$RATIO"; fi
echo "per_ssn after 100:"
cat /sys/kernel/debug/ntpers/dispatch_per_ssn 2>/dev/null | head -n 100
mkdir -p /mnt/share/.dodresult
cat > /mnt/share/.dodresult/counter_validation.json <<EOF
{"before": $BEFORE, "after": $AFTER, "delta": $DELTA, "nt100": $NT, "host100": $HOST2, "success": $SUCCESS}
EOF
cat /mnt/share/.dodresult/counter_validation.json
exit 0
