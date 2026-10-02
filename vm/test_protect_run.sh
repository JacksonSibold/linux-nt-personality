#!/bin/sh
set -x
cp /mnt/share/build/probes/test_protect.exe /tmp/test_protect.exe 2>&1 | head
chmod +x /tmp/test_protect.exe
ls -l /tmp/test_protect.exe
timeout 5 /tmp/test_protect.exe > /tmp/prot_out.txt 2>&1
RC=$?
cat /tmp/prot_out.txt
dmesg | tail -n 20
echo "RC=$RC"
echo "$RC" > /mnt/share/.dodresult/result.exitcode
if [ "$RC" -eq 42 ]; then echo "PASS"; exit 0; else echo "FAIL"; exit 1; fi
