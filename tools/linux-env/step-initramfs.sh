#!/bin/sh
# tools/linux-env/step-initramfs.sh - initramfs whose /init runs one PE and powers off.
#
# Generalises tools/linux-env/m0-initramfs.sh for the console milestone steps
# (docs/status/CONSOLE-MILESTONE.md). Runs inside the guest (Debian 12) with
# busybox-static, cpio and gzip. The PE must already be built in REPO/tests/kernel/bin.
#
# Every archive entry is owned by root (cpio --owner 0:0). binfmt_pe's ntdll search
# (pe_map_ntdll) refuses a file that is not root-owned (K-5), and the guest builder
# is uid 1000.
#
# Usage: sh step-initramfs.sh REPO_DIR PE_NAME EXPECT_RC OUT_CPIO_GZ [EXTRA_SRC:EXTRA_DEST ...]
#   PE_NAME      basename without .exe, e.g. console_s1
#   EXPECT_RC    expected exit status of the PE
#   EXTRA_*      extra files packed at the given absolute path, e.g. /ntdll.dll:... (see below)
set -eu

REPO="${1:?usage: step-initramfs.sh REPO_DIR PE_NAME EXPECT_RC OUT_CPIO_GZ [SRC:DEST...]}"
NAME="${2:?PE_NAME}"
EXPECT="${3:?EXPECT_RC}"
OUT="${4:?OUT_CPIO_GZ}"
shift 4
BUSYBOX="${BUSYBOX:-/bin/busybox}"
PE="$REPO/tests/kernel/bin/$NAME.exe"

[ -f "$PE" ] || { echo "step-initramfs: missing $PE (build tests/kernel first)" >&2; exit 1; }
[ -x "$BUSYBOX" ] || { echo "step-initramfs: missing $BUSYBOX (install busybox-static)" >&2; exit 1; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/proc" "$W/sys" "$W/dev" "$W/tests_kernel" "$W/tmp"

cp -L "$BUSYBOX" "$W/bin/busybox"
chmod 755 "$W/bin/busybox"
for applet in sh mount echo sleep cat; do
    ln -s busybox "$W/bin/$applet"
done

cp "$PE" "$W/tests_kernel/$NAME.exe"
chmod 755 "$W/tests_kernel/$NAME.exe"

# Extra files: SRC:DEST, DEST absolute inside the initramfs.
for pair in "$@"; do
    src="${pair%%:*}"
    dest="${pair#*:}"
    mkdir -p "$W$(dirname "$dest")"
    cp -L "$src" "$W$dest"
    chmod 755 "$W$dest"
done

cat > "$W/init" <<EOF
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /tmp
echo "=== STEP init: $NAME (no 9p share) ==="
/tests_kernel/$NAME.exe
RC=\$?
echo "STEP exit status: \$RC (expected $EXPECT)"
if [ "\$RC" -eq $EXPECT ]; then echo "STEP PASS"; else echo "STEP FAIL"; fi
echo o > /proc/sysrq-trigger
sleep 5
EOF
chmod 755 "$W/init"

# Fixed mtime-free archive: newc cpio, root ownership, gzip without the name field (-n).
(cd "$W" && find . | LC_ALL=C sort | cpio -o -H newc --owner 0:0 --quiet | gzip -9 -n) > "$OUT"
echo "step-initramfs: wrote $OUT ($(wc -c < "$OUT") bytes)"
