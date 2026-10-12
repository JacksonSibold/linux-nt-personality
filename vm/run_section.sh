#!/bin/sh
set -x
echo "=== section probe ==="
ls -lh /tests_kernel/probe_section_direct_vm.exe
timeout 10 /tests_kernel/probe_section_direct_vm.exe
RC=$?
echo "RC=$RC"
echo "--- probe_section.json ---"
cat /mnt/share/.dodresult/dualrun_vm_probe_section_direct.json 2>&1 | head -n 200
cat /mnt/share/.dodresult/probe_section.json 2>&1 | head -n 200
echo "--- dmesg tail ---"
dmesg | tail -n 100
echo "$RC" > /mnt/share/.dodresult/result.exitcode
exit $RC
