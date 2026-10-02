#!/bin/sh
set -x
echo "=== mem_vad 4096 test ==="
ls -lh /tests_kernel/mem_vad.exe
timeout 30 /tests_kernel/mem_vad.exe
RC=$?
echo "mem_vad RC=$RC"
dmesg | tail -n 30
if [ "$RC" -eq 0 ]; then echo "PASS"; else echo "FAIL RC=$RC"; fi
# also check for JSON
cat /tmp/mem_vad_diag.json 2>/dev/null | head
# write result
echo "$RC" > /mnt/share/.dodresult/result.exitcode
if [ "$RC" -eq 0 ]; then exit 0; else exit 1; fi
