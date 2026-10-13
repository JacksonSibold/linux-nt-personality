#!/bin/bash
# vm/guest-bootstrap.sh - build the NT personality kernel and run the M0 gate
# inside a Debian guest. Follows docs/vm/BUILD-RECIPE.md step by step; every
# step logs its name and stops the run on the first failure.
#
# Usage (inside the Debian guest, from the repository checkout):
#   KERNEL_SHA256=<64 lowercase hex> bash vm/guest-bootstrap.sh
#
# Environment:
#   KERNEL_SHA256  required. SHA256 of linux-6.18.tar.xz, taken from the
#                  kernel.org sha256sums.asc that was verified with gpg.
#                  The repository does not contain this value.
#   REPO           repository root (default: parent of this script)
#   WORKDIR        where the tarball and the tree live (default: $HOME/ntpers)
#   KERNEL_URL     tarball URL (default: kernel.org v6.x path for linux-6.18)
#   SKIP_APT=1     skip the apt-get step (packages already installed)
#   RUN_REG=1      also run the REG probe after M0 (needs reg_probe.exe)
#   VM_TIMEOUT     guest timeout in seconds for each gate (default: 90)
#
# Exit status: 0 when every step and the M0 gate pass; 1 otherwise.
#
# Step 4 applies the kernel-tree edits with tools/ntpers_kpatch.py. The patch
# files no longer apply (BUILD-RECIPE G2, G3), so the script replaces git apply.
# It stops when an anchor is missing or when the enter.c hooks are still a todo
# (G3), and writes nothing in that case. See docs/vm/BUILD-RECIPE.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
WORKDIR="${WORKDIR:-$HOME/ntpers}"
KERNEL_VERSION="6.18"
KERNEL_URL="${KERNEL_URL:-https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz}"
TARBALL="${WORKDIR}/linux-${KERNEL_VERSION}.tar.xz"
KDIR="${WORKDIR}/linux-${KERNEL_VERSION}"
SKIP_APT="${SKIP_APT:-0}"
RUN_REG="${RUN_REG:-0}"
VM_TIMEOUT="${VM_TIMEOUT:-90}"
LOG_DIR="${REPO}/.dodresult"

export KDIR

if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=()
else
    SUDO=(sudo)
fi

log() {
    printf '[guest-bootstrap %s] %s\n' "$(date -u +%H:%M:%S)" "$*"
}

step() {
    printf '\n[guest-bootstrap %s] ===== STEP %s =====\n' "$(date -u +%H:%M:%S)" "$*"
}

