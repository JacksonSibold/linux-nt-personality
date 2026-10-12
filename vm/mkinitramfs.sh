#!/bin/bash
# vm/mkinitramfs.sh - build a minimal initramfs for the ntpers VM test rig.
#
# This script is self-contained: it does NOT use the host distro's
# initramfs-tools.  It compiles a hand-rolled C /init, copies a minimal
# shell (dash + glibc), and packages everything into a gzip-compressed
# cpio archive.
#
# Usage:
#   ./vm/mkinitramfs.sh [output_file]
#
# Default output: vm/initramfs.cpio.gz

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT="${1:-${PROJECT_ROOT}/vm/initramfs.cpio.gz}"
WORK="$(mktemp -d)"

cleanup() {
    rm -rf "${WORK}"
}
trap cleanup EXIT

# Build the static init binary.
echo "Building hand-rolled /init ..."
gcc -std=c11 -Wall -Wextra -O2 -static \
    "${SCRIPT_DIR}/init.c" \
    -o "${WORK}/init"
strip "${WORK}/init"

# Directory layout.
mkdir -p "${WORK}/bin" "${WORK}/proc" "${WORK}/sys" "${WORK}/dev" "${WORK}/mnt" "${WORK}/mnt/share"

# Static busybox shell and applets.  Prefer the standalone static build if
# available; it does not depend on the host distro's initramfs-tools.
BUSYBOX="/bin/busybox"
if [[ ! -x "${BUSYBOX}" ]]; then
    BUSYBOX="$(command -v busybox || true)"
fi
if [[ -z "${BUSYBOX}" || ! -x "${BUSYBOX}" ]]; then
    echo "ERROR: busybox not found. Install busybox-static." >&2
    exit 1
fi

cp -L "${BUSYBOX}" "${WORK}/bin/busybox"
chmod 755 "${WORK}/bin/busybox"
ln -s busybox "${WORK}/bin/sh"
ln -s busybox "${WORK}/bin/mount"
ln -s busybox "${WORK}/bin/umount"
ln -s busybox "${WORK}/bin/cat"
ln -s busybox "${WORK}/bin/echo"
ln -s busybox "${WORK}/bin/grep"
ln -s busybox "${WORK}/bin/ls"
ln -s busybox "${WORK}/bin/mkdir"
ln -s busybox "${WORK}/bin/rm"
ln -s busybox "${WORK}/bin/sync"
ln -s busybox "${WORK}/bin/uname"
ln -s busybox "${WORK}/bin/hostname"
ln -s busybox "${WORK}/bin/date"
ln -s busybox "${WORK}/bin/timeout" 2>/dev/null || true
ln -s busybox "${WORK}/bin/dmesg" 2>/dev/null || true
ln -s busybox "${WORK}/bin/hexdump" 2>/dev/null || true
ln -s busybox "${WORK}/bin/od" 2>/dev/null || true
ln -s busybox "${WORK}/bin/ps" 2>/dev/null || true
ln -s busybox "${WORK}/bin/grep" 2>/dev/null || true
ln -s busybox "${WORK}/bin/awk" 2>/dev/null || true
ln -s busybox "${WORK}/bin/sed" 2>/dev/null || true
ln -s busybox "${WORK}/bin/wc" 2>/dev/null || true
ln -s busybox "${WORK}/bin/cat" 2>/dev/null || true
ln -s busybox "${WORK}/bin/ls" 2>/dev/null || true

# Include PE test binary if present
if [ -f "/tmp/pe_min.exe" ]; then
    cp /tmp/pe_min.exe "${WORK}/pe_min.exe"
    chmod 755 "${WORK}/pe_min.exe"
    echo "Added /pe_min.exe from /tmp/pe_min.exe"
elif [ -f "${PROJECT_ROOT}/pe_min.exe" ]; then
    cp "${PROJECT_ROOT}/pe_min.exe" "${WORK}/pe_min.exe"
    chmod 755 "${WORK}/pe_min.exe"
    echo "Added /pe_min.exe from project root"
