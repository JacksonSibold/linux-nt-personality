#!/bin/sh
set -x
echo "=== probe_path ==="
cp /mnt/share/tests/differential/build/probe_path.exe /tmp/probe_path.exe
chmod +x /tmp/probe_path.exe
timeout 5 /tmp/probe_path.exe > /tmp/probe_out.txt 2>&1
RC=$?
cat /tmp/probe_out.txt
echo "RC=$RC"
dmesg | tail -n 30
echo "$RC" > /mnt/share/.dodresult/result.exitcode
if [ "$RC" -eq 0 ]; then exit 0; else exit 1; fi
