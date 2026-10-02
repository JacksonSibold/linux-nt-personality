#!/bin/sh
set -eu
mkdir -p /sys/kernel/debug
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then mount -t debugfs none /sys/kernel/debug || true; fi
echo "test counter1"
BEFORE=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
if [ -z "$BEFORE" ]; then BEFORE=0; fi
echo "BEFORE=$BEFORE"
if [ -x /tests_kernel/counter1.exe ]; then /tests_kernel/counter1.exe; echo "rc=$?"; else echo "no exe"; fi
AFTER=$(cat /sys/kernel/debug/ntpers/dispatch_count 2>/dev/null | tr -d ' \n')
if [ -z "$AFTER" ]; then AFTER=0; fi
echo "AFTER=$AFTER DELTA=$((AFTER-BEFORE))"
cat /sys/kernel/debug/ntpers/dispatch_per_ssn
echo "done"
mkdir -p /mnt/share/.dodresult
echo "$((AFTER-BEFORE))" > /mnt/share/.dodresult/result.exitcode
if [ "$((AFTER-BEFORE))" -eq 2 ]; then echo PASS; exit 0; else echo FAIL; exit 1; fi