die() {
    printf '[guest-bootstrap %s] FAIL: %s\n' "$(date -u +%H:%M:%S)" "$*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

# ---------------------------------------------------------------------------
step "0: preflight"
log "REPO=${REPO}"
log "WORKDIR=${WORKDIR}"
log "KDIR=${KDIR}"
log "KERNEL_URL=${KERNEL_URL}"
[[ -n "${KERNEL_SHA256:-}" ]] || die "KERNEL_SHA256 is not set (see header)"
[[ "${KERNEL_SHA256}" =~ ^[0-9a-f]{64}$ ]] || die "KERNEL_SHA256 must be 64 lowercase hex characters"
[[ -f "${REPO}/vm/mkinitramfs.sh" && -f "${REPO}/vm/runvm.sh" ]] || die "REPO does not look like the ntpers checkout: ${REPO}"
[[ "$(uname -m)" == "x86_64" ]] || die "x86_64 guest required"
if [[ ! -r /dev/kvm ]]; then
    log "WARNING: /dev/kvm not readable; runvm.sh falls back to TCG, which is slow for the 90 s gate"
fi
log "preflight OK"

# ---------------------------------------------------------------------------
step "1: apt packages"
if [[ "${SKIP_APT}" == "1" ]]; then
    log "SKIP_APT=1, not running apt-get"
else
    export DEBIAN_FRONTEND=noninteractive
    "${SUDO[@]}" apt-get update
    "${SUDO[@]}" apt-get install -y --no-install-recommends \
        build-essential bc bison flex libelf-dev libssl-dev \
        git curl ca-certificates xz-utils cpio gzip binutils gawk \
        busybox-static libc6-dev \
        qemu-system-x86 \
        gcc-mingw-w64-x86-64 \
        python3
fi
for c in git curl tar xz cpio gzip sha256sum awk make gcc nm ar strip \
         busybox qemu-system-x86_64 x86_64-w64-mingw32-gcc flex bison bc; do
    require_cmd "${c}"
done
log "tools present"

# ---------------------------------------------------------------------------
step "2: fetch and verify the kernel tarball"
mkdir -p "${WORKDIR}" "${LOG_DIR}"
if [[ ! -f "${TARBALL}" ]]; then
    log "downloading ${KERNEL_URL}"
    curl -fL --retry 3 -o "${TARBALL}.part" "${KERNEL_URL}"
    mv "${TARBALL}.part" "${TARBALL}"
else
    log "tarball already present: ${TARBALL}"
fi
actual="$(sha256sum "${TARBALL}" | awk '{print $1}')"
if [[ "${actual}" != "${KERNEL_SHA256}" ]]; then
    die "SHA256 mismatch for ${TARBALL}: expected ${KERNEL_SHA256}, got ${actual}. Not extracting."
fi
log "SHA256 verified: ${actual}"

# ---------------------------------------------------------------------------
step "3: extract the tree"
if [[ -e "${KDIR}" ]]; then
    die "${KDIR} already exists. Remove it yourself, or set WORKDIR to a new directory. This script never deletes a tree."
fi
tar -xf "${TARBALL}" -C "${WORKDIR}"
[[ -f "${KDIR}/Makefile" ]] || die "tarball did not produce ${KDIR}"
log "extracted to ${KDIR}"

# ---------------------------------------------------------------------------
step "4: apply kernel-tree edits (tools/ntpers_kpatch.py; replaces git apply)"
# The patch files do not apply (BUILD-RECIPE G2, G3); src/kernel/patches/README.md
# marks them deprecated. The script copies the NT sources, wires Kconfig, Kbuild
# and fs, and makes the header and syscall edits, all as anchored edits. It writes
# nothing when any edit is blocked or still todo. The enter.c todo (G3) makes this
# step stop until that gap is decided; pass --allow-todo only on purpose.
python3 "${REPO}/tools/ntpers_kpatch.py" --project "${REPO}" --tree "${KDIR}" \
    | tee "${LOG_DIR}/kpatch-apply.log" \
    || die "ntpers_kpatch.py did not apply (see ${LOG_DIR}/kpatch-apply.log; BUILD-RECIPE G3, G4, G5)"
log "kernel-tree edits applied"

# ---------------------------------------------------------------------------
step "5: verify the NT wiring symbols (written by step 4)"
grep -qF 'TIF_NTABI' "${KDIR}/arch/x86/include/asm/thread_info.h" || die "TIF_NTABI not defined (step 4 did not apply)"
grep -qF 'TIF_NTSUSPEND' "${KDIR}/arch/x86/include/asm/thread_info.h" || die "TIF_NTSUSPEND not defined"
grep -qF 'do_nt_syscall' "${KDIR}/arch/x86/entry/syscall_64.c" || die "syscall_64.c has no NT dispatch"
grep -qF 'nt_thread' "${KDIR}/include/linux/sched.h" || die "task_struct has no nt_thread"
grep -qF 'nt_banner_initcall' "${KDIR}/arch/x86/nt/kernel_stubs.c" || die "nt_banner_init has no caller (BUILD-RECIPE G10)"
log "NT wiring present"

# ---------------------------------------------------------------------------
step "6: kernel .config (vm/make-kernel-config.sh)"
KDIR="${KDIR}" bash "${REPO}/vm/make-kernel-config.sh" "${LOG_DIR}/kernel-config.copy"

# ---------------------------------------------------------------------------
step "7: build bzImage (vmlinux is a by-product) and completeness check"
cd "${KDIR}"
make olddefconfig
make prepare -j1 || { rm -f scripts/basic/fixdep; make prepare -j1; }
if ! make -j"$(nproc)" bzImage 2>&1 | tee "${LOG_DIR}/kbuild-bzImage.log"; then
    die "make bzImage failed (log ${LOG_DIR}/kbuild-bzImage.log)"
fi
[[ -f arch/x86/boot/bzImage ]] || die "bzImage not produced"
[[ -f vmlinux ]] || die "vmlinux not produced"
bash "${REPO}/tests/kernel/check_kernel_complete.sh" "${KDIR}/vmlinux" \
    || die "completeness check failed (kernel incomplete)"
log "bzImage: ${KDIR}/arch/x86/boot/bzImage"

# ---------------------------------------------------------------------------
step "8: PE test binaries (tests/kernel/bin, mingw-w64)"
make -C "${REPO}/tests/kernel"
[[ -f "${REPO}/tests/kernel/bin/minimal.exe" ]] || die "tests/kernel/bin/minimal.exe not built"

# ---------------------------------------------------------------------------
step "9: initramfs (vm/mkinitramfs.sh)"
bash "${REPO}/vm/mkinitramfs.sh" "${REPO}/vm/initramfs.cpio.gz"
[[ -f "${REPO}/vm/initramfs.cpio.gz" ]] || die "initramfs not produced"

# ---------------------------------------------------------------------------
step "10: freshness stamp (must come before the gate, see BUILD-RECIPE G13)"
NTPERS_ALLOW_WRITE=1 KDIR="${KDIR}" bash "${REPO}/vm/freshness.sh" write \
    || die "freshness write failed"
bash "${REPO}/vm/freshness.sh" check || die "freshness check failed right after stamping"

# ---------------------------------------------------------------------------
# run_gate NAME GUEST_SCRIPT EXPECTED
# Runs one guest test through runvm.sh without the initramfs rebuild that
# check_m0.sh and check_reg.sh perform (that rebuild would not change the
# stamped sources, but it would change the initramfs the stamp describes).
run_guest() {
    local name="$1" guest="$2"
    local serial="${LOG_DIR}/${name}-serial.log"
    rm -f "${LOG_DIR}/result.exitcode" "${LOG_DIR}/test_${name}.json"
    set +e
    BZIMAGE="${KDIR}/arch/x86/boot/bzImage" \
    INITRD="${REPO}/vm/initramfs.cpio.gz" \
        bash "${REPO}/vm/runvm.sh" --test "${guest}" --share "${REPO}" \
            --timeout "${VM_TIMEOUT}" --log "${serial}"
    local rc=$?
    set -e
    log "runvm exit ${rc}; serial log ${serial}"
    [[ -f "${LOG_DIR}/result.exitcode" ]] || die "${name}: no result file (runvm rc ${rc})"
}

step "11: M0 gate (minimal.exe must exit 42)"
run_guest m0 /mnt/share/vm/test_m0.sh
json="${LOG_DIR}/test_m0.json"
[[ -f "${json}" ]] || die "M0: test_m0.json missing"
cat "${json}"
if grep -q '"status": "PASS"' "${json}" && grep -Eq '"exit_code": 42,?$' "${json}"; then
    log "M0 gate PASS: minimal.exe exited 42"
else
    die "M0 gate FAIL (see ${LOG_DIR}/m0-serial.log)"
fi

if [[ "${RUN_REG}" == "1" ]]; then
    step "12: REG probe (optional)"
    run_guest reg /mnt/share/vm/test_reg.sh
    if [[ "$(tr -d '[:space:]' < "${LOG_DIR}/result.exitcode")" == "0" ]]; then
        log "REG gate PASS"
    else
        die "REG gate FAIL (see ${LOG_DIR}/reg-serial.log)"
    fi
fi

log "=== guest-bootstrap: PASS ==="
