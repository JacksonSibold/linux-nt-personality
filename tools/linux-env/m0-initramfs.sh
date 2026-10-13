#!/bin/sh
# tools/linux-env/m0-initramfs.sh - build an initramfs whose /init runs the M0 PE and powers off.
#
# Runs inside the guest (Debian 12). Needs busybox-static, cpio and gzip, and a built
# tests/kernel/bin/minimal.exe (BUILD-RECIPE section 8).
#
# Why not vm/mkinitramfs.sh: its /init mounts the QEMU 9p share at /mnt/share, and QEMU for
# Windows has no 9p (docs/status/LINUX-ENV.md, blocker 4). This init needs no share: it mounts
# proc and sysfs, runs /tests_kernel/minimal.exe, prints the exit status on the serial console,
# and powers off through sysrq.
#
# Usage: sh m0-initramfs.sh REPO_DIR OUT_CPIO_GZ
set -eu

REPO="${1:?usage: m0-initramfs.sh REPO_DIR OUT_CPIO_GZ}"
OUT="${2:?usage: m0-initramfs.sh REPO_DIR OUT_CPIO_GZ}"
PE="$REPO/tests/kernel/bin/minimal.exe"
BUSYBOX="${BUSYBOX:-/bin/busybox}"

[ -f "$PE" ] || { echo "m0-initramfs: missing $PE (build tests/kernel first)" >&2; exit 1; }
[ -x "$BUSYBOX" ] || { echo "m0-initramfs: missing $BUSYBOX (install busybox-static)" >&2; exit 1; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/proc" "$W/sys" "$W/dev" "$W/tests_kernel"

cp -L "$BUSYBOX" "$W/bin/busybox"
chmod 755 "$W/bin/busybox"
for applet in sh mount echo sleep; do
    ln -s busybox "$W/bin/$applet"
done

cp "$PE" "$W/tests_kernel/minimal.exe"
chmod 755 "$W/tests_kernel/minimal.exe"

cat > "$W/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "=== M0 initramfs init (no 9p share) ==="
/tests_kernel/minimal.exe
RC=$?
echo "M0 exit status: $RC (expected 42)"
if [ "$RC" -eq 42 ]; then echo "M0 PASS"; else echo "M0 FAIL"; fi
echo o > /proc/sysrq-trigger
sleep 5
EOF
chmod 755 "$W/init"

# Fixed mtime-free archive: newc cpio, gzip without the name field (-n).
(cd "$W" && find . | LC_ALL=C sort | cpio -o -H newc --quiet | gzip -9 -n) > "$OUT"
echo "m0-initramfs: wrote $OUT ($(wc -c < "$OUT") bytes)"
