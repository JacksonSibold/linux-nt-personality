#!/bin/bash
# vm/runvm.sh - boot the ntpers kernel under QEMU/KVM and run one guest test.
#
# Usage:
#   ./vm/runvm.sh --test <guest-script-path> [--share <host-dir>] \
#                 [--timeout <seconds>] [--log <serial.log>] \
#                 [--transport 9p|initrd-serial]
#
# The guest script is executed by /init inside the initramfs.  The guest
# writes its exit code to <share>/.dodresult/result.exitcode; this script
# reads it back and exits with the same code.  A missing result file means
# TIMEOUT or a guest-side crash.
#
# Transports (docs/vm/TRANSPORTS.md):
#   9p            (default) the share is a QEMU virtfs mount at /mnt/share.
#   initrd-serial the payload is packed into the initramfs at /mnt/share, and
#                 results come back framed on the serial log.  tools/vm_serial_collect.py
#                 rebuilds <share>/.dodresult/ from that log.  Needs no 9p and no KVM.
#
# Requirements:
#   - qemu-system-x86_64 with KVM support
#   - /dev/kvm accessible
#   - bzImage at ~/ntpers/linux-6.18/arch/x86/boot/bzImage
#   - initramfs at <project>/vm/initramfs.cpio.gz (built by mkinitramfs.sh)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BZIMAGE="${BZIMAGE:-${HOME}/ntpers/linux-6.18/arch/x86/boot/bzImage}"
INITRD="${INITRD:-${PROJECT_ROOT}/vm/initramfs.cpio.gz}"

TEST_SCRIPT="/mnt/share/vmtests/smoke.sh"
SHARE_DIR="${PROJECT_ROOT}"
TIMEOUT_SEC="${VM_TIMEOUT:-120}"
VM_TRANSPORT="${VM_TRANSPORT:-9p}"
PYTHON="${PYTHON:-python3}"
LOG_FILE=""
RUN_ID=""

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --test <path>      Guest test script path inside the 9p share (default: ${TEST_SCRIPT})
  --share <dir>      Host directory shared into the guest (default: project root)
  --timeout <secs>   Hard timeout in seconds (default: ${TIMEOUT_SEC})
  --log <file>       Serial console log file (default: auto-generated in share/.dodresult/)
  --run-id <id>      Per-run nonce for staleness protection (optional)
  --transport <t>    9p or initrd-serial (default: \$VM_TRANSPORT, currently ${VM_TRANSPORT})
  -h, --help         Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test)
            TEST_SCRIPT="$2"; shift 2 ;;
        --share)
            SHARE_DIR="$2"; shift 2 ;;
        --timeout)
            TIMEOUT_SEC="$2"; shift 2 ;;
        --log)
            LOG_FILE="$2"; shift 2 ;;
        --run-id)
            RUN_ID="$2"; shift 2 ;;
        --transport)
            VM_TRANSPORT="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "ERROR: unknown option $1" >&2; usage; exit 2 ;;
    esac
done

case "${VM_TRANSPORT}" in
    9p|initrd-serial) ;;
    *) echo "ERROR: VM_TRANSPORT must be 9p or initrd-serial (got ${VM_TRANSPORT})" >&2; exit 2 ;;
esac

if [[ ! -e "${BZIMAGE}" ]]; then
    echo "ERROR: kernel bzImage not found: ${BZIMAGE}" >&2
    exit 1
fi
if [[ ! -e "${INITRD}" ]]; then
    echo "ERROR: initramfs not found: ${INITRD}" >&2
    echo "Run: ./vm/mkinitramfs.sh" >&2
    exit 125
fi

# --- Freshness gate: refuse stale artifacts on ANY path (including verifier) ---
# No special trust for verifiers - this is how false refutation #22 happened.
if [[ -x "${PROJECT_ROOT}/vm/freshness.sh" ]]; then
  if ! "${PROJECT_ROOT}/vm/freshness.sh" check 2>&1; then
    echo "runvm: FRESHNESS GATE REFUSED - stale artifact detected, refusing to produce result" >&2
    echo "runvm: This applies to verifiers too (no bypass)" >&2
    exit 125
  fi
else
  echo "runvm: WARNING freshness.sh not found, skipping gate (should not happen)" >&2
fi

mkdir -p "${SHARE_DIR}/.dodresult"
RESULT_FILE="${SHARE_DIR}/.dodresult/result.exitcode"
rm -f "${RESULT_FILE}"
if [[ -n "${RUN_ID}" ]]; then
    echo "${RUN_ID}" > "${SHARE_DIR}/.dodresult/current_run_id"
    echo "runvm: run_id=${RUN_ID}"
