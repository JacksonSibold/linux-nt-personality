#!/bin/bash
# tools/linux-env/run-vm.sh - bash equivalent of run-vm.ps1 (Linux, or WSL, with qemu-system-x86_64).
#
# Boots the kernel with -kernel/-initrd, with a wall-clock timeout, captures serial to a file, and
# rebuilds the guest results with tools/vm_serial_collect.py under --out. Same contract and exit codes
# as run-vm.ps1. It is a boot-and-collect runner, not a verifier: the freshness gate is not run here.
# Use vm/runvm.sh for gate-bearing results.
#
# Usage:
#   tools/linux-env/run-vm.sh --initrd <initramfs.cpio.gz> --out <dir> \
#       [--kernel <bzImage>] [--test /mnt/share/vm/test_m0.sh] [--timeout 240] [--mem 1024] [--cpus 1] \
#       [--qemu qemu-system-x86_64] [--python python3]
#
# Exit: the guest's result.exitcode when the serial emit verifies; 124 when the guest did not finish
# its emit (timeout or crash); 1 on a transport verification failure; 126 when QEMU was refused twice;
# 2 on usage errors.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
KERNEL="${BZIMAGE:-${HOME}/ntpers/linux-6.18/arch/x86/boot/bzImage}"
INITRD=""
OUT=""
TEST="/mnt/share/vm/test_m0.sh"
TIMEOUT_SEC=240
MEM=1024
CPUS=1
QEMU="${QEMU:-qemu-system-x86_64}"
PYTHON="${PYTHON:-python3}"

usage() {
    # Print the leading comment block (everything up to the first non-comment line).
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --initrd) INITRD="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --test) TEST="$2"; shift 2 ;;
        --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
        --mem) MEM="$2"; shift 2 ;;
        --cpus) CPUS="$2"; shift 2 ;;
        --qemu) QEMU="$2"; shift 2 ;;
        --python) PYTHON="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "run-vm: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "${INITRD}" || -z "${OUT}" ]]; then
    echo "run-vm: --initrd and --out are required" >&2
    usage >&2
    exit 2
fi
for p in "${KERNEL}" "${INITRD}" "${ROOT}/tools/vm_serial_collect.py"; do
    if [[ ! -e "${p}" ]]; then
        echo "run-vm: missing ${p}" >&2
        exit 2
    fi
done
if ! command -v "${QEMU}" >/dev/null 2>&1; then
    echo "run-vm: QEMU not found: ${QEMU}" >&2
    exit 2
fi
if ! command -v timeout >/dev/null 2>&1; then
    echo "run-vm: GNU timeout not found" >&2
    exit 1
fi

mkdir -p "${OUT}"
RESULT_FILE="${OUT}/.dodresult/result.exitcode"
RECORD="${OUT}/run-vm-record.txt"
rm -f "${RESULT_FILE}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="${OUT}/serial-${STAMP}.log"
APPEND="console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1 dodtransport=initrd-serial dodtest=${TEST}"

if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    ACCEL=(-accel kvm -accel tcg -cpu host)
else
    ACCEL=(-accel tcg -cpu max)
fi

QEMU_ARGS=(
    -name ntpers-serial
    "${ACCEL[@]}"
    -machine q35
    -m "${MEM}"
    -smp "${CPUS}"
    -kernel "${KERNEL}"
    -initrd "${INITRD}"
    -append "${APPEND}"
    -display none
    -monitor none
    -nic none
    -no-reboot
    -serial "file:${LOG}"
)

echo "run-vm: kernel=${KERNEL}"
echo "run-vm: initrd=${INITRD}"
echo "run-vm: test=${TEST}"
echo "run-vm: timeout=${TIMEOUT_SEC}s"
echo "run-vm: log=${LOG}"

run_qemu() {
    rm -f "${LOG}"
    set +e
    timeout --foreground --signal=KILL "${TIMEOUT_SEC}s" "${QEMU}" "${QEMU_ARGS[@]}"
    QEMU_RC=$?
    set -e
    TIMED_OUT=0
    if [[ "${QEMU_RC}" -eq 124 || "${QEMU_RC}" -eq 137 ]]; then
        TIMED_OUT=1
    fi
}

ATTEMPTS=1
run_qemu
if [[ "${QEMU_RC}" -eq 126 ]]; then
    echo "run-vm: attempt 1 refused (rc=126); retrying once" >&2
    echo "${STAMP} attempt 1 refused (rc=126); retrying once" >> "${RECORD}"
    ATTEMPTS=2
    run_qemu
fi
echo "${STAMP} attempt ${ATTEMPTS} qemu rc=${QEMU_RC} timed_out=${TIMED_OUT}" >> "${RECORD}"
echo "run-vm: qemu attempt ${ATTEMPTS} rc=${QEMU_RC} timed_out=${TIMED_OUT}"
if [[ "${QEMU_RC}" -eq 126 && "${TIMED_OUT}" -eq 0 ]]; then
    echo "run-vm: QEMU refused to start twice (policy). Not bypassed." >&2
    exit 126
fi

set +e
"${PYTHON}" "${ROOT}/tools/vm_serial_collect.py" --log "${LOG}" --out "${OUT}"
COLLECT_RC=$?
set -e
echo "run-vm: collector rc=${COLLECT_RC}"

if [[ "${COLLECT_RC}" -eq 0 && -f "${RESULT_FILE}" ]]; then
    GUEST_RC="$(tr -d '[:space:]' < "${RESULT_FILE}")"
    echo "run-vm: guest exited with code ${GUEST_RC}"
    exit "${GUEST_RC}"
fi
if [[ "${COLLECT_RC}" -eq 3 || "${TIMED_OUT}" -eq 1 ]]; then
    echo "run-vm: TIMEOUT or incomplete emit after ${TIMEOUT_SEC}s (no result files)" >&2
    tail -n 40 "${LOG}" >&2 || true
    exit 124
fi
echo "run-vm: ERROR serial transport verification failed (collector rc ${COLLECT_RC}); see ${LOG}" >&2
exit 1