fi

# Include tests/kernel PE binaries (in-VM harness)
if [ -d "${PROJECT_ROOT}/tests/kernel/bin" ]; then
    mkdir -p "${WORK}/tests_kernel"
    for f in "${PROJECT_ROOT}/tests/kernel/bin"/*.exe; do
        [ -e "$f" ] || continue
        cp "$f" "${WORK}/tests_kernel/"
        chmod 755 "${WORK}/tests_kernel/$(basename "$f")"
        echo "Added /tests_kernel/$(basename "$f")"
    done
fi
# Also include minimal fallback PEs if built in /tmp
for f in /tmp/minimal*.exe /tmp/pe_min*.exe; do
    [ -e "$f" ] || continue
    mkdir -p "${WORK}/tests_kernel"
    cp "$f" "${WORK}/tests_kernel/" 2>/dev/null || true
    chmod 755 "${WORK}/tests_kernel/$(basename "$f")" 2>/dev/null || true
done

# Include ntdll/kernel32 and common import DLLs for differential probes
for dll in ntdll.dll kernel32.dll msvcrt.dll advapi32.dll user32.dll kernelbase.dll ucrtbase.dll; do
    if [ -f "/tmp/$dll" ]; then
        cp "/tmp/$dll" "${WORK}/$dll"
        chmod 755 "${WORK}/$dll"
        echo "Added /$dll from /tmp/$dll"
        mkdir -p "${WORK}/windows/system32" "${WORK}/tmp"
        cp "/tmp/$dll" "${WORK}/windows/system32/$dll"
        cp "/tmp/$dll" "${WORK}/tmp/$dll"
        echo "Added /windows/system32/$dll and /tmp/$dll"
    elif [ -f "${PROJECT_ROOT}/build/$dll" ]; then
        cp "${PROJECT_ROOT}/build/$dll" "${WORK}/$dll"
        chmod 755 "${WORK}/$dll"
        echo "Added /$dll from build/$dll"
        mkdir -p "${WORK}/windows/system32" "${WORK}/tmp"
        cp "${PROJECT_ROOT}/build/$dll" "${WORK}/windows/system32/$dll"
        cp "${PROJECT_ROOT}/build/$dll" "${WORK}/tmp/$dll"
    fi
done
# Also copy a dedicated import test PE if present
if [ -f "/tmp/minimal.exe" ]; then
    cp /tmp/minimal.exe "${WORK}/pe_test.exe"
    chmod 755 "${WORK}/pe_test.exe"
    echo "Added /pe_test.exe from /tmp/minimal.exe"
fi

# Include ELF twin for PMU (if built)
if [ -f "${PROJECT_ROOT}/tests/kernel/bin/pmu_elf" ]; then
    mkdir -p "${WORK}/tests_kernel"
    cp "${PROJECT_ROOT}/tests/kernel/bin/pmu_elf" "${WORK}/tests_kernel/pmu_elf"
    chmod 755 "${WORK}/tests_kernel/pmu_elf"
    echo "Added /tests_kernel/pmu_elf"
fi
for f in "${PROJECT_ROOT}/tests/kernel/bin"/*.elf "${PROJECT_ROOT}/tests/kernel/bin"/pmu_*; do
    [ -e "$f" ] || continue
    # already handled
    true
done

# Include perf binary for PMU measurement if available - disabled for DD3 to avoid cpio read error (65M initramfs)
# perf disabled to keep initramfs < 30M and avoid unpack failure
if false && command -v perf >/dev/null 2>&1; then
    PERF_BIN="$(command -v perf)"
    mkdir -p "${WORK}/usr/bin"
    cp -L "$PERF_BIN" "${WORK}/usr/bin/perf"
    chmod 755 "${WORK}/usr/bin/perf"
    echo "Added perf from $PERF_BIN"
    if command -v ldd >/dev/null 2>&1; then
        for lib in $(ldd "$PERF_BIN" 2>/dev/null | grep -oE '/[^ ]+' | sort -u); do
            [ -e "$lib" ] || continue
            mkdir -p "${WORK}$(dirname "$lib")"
            cp -L "$lib" "${WORK}${lib}" 2>/dev/null || true
            echo "Added lib $lib"
        done
        mkdir -p "${WORK}/lib64"
        if [ -e "/lib64/ld-linux-x86-64.so.2" ]; then
            cp -L "/lib64/ld-linux-x86-64.so.2" "${WORK}/lib64/ld-linux-x86-64.so.2" 2>/dev/null || true
        fi
        if [ -e "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" ]; then
            mkdir -p "${WORK}/lib/x86_64-linux-gnu"
            cp -L "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" "${WORK}/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" 2>/dev/null || true
        fi
    fi
    ln -sf busybox "${WORK}/bin/strings" 2>/dev/null || true
fi

# --- Identity stamping: embed source digest into initramfs ---
# Deterministic ordering: this embeds OVERALL into the cpio (WORK/ntpers.digest) atomically
# with the archive build, and also writes sidecars. When called from kbuild/rebuild_kdir,
# the authoritative freshness stamp happens ONCE after this script finishes (kbuild's single
# final NTPERS_ALLOW_WRITE=1 freshness write), which overwrites sidecars with the same value
# because the manifest excludes generated artifacts (build_digest.h etc) and is stable.
# Previously two freshness stamps (before and after mkinitramfs) caused embedded vs on-disk mismatch;
# now single final stamp guarantees they agree.
# Compute overall digest via freshness.sh if available
OVERALL="unknown"
if [ -x "$PROJECT_ROOT/vm/freshness.sh" ]; then
  TMPMAN=$(mktemp)
  "$PROJECT_ROOT/vm/freshness.sh" compute "$TMPMAN" >/dev/null 2>&1 || true
  OVERALL=$(sha256sum "$TMPMAN" 2>/dev/null | awk '{print $1}' || echo "unknown")
  rm -f "$TMPMAN"
elif [ -f "$PROJECT_ROOT/.ntpers_build.digest" ]; then
  OVERALL=$(cat "$PROJECT_ROOT/.ntpers_build.digest" 2>/dev/null | tr -d '[:space:]' || echo "unknown")
fi
GIT_COMMIT="unknown"
if command -v git >/dev/null 2>&1 && [ -d "$PROJECT_ROOT/.git" ]; then
  GIT_COMMIT=$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || echo "unknown")
fi
echo "$OVERALL" > "${WORK}/ntpers.digest"
cat > "${WORK}/ntpers.identity.json" <<EOF
{"overall_digest":"$OVERALL","git_commit":"$GIT_COMMIT","timestamp":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
EOF
echo "Stamped initramfs identity: digest $OVERALL commit $GIT_COMMIT"

# Build the cpio archive.  Use a reproducible order.
echo "Packaging initramfs -> ${OUT} ..."
(
    cd "${WORK}"
    find . -mindepth 1 -print0 | LC_ALL=C sort -z | \
        cpio --null -o --format=newc | gzip -9
) > "${OUT}"

SIZE="$(stat -c%s "${OUT}" 2>/dev/null || stat -f%z "${OUT}" 2>/dev/null)"
echo "Initramfs ready: ${OUT} (${SIZE} bytes)"
# Sidecar identity files for verification
echo "$OVERALL" > "${PROJECT_ROOT}/vm/initramfs.digest" 2>/dev/null || true
sha256sum "${OUT}" | awk '{print $1}' > "${PROJECT_ROOT}/vm/initramfs.sha256" 2>/dev/null || true
cat > "${PROJECT_ROOT}/vm/initramfs.identity.json" <<EOF2
{"overall_digest":"$OVERALL","initramfs_sha256":"$(cat "${PROJECT_ROOT}/vm/initramfs.sha256" 2>/dev/null)","git_commit":"$GIT_COMMIT","timestamp":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
EOF2
echo "Initramfs identity sidecars: ${PROJECT_ROOT}/vm/initramfs.digest + .sha256 + .identity.json"
