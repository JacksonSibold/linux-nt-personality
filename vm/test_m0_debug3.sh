#!/bin/sh
set -eu
echo "=== M0 debug ==="
ls -l /tests_kernel/minimal.exe
hexdump -C /tests_kernel/minimal.exe | head -n 5
echo "Running with strace if available..."
if command -v strace >/dev/null 2>&1; then
  strace /tests_kernel/minimal.exe 2>&1 | head -n 50 || true
else
  echo "no strace"
fi
echo "Running minimal.exe directly..."
set +e
timeout 5 /tests_kernel/minimal.exe
RC=$?
set -e
echo "RC=$RC"
dmesg 2>&1 | tail -n 100
dmesg 2>&1 | grep -i "segfault\|minimal\|binfmt" | tail -n 50
echo "$RC" > /mnt/share/.dodresult/result.exitcode
if [ "$RC" -eq 42 ]; then exit 0; else exit 1; fi
