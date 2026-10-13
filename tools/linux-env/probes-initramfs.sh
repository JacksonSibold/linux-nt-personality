#!/bin/sh
# tools/linux-env/probes-initramfs.sh - build the differential probes for the NT personality and an
# initramfs whose /init runs each probe in turn, framed by serial markers, then powers off.
#
# Generalises tools/linux-env/m0-initramfs.sh (one PE) to every probe in tests/differential/probes
# that has an NTPERS_VM branch (the *_direct probes). Runs inside the guest (Debian 12), needs
# x86_64-w64-mingw32-gcc, busybox-static (with the timeout applet), cpio and gzip.
#
# Probe stdout is the NT console handle -11 (see tools/linux-env/ntpers_vm_stdout.c). The kernel
# prints it on the serial console. Each probe is framed on the console by:
#   NTPERS_PROBE_BEGIN <probe>
#   ...probe stdout (printk lines, prefixed "nt console[-11]: ")...
#   NTPERS_PROBE_RC <probe> <exit status>
#   NTPERS_PROBE_END <probe>
# tools/linux-env/split_probe_serial.py turns the serial log into one JSON file per probe.
#
# Usage: sh probes-initramfs.sh REPO_DIR OUT_CPIO_GZ [PROBE ...]
#   PROBE names are without .c (default: every probes/*_direct.c with an NTPERS_VM branch).
set -eu

REPO="${1:?usage: probes-initramfs.sh REPO_DIR OUT_CPIO_GZ [PROBE...]}"
OUT="${2:?usage: probes-initramfs.sh REPO_DIR OUT_CPIO_GZ [PROBE...]}"
shift 2
PROBES_SRC="$REPO/tests/differential/probes"
SHIM="$REPO/tools/linux-env/ntpers_vm_stdout.c"
CC="${CC:-x86_64-w64-mingw32-gcc}"
BUSYBOX="${BUSYBOX:-/bin/busybox}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-900}"

[ -f "$SHIM" ] || { echo "probes-initramfs: missing $SHIM" >&2; exit 1; }
[ -x "$BUSYBOX" ] || { echo "probes-initramfs: missing $BUSYBOX (install busybox-static)" >&2; exit 1; }
command -v "$CC" >/dev/null 2>&1 || { echo "probes-initramfs: missing $CC" >&2; exit 1; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/proc" "$W/sys" "$W/dev" "$W/tests_kernel" "$W/build" "$W/tmp"

# Probe list: explicit arguments, else every *_direct.c with an NTPERS_VM branch.
if [ $# -gt 0 ]; then
    LIST="$*"
else
    LIST=""
    for f in "$PROBES_SRC"/*_direct.c; do
        grep -qE "NTPERS_VM|VM_BUILD" "$f" || continue
        LIST="$LIST $(basename "$f" .c)"
    done
fi

# Compile the shim once.
"$CC" -c -Os -ffreestanding -fno-builtin -fno-stack-protector -o "$W/build/ntpers_vm_stdout.o" "$SHIM"

for P in $LIST; do
    SRC="$PROBES_SRC/$P.c"
    [ -f "$SRC" ] || { echo "probes-initramfs: missing $SRC" >&2; exit 1; }
    "$CC" -DNTPERS_VM -DVM_BUILD \
        -Dlinux_open=ntpers_vm_open -Dlinux_write=ntpers_vm_write -Dlinux_close=ntpers_vm_close \
        -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp \
        -Os -ffreestanding -fno-builtin -fno-stack-protector -w \
        -o "$W/tests_kernel/$P.exe" "$SRC" "$W/build/ntpers_vm_stdout.o"
    chmod 755 "$W/tests_kernel/$P.exe"
    echo "probes-initramfs: built $P.exe ($(wc -c < "$W/tests_kernel/$P.exe") bytes)" >&2
done

cp -L "$BUSYBOX" "$W/bin/busybox"
chmod 755 "$W/bin/busybox"
for applet in sh mount echo sleep timeout; do
    ln -s busybox "$W/bin/$applet"
done

{
    echo '#!/bin/sh'
    echo 'mount -t proc proc /proc'
    echo 'mount -t sysfs sysfs /sys'
    # The NT path C:\tmp maps to /tmp in the personality (src/kernel/nt/io.c with g_kshim_root "/"),
    # and the probes create their scratch files there. Windows has C:\tmp; this provides it.
    echo 'mount -t tmpfs tmpfs /tmp'
    # Markers go through /dev/kmsg so they share the printk sequence with the probe's console
    # output (the probe's stdout is printk'd asynchronously; a plain echo can overtake it).
    echo 'mknod /dev/kmsg c 1 11 2>/dev/null'
    echo 'mark() { echo "$1" > /dev/kmsg 2>/dev/null || echo "$1"; }'
    echo 'mark "=== NT probes initramfs init (no 9p share) ==="'
    for P in $LIST; do
        echo "mark \"NTPERS_PROBE_BEGIN $P\""
        echo "timeout $PROBE_TIMEOUT /tests_kernel/$P.exe"
        echo "RC=\$?"
        echo "mark \"NTPERS_PROBE_RC $P \$RC\""
        echo "mark \"NTPERS_PROBE_END $P\""
    done
    echo 'mark "NTPERS_ALL_DONE"'
    echo 'echo o > /proc/sysrq-trigger'
    echo 'sleep 5'
} > "$W/init"
chmod 755 "$W/init"

# Fixed mtime-free archive: newc cpio, gzip without the name field (-n).
(cd "$W" && find . -path ./build -prune -o -print | LC_ALL=C sort | cpio -o -H newc --quiet | gzip -9 -n) > "$OUT"
echo "probes-initramfs: wrote $OUT ($(wc -c < "$OUT") bytes) probes:$LIST" >&2
