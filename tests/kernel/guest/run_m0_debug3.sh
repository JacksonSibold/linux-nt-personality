#!/bin/sh
set -eu
echo "=== M0 debug3 ==="
dmesg -c > /dev/null 2>&1 || true
echo "before dmesg cleared"
/tests_kernel/minimal0.exe
RC=$?
echo "RC=$RC"
dmesg | tail -n 50 | cat
echo "---"
dmesg | grep -i "binfmt\|NT\|do_nt\|TIF" | head -n 50 | cat
if [ "$RC" -eq 42 ]; then
  echo "M0 PASS"
  exit 0
else
  echo "M0 FAIL"
  exit 1
fi
