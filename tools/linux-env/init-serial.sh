#!/bin/sh
# tools/linux-env/init-serial.sh - /init for the initrd-serial transport on a host with no C toolchain.
#
# Packed as /init by tools/vm_initrd_pack.py (the Windows host has no gcc to build vm/init.c).
# It keeps the contract of vm/init.c in initrd-serial mode:
#   1. run the test script named by dodtest= (default /mnt/share/vmtests/smoke.sh),
#   2. write its exit code to /mnt/share/.dodresult/result.exitcode,
#   3. print every .dodresult file as framed base64 with /emit-results.sh,
#   4. power off through sysrq.
# /mnt/share is the initramfs payload tree, so no 9p mount is done.
#
# Applets this needs in the archive: sh, mount, echo, sleep (busybox base), and cat, tee,
# date, mkdir, rm, sync, base64, sha256sum, find, sort, cut, tr, wc (symlinks added by the packer).
PATH=/bin
export PATH

mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null

TEST=/mnt/share/vmtests/smoke.sh
read -r CMDLINE < /proc/cmdline
for word in $CMDLINE; do
    case "$word" in
        dodtest=*) TEST="${word#dodtest=}" ;;
    esac
done

mkdir -p /mnt/share/.dodresult
echo "init: transport: initrd-serial (sh init)"
echo "init: running test script: $TEST"
sh "$TEST"
RC=$?
echo "init: test exited with code $RC"
echo "$RC" > /mnt/share/.dodresult/result.exitcode
/bin/sh /emit-results.sh /mnt/share
echo o > /proc/sysrq-trigger
sleep 5
