#!/bin/sh
set -u
echo "=== ARCH simple test ==="
mkdir -p /sys/kernel/debug
mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>&1 | head || true
cat /sys/kernel/debug/ntpers/dispatch_count 2>&1 | head
cat /sys/kernel/debug/ntpers/host_count 2>&1 | head
BEFORE=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n'); [ -z "$BEFORE" ] && BEFORE=0
BEFORE_HOST=$(cat /sys/kernel/debug/ntpers/host_count 2>/dev/null | tr -d ' \n'); [ -z "$BEFORE_HOST" ] && BEFORE_HOST=0
echo "BEFORE NT=$BEFORE HOST=$BEFORE_HOST"
i=1; SUCCESS=0
while [ "$i" -le 100 ]; do
  /tests_kernel/minimal0.exe 2>/dev/null
  RC=$?
  if [ "$RC" -eq 0 ] || [ "$RC" -eq 42 ]; then SUCCESS=$((SUCCESS+1)); fi
  i=$((i+1))
done
AFTER=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n'); [ -z "$AFTER" ] && AFTER=0
AFTER_HOST=$(cat /sys/kernel/debug/ntpers/host_count 2>/dev/null | tr -d ' \n'); [ -z "$AFTER_HOST" ] && AFTER_HOST=0
NT=$((AFTER - BEFORE))
HOST=$((AFTER_HOST - BEFORE_HOST))
echo "100 launches NT=$NT HOST=$HOST success=$SUCCESS"
if [ "$NT" -gt 0 ]; then RATIO=$(awk "BEGIN{printf \"%.4f\", $HOST/$NT}"); echo "RATIO=$RATIO"; fi
cat /sys/kernel/debug/ntpers/dispatch_per_ssn 2>/dev/null | head -n 100
mkdir -p /mnt/share/.dodresult
cat > /mnt/share/.dodresult/arch_simple.json <<EOF
{"nt100": $NT, "host100": $HOST, "success": $SUCCESS}
EOF
cat /mnt/share/.dodresult/arch_simple.json
if [ "$HOST" -eq 0 ] && [ "$NT" -gt 0 ]; then echo "HOST zero OK"; else echo "HOST non-zero"; fi
awk "BEGIN{exit !( $HOST / $NT < 0.05 )}" && echo "RATIO OK" || echo "RATIO FAIL"
exit 0
