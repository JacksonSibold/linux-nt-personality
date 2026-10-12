#!/bin/sh
set -eu
echo "=== F2 IO test ==="
mkdir -p /tmp
ls -ld /tmp 2>&1 | head -n 5
ls -lh /tests_kernel/ 2>&1 | head -n 20
hexdump -C /tests_kernel/f2_io.exe 2>&1 | head -n 5 || od -An -tx1 /tests_kernel/f2_io.exe 2>&1 | head -n 5
PE="/tests_kernel/f2_io.exe"
if [ ! -x "$PE" ]; then
  echo "f2_io not found"
  exit 1
fi
set +e
"$PE"
RC=$?
set -e
RC_MASKED=$((RC & 0xFF))
if [ "$RC" -ge 128 ]; then RC_MASKED=$((RC-128)); fi
echo "f2_io exit $RC masked $RC_MASKED"
echo "=== dmesg after ==="
dmesg 2>&1 | tail -n 50 || echo "no dmesg"
dmesg | grep -i "nt console\|binfmt_pe\|nt dispatch" | head -n 30 || echo "no console/binfmt"
if [ "$RC_MASKED" -eq 0 ]; then
  echo "F2 PASS"
  exit 0
else
  echo "F2 FAIL"
  exit 1
fi
