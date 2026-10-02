#!/bin/sh
set -x
echo "=== pmu_elf test ==="
ls -lh /tests_kernel/pmu_elf
/tmp/test_elf 2>&1 | head
/tests_kernel/pmu_elf 2>&1 | head
echo "pmu_elf RC=$?"
dmesg | tail -n 20
echo 42 > /mnt/share/.dodresult/result.exitcode
exit 42
