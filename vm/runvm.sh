#!/bin/bash
# vm/runvm.sh - boot the ntpers kernel under QEMU/KVM and run one guest test.
#
# Usage:
#   ./vm/runvm.sh --test <guest-script-path> [--share <host-dir>] \
#                 [--timeout <seconds>] [--log <serial.log>]
#
# The guest script is executed by /init inside the initramfs.  The guest
# writes its exit code to <share>/.dodresult/result.exitcode; this script
# reads it back and exits with the same code.  A missing result file means
# TIMEOUT or a guest-side crash.
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
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "ERROR: unknown option $1" >&2; usage; exit 2 ;;
    esac
done

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

QEMU_ARGS=(
    -accel kvm -accel tcg
    -cpu host
    -m 4G
    -smp 4
    -nographic
    -kernel "${BZIMAGE}"
    -initrd "${INITRD}"
    -append "${APPEND[*]}"
    -virtfs "local,path=${SHARE_DIR},mount_tag=share,security_model=none"
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
