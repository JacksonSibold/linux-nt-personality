#!/bin/sh
set -eu
echo "=== M0 debug ==="
dmesg | tail -n 20 | cat
echo "---"
/tests_kernel/minimal0.exe
RC=$?
echo "RC=$RC"
dmesg | tail -n 20 | cat
if [ "$RC" -eq 42 ]; then
  echo "M0 PASS"
  exit 0
else
  echo "M0 FAIL"
  exit 1
fi
