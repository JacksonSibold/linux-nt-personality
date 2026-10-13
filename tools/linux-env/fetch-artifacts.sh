#!/usr/bin/env bash
# Downloads and verifies every external artifact used by the Linux guest, and extracts what is needed
# WITHOUT running any installer. Every download is checked against a published SHA256/SHA512.
# Nothing here elevates. Downloads land in $LINUX_ENV/downloads/<name>/ (one empty dir per artifact).
# Usage: tools/linux-env/fetch-artifacts.sh
set -euo pipefail
LINUX_ENV="${LINUX_ENV:-C:/Users/Jackson/repos/linux-env}"
D="$LINUX_ENV/downloads"
mkdir -p "$D"

fetch() { # fetch <url> <dest-file> <sha256>
    local url="$1" dest="$2" want="$3"
    mkdir -p "$(dirname "$dest")"
    curl -sS -L --max-time 900 -o "$dest" "$url"
    echo "$want  $dest" | sha256sum -c -
}

# 1. 7-Zip standalone tools (official 7-zip.org). 7zr extracts the "extra" package, which gives 7za.
mkdir -p "$D/7zip"
fetch https://www.7-zip.org/a/7zr.exe "$D/7zip/7zr.exe" 256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282
fetch https://www.7-zip.org/a/7z2604-extra.7z "$D/7zip/7z2604-extra.7z" dc4b11d3399db18b063630137145f5585d8f7ac847bf3639bd1185d7d1f7cee0
mkdir -p "$D/7zip/extra"
"$D/7zip/7zr.exe" x -y -o"$D/7zip/extra" "$D/7zip/7z2604-extra.7z" >/dev/null

# 2. Full 7-Zip (has the NSIS reader) taken from the official MSI payload; the MSI is only extracted, not installed.
fetch https://www.7-zip.org/a/7z2604-x64.msi "$D/7zip-msi/7z2604-x64.msi" 0b01334a418654293513449f61d6bfc99e5196ca371e3c3dc961eda57cd535c6
mkdir -p "$D/7zip-msi/x" "$LINUX_ENV/tools-7z"
"$D/7zip/extra/7za.exe" x -y -o"$D/7zip-msi/x" "$D/7zip-msi/7z2604-x64.msi" >/dev/null
cp "$D/7zip-msi/x/_7z.exe" "$LINUX_ENV/tools-7z/7z.exe"
cp "$D/7zip-msi/x/_7z.dll" "$LINUX_ENV/tools-7z/7z.dll"

# 3. QEMU 11.1.0 setup from the official weilnetz mirror (winget manifest SoftwareFreedomConservancy.QEMU).
#    NOT executed: its embedded manifest requires administrator, so it is only unpacked with 7z.
fetch https://qemu.weilnetz.de/w64/2026/qemu-w64-setup-20260811.exe "$D/qemu/qemu-w64-setup-20260811.exe" f98a8aeb5f7faea9765b6dee28316c266cd179d80354a2fed8e50176f9a2e59f
mkdir -p "$LINUX_ENV/qemu"
"$LINUX_ENV/tools-7z/7z.exe" x -y -o"$LINUX_ENV/qemu" "$D/qemu/qemu-w64-setup-20260811.exe" >/dev/null

# 4. Debian 12 generic cloud image (cloud.debian.org), SHA512 from the official SHA512SUMS.
mkdir -p "$D/debian"
curl -sS -L --max-time 900 -o "$D/debian/debian-12-genericcloud-amd64.qcow2" \
    https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-genericcloud-amd64.qcow2
echo "a09170d17e13af43da61774666f520e3bbec0f8bd96b4db37c15f3426fb327ccad950b80dbe78c09aa5ca90ce5015e99334b0ba5188fd4018736bc3fb5f0882c  $D/debian/debian-12-genericcloud-amd64.qcow2" | sha512sum -c -

# 5. Debian cloud kernel 6.1.0-53-cloud-amd64 (the image's kernel), checked against the bookworm-security Packages index.
mkdir -p "$D/debian-kernel"
K="$D/debian-kernel/linux-image-6.1.0-53-cloud-amd64-unsigned_6.1.187-1_amd64.deb"
fetch https://deb.debian.org/debian-security/pool/updates/main/l/linux/linux-image-6.1.0-53-cloud-amd64-unsigned_6.1.187-1_amd64.deb \
    "$K" 9001721c6ba33401f95bbeeb0e41ec91ce2cdb5aaceac6133cb9d093734bba0e
mkdir -p "$D/debian-kernel/deb" "$D/debian-kernel/tar" "$LINUX_ENV/kernel"
"$LINUX_ENV/tools-7z/7z.exe" x -y -o"$D/debian-kernel/deb" "$K" >/dev/null
"$LINUX_ENV/tools-7z/7z.exe" x -y -o"$D/debian-kernel/tar" "$D/debian-kernel/deb/data.tar" >/dev/null
cp "$D/debian-kernel/tar/boot/vmlinuz-6.1.0-53-cloud-amd64" "$LINUX_ENV/kernel/"

# 6. busybox-static 1.35.0 (bookworm main), for the initramfs.
BB="$D/debian-kernel/busybox-static_1.35.0-4+deb12u1+b1_amd64.deb"
fetch https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.35.0-4+deb12u1+b1_amd64.deb "$BB" \
    3d3fdbe91d4660c873e14b092c213fe81c1da6362daa236eb25d0171eb108744
mkdir -p "$D/debian-kernel/bb"
"$LINUX_ENV/tools-7z/7z.exe" x -y -o"$D/debian-kernel/bb" "$BB" >/dev/null
"$LINUX_ENV/tools-7z/7z.exe" x -y -o"$D/debian-kernel/bb/data" "$D/debian-kernel/bb/data.tar" >/dev/null 2>&1 || true
mkdir -p "$D/debian-kernel/bb/bin"
cp "$D/debian-kernel/bb/data/bin/busybox" "$D/debian-kernel/bb/bin/busybox"
echo "fetch complete"
