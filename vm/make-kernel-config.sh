#!/bin/bash
# vm/make-kernel-config.sh - write the NT personality .config into a prepared
# Linux 6.18 tree (KDIR), or check an existing .config against the profile.
#
# Precondition (write mode): the tree has the NT wiring from docs/vm/BUILD-RECIPE.md
# step 5 (arch/x86/Kconfig.nt exists and arch/x86/Kconfig sources it). Without that the
# NT symbols do not exist, and scripts/config would drop them silently.
#
# Profiles (PROFILE, default "min"):
#   min   the options the NT personality and the host-boot initramfs path need
#         (M0 and the console steps S1-S4, tools/linux-env/boot-m0-host.ps1). No
#         networking, no block layer, no virtio, no 9p, no modules, no BPF, no perf,
#         no debug info. This is the profile for compile-time work; see
#         docs/vm/BUILD-RECIPE.md section 6a.
#   full  the earlier configuration: the same base plus the QEMU 9p share transport
#         (VIRTIO, VIRTIO_PCI, VIRTIO_BLK, NET_9P, 9P_FS, which needs NET and BLOCK).
#         Needed only by vm/runvm.sh, which mounts the repository through 9p.
#
# Method (write mode): x86_64 tinyconfig (BASE, default tinyconfig), then
# scripts/config --disable for the profile's disable list and --enable for its
# required list, then olddefconfig. olddefconfig fills every unset symbol with its
# default, so a default-y subsystem (BLOCK, BPF, PERF_EVENTS) comes back unless it is
# set to "is not set" first; the disable list does that. The result is checked the
# by check_config: every required symbol must be =y and no forbidden symbol may be =y
# or =m, because olddefconfig silently drops options whose dependencies are unmet.
#
# Usage:
#   KDIR=$HOME/ntpers/linux-6.18 ./vm/make-kernel-config.sh [copy-to-file]
#   PROFILE=full KDIR=... ./vm/make-kernel-config.sh
#   ./vm/make-kernel-config.sh --check [config-file]   # default: $KDIR/.config
#   PROFILE=full ./vm/make-kernel-config.sh --check some.config
#
# The optional argument (write mode) is a path to which a copy of the final .config
# is written. The script does not write vm/kernel-config-ntpers; that file is
# missing from the repository (see BUILD-RECIPE.md, Known gaps).
#
# Exit: 0 PASS, 1 a required option is missing or a forbidden one is enabled.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KDIR="${KDIR:-$HOME/ntpers/linux-6.18}"
PROFILE="${PROFILE:-min}"
SCRIPTS_CONFIG="${KDIR}/scripts/config"

MODE=write
COPY_TO=""
CHECK_FILE=""
if [[ "${1:-}" == "--check" ]]; then
    MODE=check
    CHECK_FILE="${2:-}"
