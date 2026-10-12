#!/bin/sh
set -x
echo "=== test_image_vad ==="
cp /mnt/share/build/probes/test_image_vad.exe /tmp/test_image_vad.exe
chmod +x /tmp/test_image_vad.exe
ls -l /tmp/test_image_vad.exe
timeout 5 /tmp/test_image_vad.exe > /tmp/img_out.txt 2>&1
RC=$?
cat /tmp/img_out.txt
echo "RC=$RC"
dmesg | tail -n 30
# also capture serial log already has output via NtWriteFile (which goes to console)
# Check if output contains PASS
if grep -q "PASS" /tmp/img_out.txt; then echo "IMG TEST PASS"; else echo "IMG TEST FAIL"; fi
echo "$RC" > /mnt/share/.dodresult/result.exitcode
if [ "$RC" -eq 42 ]; then exit 0; else exit 1; fi
