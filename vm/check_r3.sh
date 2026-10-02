#!/bin/sh
set -x
echo "=== R3 minimal test ==="
ls -lh /tmp/r3_minimal.exe /tests_kernel/*.exe 2>&1 | head -n 30
if [ ! -f /tmp/r3_minimal.exe ]; then cp /mnt/share/build/probes/r3_minimal.exe /tmp/r3_minimal.exe; chmod +x /tmp/r3_minimal.exe; fi
ls -l /tmp/r3_minimal.exe
echo "attempt 1 direct run"
timeout 5 /tmp/r3_minimal.exe > /tmp/r3_out.txt 2>&1
RC1=$?
echo "RC1=$RC1"
cat /tmp/r3_out.txt
hexdump -C /tmp/r3_out.txt | head
dmesg | tail -n 200
echo "=== test image_vad ==="
cp /mnt/share/build/probes/test_image_vad.exe /tmp/test_image_vad.exe
chmod +x /tmp/test_image_vad.exe
timeout 5 /tmp/test_image_vad.exe > /tmp/img.txt 2>&1
RC2=$?
echo "RC2=$RC2"
cat /tmp/img.txt
dmesg | tail -n 200
ls /sys/kernel/debug/nt/ 2>&1
cat /sys/kernel/debug/nt/mm_vad 2>&1 | head -n 200
echo "$RC1" > /mnt/share/.dodresult/result.exitcode
if [ $RC1 -eq 42 ]; then echo "R3 PASS"; exit 0; else echo "R3 FAIL"; exit 1; fi
