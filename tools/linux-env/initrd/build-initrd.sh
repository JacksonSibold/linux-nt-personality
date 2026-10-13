#!/usr/bin/env bash
# Builds $LINUX_ENV/initrd/initrd.cpio for the Debian 12 cloud kernel (6.1.0-53-cloud-amd64).
# Inputs must already exist: run fetch-artifacts.sh first.
#  - busybox-static binary (static, runs /init)
#  - virtio modules from the kernel .deb (the cloud kernel builds them as modules)
#  - init (this directory): loads modules, mounts the image root, switch_roots to /sbin/init,
#    or to /bin/bash when the kernel cmdline contains ntpers.shell=bash.
# Windows tar writes cpio entries without the exec bit, so fixmode.js patches /init and busybox to 0755.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LINUX_ENV="${LINUX_ENV:-C:/Users/Jackson/repos/linux-env}"
STAGE="$LINUX_ENV/initrd/stage"
KMOD="$LINUX_ENV/downloads/debian-kernel/tar/lib/modules/6.1.0-53-cloud-amd64/kernel"

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/modules"
cp "$LINUX_ENV/downloads/debian-kernel/bb/bin/busybox" "$STAGE/bin/busybox"
cp "$KMOD/net/core/failover.ko" "$KMOD/drivers/net/net_failover.ko" \
   "$KMOD/drivers/virtio/virtio.ko" "$KMOD/drivers/virtio/virtio_ring.ko" \
   "$KMOD/drivers/virtio/virtio_pci_legacy_dev.ko" "$KMOD/drivers/virtio/virtio_pci_modern_dev.ko" \
   "$KMOD/drivers/virtio/virtio_pci.ko" "$KMOD/drivers/block/virtio_blk.ko" \
   "$KMOD/drivers/net/virtio_net.ko" "$STAGE/modules/"
tr -d '\r' < "$HERE/init" > "$STAGE/init"
chmod 755 "$STAGE/init" "$STAGE/bin/busybox"

OUT="$LINUX_ENV/initrd/initrd.cpio"
rm -f "$OUT"
C:/Windows/System32/tar.exe --format newc -cf "$OUT" -C "$STAGE" .
node "$HERE/fixmode.js" "$OUT" ./init ./bin/busybox
C:/Windows/System32/tar.exe -tvf "$OUT" | grep -E ' \./(init|bin/busybox)$'
echo "built $OUT"