elif [[ $# -gt 1 ]]; then
    echo "usage: make-kernel-config.sh [copy-to-file] | --check [config-file]" >&2
    exit 2
else
    COPY_TO="${1:-}"
fi

CONFIG_FILE="${KDIR}/.config"
[[ "${MODE}" == check && -n "${CHECK_FILE}" ]] && CONFIG_FILE="${CHECK_FILE}"

die() {
    echo "make-kernel-config: ERROR: $*" >&2
    exit 1
}

# Options. Names are given without the CONFIG_ prefix.
#
# Shared by both profiles:
#   64BIT                             tinyconfig on x86 comes out 32-bit (CONFIG_X86_32=y);
#                                     X86_64 (and so NT_PERSONALITY, BINFMT_PE) needs 64BIT
#   NT_PERSONALITY, BINFMT_PE         the NT ABI and the PE loader (added by the tree wiring)
#   PRINTK, TTY                       off in tinyconfig; the console needs both
#   SERIAL_8250, SERIAL_8250_CONSOLE  console=ttyS0 on the QEMU command line
#   EARLY_PRINTK                      earlyprintk=serial,ttyS0 on the command line
#   BLK_DEV_INITRD, RD_GZIP           the initramfs is a gzip-compressed cpio
#   BINFMT_ELF                        busybox and init are static ELF binaries
#   BINFMT_SCRIPT                     #! scripts (the M0 initramfs /init is a shell script);
#                                     off in tinyconfig, so exec returned ENOEXEC (-8)
#   SHMEM, TMPFS                      /tmp is a tmpfs (step-initramfs.sh, S3)
#   PROC_FS, SYSFS                    mounted by the initramfs /init
#   DEVTMPFS, DEVTMPFS_MOUNT          /dev (and /dev/console) populated before /init runs
#   MAGIC_SYSRQ                       the initramfs /init powers off through /proc/sysrq-trigger
#   SECCOMP                           the seccomp ABI that the kpatch edits touch (syscall_64.c);
#                                     def_bool, depends only on HAVE_ARCH_SECCOMP
#   DEBUG_FS                          debugfs_tests.c and the guest tests use /sys/kernel/debug
#   DEBUG_INFO_NONE                   no debug info (tinyconfig default; checked explicitly)
#
# AUDIT is not in the shared list: in 6.18 it depends on NET (init/Kconfig), and the
# kpatch audit edits only add uapi constants (AUDIT_ARCH_NTX86_64), which build without
# CONFIG_AUDIT. Full profile only.
#
# Full profile only (the 9p transport for vm/runvm.sh):
#   VIRTIO_MENU, VIRTIO, VIRTIO_PCI, VIRTIO_BLK   QEMU devices used by runvm.sh
#   PCI                               prerequisite of VIRTIO_PCI
#   NET, BLOCK                        prerequisites of NET_9P and VIRTIO_BLK
#   NET_9P, NET_9P_VIRTIO, 9P_FS      the "share" mount in init.c
#   AUDIT                             needs NET; kept for the earlier layout
#   DYNAMIC_DEBUG                     used by the guest tests in the earlier layout
REQUIRED_SHARED=(
    64BIT
    PRINTK
    TTY
    SERIAL_8250
    SERIAL_8250_CONSOLE
    EARLY_PRINTK
    BLK_DEV_INITRD
    RD_GZIP
    BINFMT_ELF
    BINFMT_SCRIPT
    NT_PERSONALITY
    BINFMT_PE
    SHMEM
    TMPFS
    PROC_FS
    SYSFS
    DEVTMPFS
    DEVTMPFS_MOUNT
    MAGIC_SYSRQ
    SECCOMP
    DEBUG_FS
    DEBUG_INFO_NONE
)

REQUIRED_FULL_EXTRA=(
    VIRTIO_MENU
    VIRTIO
    VIRTIO_PCI
    VIRTIO_BLK
    PCI
    NET
    BLOCK
    NET_9P
    NET_9P_VIRTIO
    9P_FS
    AUDIT
    DYNAMIC_DEBUG
)

# Min profile: options olddefconfig would otherwise enable by default (set to "is not
# set" before olddefconfig runs). PERF_EVENTS is not here: arch/x86/Kconfig selects it
# unconditionally, so it stays on for x86_64 (see docs/vm/BUILD-RECIPE.md section 6a).
#   NET, BLOCK, PCI, VIRTIO*, NET_9P*, 9P_FS  the transports and the networking stack
#   BPF, BPF_SYSCALL                          BPF is default y and NET selects it
#   MODULES                                   nothing is loaded at run time
#   VT, INPUT, SERIO                          the console is the serial port; VT selects INPUT
#   UNIX98_PTYS                               no pty users in the boot path
#   CPU_SUP_HYGON/CENTAUR/ZHAOXIN            vendors the QEMU guest never uses (CPU_SUP_INTEL and
#                                             CPU_SUP_AMD stay). Their prompts only show with
#                                             PROCESSOR_SELECT=y (REQUIRED_MIN_EXTRA)
#   KERNEL_XZ, KERNEL_ZSTD, ...               boot compressors other than gzip (KERNEL_GZIP is required)
#   RD_XZ, RD_LZMA, RD_BZIP2, RD_LZO, RD_LZ4, RD_ZSTD  initramfs decompressors; RD_GZIP stays
# MICROCODE is not listed: it is def_bool, on whenever CPU_SUP_INTEL or CPU_SUP_AMD is on.
MIN_DISABLE=(
    NET
    BLOCK
    PCI
    VIRTIO
    VIRTIO_PCI
    VIRTIO_BLK
    VIRTIO_MENU
    NET_9P
    NET_9P_VIRTIO
    9P_FS
    BPF
    BPF_SYSCALL
    DYNAMIC_DEBUG
    MODULES
    VT
    INPUT
    SERIO
    UNIX98_PTYS
    CPU_SUP_HYGON
    CPU_SUP_CENTAUR
    CPU_SUP_ZHAOXIN
    KERNEL_XZ
    KERNEL_BZIP2
    KERNEL_LZMA
    KERNEL_LZO
    KERNEL_LZ4
    KERNEL_ZSTD
    RD_XZ
    RD_LZMA
    RD_BZIP2
    RD_LZO
    RD_LZ4
    RD_ZSTD
)

# Min profile: options that must be =y in addition to the shared list. KERNEL_GZIP
# replaces the default XZ boot compressor (lib/xz is then not built). PROCESSOR_SELECT
# shows the vendor prompts (it is off in tinyconfig, which would make CPU_SUP_* ignore
# "is not set").
REQUIRED_MIN_EXTRA=(
    KERNEL_GZIP
    PROCESSOR_SELECT
)

# Min profile: a check failure if any of these is =y or =m. Every MIN_DISABLE symbol is
# checked, so an option that olddefconfig turns back on through a select is caught.
MIN_FORBIDDEN=(
    "${MIN_DISABLE[@]}"
    INET
    IPV6
    WIRELESS
    AUDIT
)

case "${PROFILE}" in
    min)
        REQUIRED=("${REQUIRED_SHARED[@]}" "${REQUIRED_MIN_EXTRA[@]}")
        DISABLE=("${MIN_DISABLE[@]}")
        FORBIDDEN=("${MIN_FORBIDDEN[@]}")
        ;;
    full)
        REQUIRED=("${REQUIRED_SHARED[@]}" "${REQUIRED_FULL_EXTRA[@]}")
        DISABLE=()
        FORBIDDEN=()
        ;;
    *) die "PROFILE must be min or full (got '${PROFILE}')" ;;
