#!/bin/sh
set -eu
echo "=== M0 42 test ==="
dmesg -c > /dev/null 2>&1 || true
/tests_kernel/minimal.exe
RC=$?
echo "RC=$RC"
dmesg | tail -n 20 | cat
if [ "$RC" -eq 42 ]; then
  echo "M0 PASS 42"
  exit 0
else
  echo "M0 FAIL expected 42 got $RC"
  exit 1
fi
