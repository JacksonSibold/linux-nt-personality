#!/bin/sh
set -x
echo "=== debug ==="
ls -l /windows/system32/ntdll.dll /ntdll.dll /tmp/ntdll.dll 2>&1
echo "--- ntdll check ---"
ls -l /windows/system32/ 2>&1 | head -n 20
echo "--- running minimal ---"
timeout 5 /tests_kernel/minimal.exe; echo "minimal exit:$?"
echo "--- dmesg binfmt ---"
dmesg | grep -E "binfmt_pe|ntdll|NtTerminate" | tail -n 100
echo "--- done ---"
echo "0" > /mnt/share/.dodresult/result.exitcode
