#!/bin/bash
# vm/make-kernel-config.sh - write the NT personality .config into a prepared
# Linux 6.18 tree (KDIR).
#
# Precondition: the tree has the NT wiring from docs/vm/BUILD-RECIPE.md step 5
# (arch/x86/Kconfig.nt exists and arch/x86/Kconfig sources it). Without that the
# NT symbols do not exist, and scripts/config would drop them silently.
#
# Method: x86_64 tinyconfig (BASE, default tinyconfig; BASE=defconfig for the
# larger config), then scripts/config --enable for the option list
# below, then olddefconfig. The result is accepted only if every listed symbol
# is "=y" after olddefconfig, because olddefconfig silently drops options whose
# dependencies are not met.
#
# Usage:
#   KDIR=$HOME/ntpers/linux-6.18 ./vm/make-kernel-config.sh [copy-to-file]
#
# The optional argument is a path to which a copy of the final .config is
# written. The script does not write vm/kernel-config-ntpers; that file is
# missing from the repository (see BUILD-RECIPE.md, Known gaps).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KDIR="${KDIR:-$HOME/ntpers/linux-6.18}"
COPY_TO="${1:-}"
CONFIG_FILE="${KDIR}/.config"
SCRIPTS_CONFIG="${KDIR}/scripts/config"

die() {
    echo "make-kernel-config: ERROR: $*" >&2
    exit 1
}

echo "=== make-kernel-config ==="
echo "KDIR=${KDIR}"
echo "PROJECT_ROOT=${PROJECT_ROOT}"

[[ -f "${KDIR}/Makefile" ]] || die "no kernel tree at ${KDIR} (run BUILD-RECIPE steps 1-4 first)"
[[ -f "${SCRIPTS_CONFIG}" ]] || die "missing ${SCRIPTS_CONFIG} (tarball incomplete?)"
[[ -f "${KDIR}/arch/x86/Kconfig.nt" ]] || die "arch/x86/Kconfig.nt missing (BUILD-RECIPE step 5)"
grep -qF 'source "arch/x86/Kconfig.nt"' "${KDIR}/arch/x86/Kconfig" \
    || die "arch/x86/Kconfig does not source arch/x86/Kconfig.nt (BUILD-RECIPE step 5)"

# Options. Names are given without the CONFIG_ prefix.
#   NT_PERSONALITY, BINFMT_PE         the NT ABI and the PE loader (added by the tree wiring)
#   VIRTIO, VIRTIO_PCI, VIRTIO_BLK    transport for the QEMU devices (runvm.sh)
#   NET_9P, NET_9P_VIRTIO, 9P_FS      the host share mounted by init.c (tag "share")
#   DEVTMPFS, DEVTMPFS_MOUNT          /dev populated by init.c
#   SERIAL_8250, SERIAL_8250_CONSOLE  console=ttyS0 on the runvm.sh command line
#   EARLY_PRINTK                      earlyprintk=serial,ttyS0 on the command line
#   BLK_DEV_INITRD, RD_GZIP           the initramfs is a gzip-compressed cpio
#   BINFMT_ELF                        busybox and init are static ELF binaries
#   BINFMT_SCRIPT                     #! scripts (the M0 initramfs /init is a shell script);
#                                     off in tinyconfig, so exec returned ENOEXEC (-8)
#   PCI                               prerequisite of VIRTIO_PCI
#   TMPFS, PROC_FS, SYSFS             mounted by init.c
#   MAGIC_SYSRQ                       init.c powers off through /proc/sysrq-trigger
#   SECCOMP, AUDIT                    the seccomp/audit ABI the NT patches touch
#   DEBUG_FS, DYNAMIC_DEBUG           /sys/kernel/debug used by init.c and the guest tests
#   NET, BLOCK                        prerequisites of NET_9P and VIRTIO_BLK (tinyconfig has neither)
#   64BIT                             tinyconfig on x86 comes out 32-bit (CONFIG_X86_32=y);
#                                     X86_64 (and so NT_PERSONALITY, BINFMT_PE) needs 64BIT
#   PRINTK, TTY, SHMEM, VIRTIO_MENU   off in tinyconfig; the console, 8250 serial, tmpfs and
#                                     virtio drivers depend on them. VIRTIO itself has no prompt
#                                     and is selected by VIRTIO_PCI
REQUIRED=(
    64BIT
    PRINTK
    TTY
    SHMEM
    VIRTIO_MENU
    NET
    BLOCK
    NT_PERSONALITY
    BINFMT_PE
    VIRTIO
    VIRTIO_PCI
    VIRTIO_BLK
    NET_9P
    NET_9P_VIRTIO
    9P_FS
    DEVTMPFS
    DEVTMPFS_MOUNT
    SERIAL_8250
    SERIAL_8250_CONSOLE
    EARLY_PRINTK
    BLK_DEV_INITRD
    RD_GZIP
    BINFMT_ELF
    BINFMT_SCRIPT
    PCI
    TMPFS
    PROC_FS
    SYSFS
    MAGIC_SYSRQ
    SECCOMP
    AUDIT
    DEBUG_FS
    DYNAMIC_DEBUG
)

BASE="${BASE:-tinyconfig}"
echo "Starting from x86_64 ${BASE} ..."
make -C "${KDIR}" ARCH=x86_64 "${BASE}"

enable_args=()
for sym in "${REQUIRED[@]}"; do
    enable_args+=(--enable "${sym}")
done
echo "Enabling ${#REQUIRED[@]} options with scripts/config ..."
bash "${SCRIPTS_CONFIG}" --file "${CONFIG_FILE}" --disable X86_32 "${enable_args[@]}"

echo "Resolving dependencies with olddefconfig ..."
make -C "${KDIR}" ARCH=x86_64 olddefconfig

missing=0
for sym in "${REQUIRED[@]}"; do
    if grep -qx "CONFIG_${sym}=y" "${CONFIG_FILE}"; then
        echo "OK: CONFIG_${sym}=y"
    else
        echo "MISSING: CONFIG_${sym}=y (after olddefconfig: $(grep -E "^(# )?CONFIG_${sym}[ =]" "${CONFIG_FILE}" || echo 'absent'))" >&2
        missing=1
    fi
done
[[ ${missing} -eq 0 ]] || die "one or more required options are not =y"

if [[ -n "${COPY_TO}" ]]; then
    cp "${CONFIG_FILE}" "${COPY_TO}"
    echo "copied .config to ${COPY_TO}"
fi
echo "=== make-kernel-config: PASS ==="
