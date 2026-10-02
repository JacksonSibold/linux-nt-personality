#!/bin/sh
set -x
echo "=== R3 import test ==="
cp /mnt/share/build/probes/r3_minimal.exe /tmp/r3_test.exe
chmod +x /tmp/r3_test.exe
ls -l /tmp/r3_test.exe
ls -l /tmp/kernel32.dll /tmp/ntdll.dll || true
ls -l /windows/system32/kernel32.dll || true
echo "--- running R3 ---"
# Capture output: R3 writes to stdout via WriteFile -> should appear in serial?
timeout 5 /tmp/r3_test.exe > /tmp/r3_out.txt 2>&1; echo "R3 exit:$?"
cat /tmp/r3_out.txt
echo "--- dmesg tail ---"
dmesg | tail -n 200
echo "--- done ---"
cat /tmp/r3_out.txt > /mnt/share/.dodresult/r3_output.txt
# Write result based on exit code
RC=$?
# timeout exit is above, need to capture actual PE exit
# Re-run and capture exit via shell
timeout 5 /tmp/r3_test.exe; RC2=$?
echo "RC2=$RC2"
if [ "$RC2" -eq 42 ]; then echo "PASS"; echo "42" > /mnt/share/.dodresult/result.exitcode; exit 0; else echo "FAIL"; echo "$RC2" > /mnt/share/.dodresult/result.exitcode; exit 1; fi
