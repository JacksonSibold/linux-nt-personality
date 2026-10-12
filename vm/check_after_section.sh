#!/bin/sh
timeout 10 /tests_kernel/probe_section_direct_vm.exe
echo "probe done"
cat /sys/kernel/debug/ntpers/dispatch_per_ssn 2>&1 | grep -E "NtCreateSection|NtMapView|NtUnmap|NtQuerySection"
cat /sys/kernel/debug/ntpers/dispatch_count 2>&1
