#!/bin/sh
set -eu
echo "=== M0 test ==="
ls -l /tests_kernel/minimal* 2>&1 | cat
echo "---"
/tests_kernel/minimal0.exe
RC=$?
echo "minimal0 RC=$RC"
if [ "$RC" -eq 42 ]; then
  echo "M0 PASS"
  exit 0
else
  echo "M0 FAIL expected 42 got $RC"
  exit 1
fi
