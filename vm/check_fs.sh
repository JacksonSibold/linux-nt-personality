#!/bin/sh
set -x
echo "=== FS check ==="
ls -l / 2>&1 | head -n 50
ls -l /*.dll 2>&1 | head -n 50
ls -l /ntdll.dll /kernel32.dll /msvcrt.dll 2>&1
ls -l /tmp/*.dll 2>&1 | head
ls -l /windows/system32/ 2>&1 | head -n 50
cat /proc/self/maps | head
echo "try cat via linux"
cat /kernel32.dll 2>&1 | head -c 100 | od -An -tx1 | head
echo "try nt open via test program"
cp /mnt/share/build/probes/test_image_vad.exe /tmp/t.exe 2>&1
ls -l /tmp/t.exe
# direct check of file via nt path using a tiny pe that just opens
# Instead we can try to run a simple PE that does NtCreateFile and prints status
# Use existing mem_vad etc? just run r3 again with more debug
dmesg | tail -n 50
echo "done"
echo 0 > /mnt/share/.dodresult/result.exitcode
