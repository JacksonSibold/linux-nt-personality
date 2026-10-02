#!/bin/sh
set -eu
echo "=== dmesg ntpers ==="
dmesg | grep -i ntpers | head -n 20 || echo "no ntpers in dmesg"
echo "=== debugfs ls ==="
ls -la /sys/kernel/debug/ 2>&1 | head -n 30
ls -la /sys/kernel/debug/ntpers/ 2>&1 | head -n 30 || echo "no ntpers dir"
cat /sys/kernel/debug/ntpers/selftest 2>&1 | head -n 100 || echo "no selftest"
echo "=== kallsyms ==="
grep nt_debug /proc/kallsyms 2>&1 | head -n 20 || echo "no kallsyms"