fi

if [[ -z "${LOG_FILE}" ]]; then
    TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
    LOG_FILE="${SHARE_DIR}/.dodresult/serial-${TIMESTAMP}.log"
fi

echo "runvm: kernel=${BZIMAGE}"
echo "runvm: initrd=${INITRD}"
echo "runvm: test=${TEST_SCRIPT}"
echo "runvm: share=${SHARE_DIR}"
echo "runvm: timeout=${TIMEOUT_SEC}s"
echo "runvm: log=${LOG_FILE}"
echo "runvm: transport=${VM_TRANSPORT}"

# Kernel command line.  The initramfs /init reads dodtest= to pick the script.
# S2: SMAP enabled - all NT syscalls now use copy_from_user/copy_to_user
APPEND=(
    "console=ttyS0,115200n8"
    "earlyprintk=serial,ttyS0,115200n8"
    "nokaslr"
    "quiet"
    "panic=1"
    "dodtest=${TEST_SCRIPT}"
)
if [[ -n "${RUN_ID}" ]]; then
    APPEND+=("dodrunid=${RUN_ID}")
fi
if [[ "${VM_TRANSPORT}" == "initrd-serial" ]]; then
    APPEND+=("dodtransport=initrd-serial")
fi

# The 9p transport keeps its exact arguments.  initrd-serial has no virtfs, and it
# takes -cpu max when /dev/kvm is not usable (TCG-only hosts).
if [[ "${VM_TRANSPORT}" == "9p" ]]; then
    SHARE_ARGS=(-virtfs "local,path=${SHARE_DIR},mount_tag=share,security_model=none")
    CPU_ARGS=(-cpu host)
else
    SHARE_ARGS=()
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
        CPU_ARGS=(-cpu host)
    else
        CPU_ARGS=(-cpu max)
    fi
fi

QEMU_ARGS=(
    -accel kvm -accel tcg
    ${CPU_ARGS[@]+"${CPU_ARGS[@]}"}
    -m 4G
    -smp 4
    -nographic
    -kernel "${BZIMAGE}"
    -initrd "${INITRD}"
    -append "${APPEND[*]}"
    ${SHARE_ARGS[@]+"${SHARE_ARGS[@]}"}
    -serial "file:${LOG_FILE}"
    -device "virtio-rng-pci"
)

# Use GNU timeout for the hard wall-clock limit.
if ! command -v timeout >/dev/null 2>&1; then
    echo "ERROR: GNU timeout command not found." >&2
    exit 1
fi

set +e
"timeout" --foreground --signal=KILL "${TIMEOUT_SEC}s" \
    qemu-system-x86_64 "${QEMU_ARGS[@]}"
QEMU_RC=$?
set -e

if [[ "${VM_TRANSPORT}" == "initrd-serial" ]]; then
    # Rebuild <share>/.dodresult/ from the serial log.  Exit 3 means the guest did not
    # finish its emit (timeout, crash); it falls through to the TIMEOUT / ERROR paths below
    # exactly as a missing 9p result would.  Exit 1 or 2 is a transport failure, never a result.
    set +e
    "${PYTHON}" "${PROJECT_ROOT}/tools/vm_serial_collect.py" --log "${LOG_FILE}" --out "${SHARE_DIR}"
    COLLECT_RC=$?
    set -e
    if [[ "${COLLECT_RC}" -eq 1 || "${COLLECT_RC}" -eq 2 ]]; then
        echo "runvm: ERROR serial transport verification failed (collector rc ${COLLECT_RC}); log=${LOG_FILE}" >&2
        exit 1
    fi
fi

GUEST_RC=""
# Poll for 9p result file visibility with bounded timeout (fix 0.5s race -- S1 vm_execution false positive)
# 9p virtfs may delay host visibility by >0.5s; poll up to 10s with 0.2s interval
for _poll in $(seq 1 50); do
    if [[ -f "${RESULT_FILE}" ]]; then
        break
    fi
    sleep 0.2
done
if [[ -f "${RESULT_FILE}" ]]; then
    GUEST_RC="$(cat "${RESULT_FILE}" | tr -d '[:space:]')"
fi

if [[ -n "${GUEST_RC}" ]]; then
    echo "runvm: guest exited with code ${GUEST_RC}"
    exit "${GUEST_RC}"
fi

if [[ "${QEMU_RC}" -eq 124 || "${QEMU_RC}" -eq 137 ]]; then
    echo "runvm: TIMEOUT after ${TIMEOUT_SEC}s (result file never written)"
    exit 124
fi

echo "runvm: ERROR - no result file and QEMU exited with code ${QEMU_RC}"
exit 1