esac

# check_config FILE: every REQUIRED symbol is =y, no FORBIDDEN symbol is =y or =m, and in
# the min profile no debug-info option other than DEBUG_INFO_NONE is enabled.
check_config() {
    local file="$1" bad=0 sym
    echo "Checking ${file} against profile '${PROFILE}' ..."
    for sym in "${REQUIRED[@]}"; do
        if grep -qx "CONFIG_${sym}=y" "${file}"; then
            echo "OK: CONFIG_${sym}=y"
        else
            echo "MISSING: CONFIG_${sym}=y (found: $(grep -E "^(# )?CONFIG_${sym}[ =]" "${file}" || echo 'absent'))" >&2
            bad=1
        fi
    done
    for sym in ${FORBIDDEN[@]+"${FORBIDDEN[@]}"}; do
        if grep -qE "^CONFIG_${sym}=[ym]$" "${file}"; then
            echo "FORBIDDEN: CONFIG_${sym} is enabled in profile '${PROFILE}'" >&2
            bad=1
        fi
    done
    if [[ "${PROFILE}" == min ]]; then
        local dbg
        dbg="$(grep -E '^CONFIG_DEBUG_INFO[A-Z0-9_]*=y$' "${file}" | grep -v '^CONFIG_DEBUG_INFO_NONE=y$' || true)"
        if [[ -n "${dbg}" ]]; then
            echo "FORBIDDEN: debug info enabled in profile 'min': ${dbg}" >&2
            bad=1
        fi
    fi
    return "${bad}"
}

if [[ "${MODE}" == check ]]; then
    echo "=== make-kernel-config --check ==="
    [[ -f "${CONFIG_FILE}" ]] || die "no config file ${CONFIG_FILE}"
    if check_config "${CONFIG_FILE}"; then
        echo "=== make-kernel-config: CHECK PASS (${PROFILE}) ==="
        exit 0
    fi
    die "config ${CONFIG_FILE} fails profile '${PROFILE}'"
fi

echo "=== make-kernel-config ==="
echo "PROFILE=${PROFILE} KDIR=${KDIR}"
echo "PROJECT_ROOT=${PROJECT_ROOT}"

[[ -f "${KDIR}/Makefile" ]] || die "no kernel tree at ${KDIR} (run BUILD-RECIPE steps 1-4 first)"
[[ -f "${SCRIPTS_CONFIG}" ]] || die "missing ${SCRIPTS_CONFIG} (tarball incomplete?)"
[[ -f "${KDIR}/arch/x86/Kconfig.nt" ]] || die "arch/x86/Kconfig.nt missing (BUILD-RECIPE step 5)"
grep -qF 'source "arch/x86/Kconfig.nt"' "${KDIR}/arch/x86/Kconfig" \
    || die "arch/x86/Kconfig does not source arch/x86/Kconfig.nt (BUILD-RECIPE step 5)"

BASE="${BASE:-tinyconfig}"
echo "Starting from x86_64 ${BASE} ..."
make -C "${KDIR}" ARCH=x86_64 "${BASE}"

set_args=(--disable X86_32)
for sym in ${DISABLE[@]+"${DISABLE[@]}"}; do
    set_args+=(--disable "${sym}")
done
for sym in "${REQUIRED[@]}"; do
    set_args+=(--enable "${sym}")
done
echo "Setting options with scripts/config: ${#REQUIRED[@]} required, ${#DISABLE[@]} disabled ..."
bash "${SCRIPTS_CONFIG}" --file "${CONFIG_FILE}" "${set_args[@]}"

echo "Resolving dependencies with olddefconfig ..."
make -C "${KDIR}" ARCH=x86_64 olddefconfig

if ! check_config "${CONFIG_FILE}"; then
    die "profile '${PROFILE}' is not satisfied after olddefconfig"
fi

if [[ -n "${COPY_TO}" ]]; then
    cp "${CONFIG_FILE}" "${COPY_TO}"
    echo "copied .config to ${COPY_TO}"
fi
echo "enabled options: $(grep -c '=[ym]$' "${CONFIG_FILE}")"
echo "=== make-kernel-config: PASS (${PROFILE}) ==="
