#!/usr/bin/env bash
# vm/incremental-kernel.sh - rebuild the NT personality after editing NT sources only.
#
# Use this after the kernel tree is built once (docs/vm/BUILD-RECIPE.md, section 8a).
# It does not re-run the config, the full tree build or the completeness gate.
#
# Which files change: every NT source whose content (CRLF removed, sha256) differs from
# the copy recorded in the guest manifest, GUEST_MANIFEST. The manifest records what the
# guest tree was last built from, so a file that was edited and then reverted is copied
# back, and a first run copies every NT source.
#
# Steps:
#   1. copy the changed NT sources (LF) into the guest checkout with tar over ssh (the
#      transport tools/linux-env/sync-repo.sh uses; the Windows host has no rsync);
#   2. in the guest, run tools/ntpers_kpatch.py on the kernel tree, which copies the
#      sources into arch/x86/nt and the other places the build reads (idempotent);
#   3. make the NT objects only: make arch/x86/nt/;
#   4. make bzImage, which relinks vmlinux and rebuilds the image;
#   5. only if 1-4 succeeded, write the new manifest to the guest.
#
# NT sources: src/kernel/nt/, src/kernel/fs/binfmt_pe.c, src/kernel/fs/kshim.[ch],
# src/loader/pe_text.[ch], every src/ntdll/*.h (kpatch copies them to arch/ntdll/,
# which binfmt_pe.c includes os_identity.h from) and arch/x86/nt/ (step 2 overwrites
# the arch copies of the files listed in src/kernel/nt).
#
# Usage:
#   vm/incremental-kernel.sh          copy the changed NT sources, then steps 1-5
#   vm/incremental-kernel.sh --all    copy every NT source, then steps 1-5
#   vm/incremental-kernel.sh --dry    print the file list and the steps, change nothing
#                                     (--all and --dry can be combined)
#
# Environment:
#   GUEST_SSH       ssh command (default: bash tools/linux-env/ssh-guest.sh)
#   GUEST_REPO      guest checkout (default: kfast/repo under the guest home)
#   GUEST_KDIR      guest kernel tree (default: kfast/linux-src under the guest home)
#   GUEST_MANIFEST  manifest file in the guest (default: kfast/nt-sync.manifest)
#   JOBS            make -j value (default 2; the guest is shared, keep it at 2)
#   CCACHE          "1" (default) builds with CC="ccache gcc"; "0" turns it off
#
# Exit: 0 on success, 1 when a step fails, 2 on bad usage.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GUEST_SSH="${GUEST_SSH:-bash $ROOT/tools/linux-env/ssh-guest.sh}"
GUEST_REPO="${GUEST_REPO:-kfast/repo}"
GUEST_KDIR="${GUEST_KDIR:-kfast/linux-src}"
GUEST_MANIFEST="${GUEST_MANIFEST:-kfast/nt-sync.manifest}"
JOBS="${JOBS:-2}"
CCACHE="${CCACHE:-1}"

NT_PATHS=(src/kernel/nt src/kernel/fs/binfmt_pe.c src/kernel/fs/kshim.c src/kernel/fs/kshim.h arch/x86/nt src/loader/pe_text.c src/loader/pe_text.h)
# Every ntdll header is staged, not only those a file names: kpatch copies all of them.
NTDLL_HEADERS=(src/ntdll/*.h)

MODE=changed
DRY=0
for arg in "$@"; do
    case "$arg" in
        --all) MODE=all ;;
        --dry) DRY=1 ;;
        *) echo "usage: vm/incremental-kernel.sh [--all] [--dry]" >&2; exit 2 ;;
    esac
done

log() { echo "incremental-kernel: $*"; }
die() { echo "incremental-kernel: ERROR: $*" >&2; exit 1; }

# 1. the manifest of the working tree: "sha256  path" per NT file (CRLF removed)
mapfile -t NT_FILES < <({ find "${NT_PATHS[@]}" -type f; printf '%s\n' "${NTDLL_HEADERS[@]}"; } | sort -u)
LOCAL_MANIFEST=""
for f in ${NT_FILES[@]+"${NT_FILES[@]}"}; do
    h="$(tr -d '\r' < "$f" | sha256sum | cut -d' ' -f1)"
    LOCAL_MANIFEST+="$h  $f"$'\n'
done

# 2. the manifest in the guest (empty when the guest has never been synced)
GUEST_TEXT="$($GUEST_SSH "cat ~/${GUEST_MANIFEST} 2>/dev/null || true")"
declare -A GUEST_HASH=()
while read -r h p; do
    [[ -n "${p:-}" ]] && GUEST_HASH["$p"]="$h"
done <<< "$GUEST_TEXT"

# 3. the changed set
FILES=()
while read -r h p; do
    [[ -n "${p:-}" ]] || continue
    if [[ "$MODE" == all || "${GUEST_HASH[$p]:-}" != "$h" ]]; then
        FILES+=("$p")
    fi
done <<< "$LOCAL_MANIFEST"

if [[ ${#FILES[@]} -eq 0 ]]; then
    log "guest tree is up to date with the NT sources (use --all to force a copy)"
    exit 0
fi

log "NT sources to copy: ${#FILES[@]} of ${#NT_FILES[@]} file(s)"
printf '  %s\n' "${FILES[@]}"

CC_ARG=""
[[ "$CCACHE" == 1 ]] && CC_ARG='CC="ccache gcc"'
STEPS=(
    "copy ${#FILES[@]} file(s) to guest:${GUEST_REPO}"
    "python3 tools/ntpers_kpatch.py --tree ~/${GUEST_KDIR} (in ~/${GUEST_REPO})"
    "make -j${JOBS} ARCH=x86_64 ${CC_ARG} arch/x86/nt/ (in ~/${GUEST_KDIR})"
    "make -j${JOBS} ARCH=x86_64 ${CC_ARG} bzImage (in ~/${GUEST_KDIR})"
    "write manifest guest:${GUEST_MANIFEST}"
)
if [[ "$DRY" == 1 ]]; then
    for s in "${STEPS[@]}"; do log "would run: $s"; done
    exit 0
fi

# 4. stage the sources (LF copies) and unpack them in the guest checkout
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
for f in "${FILES[@]}"; do
    mkdir -p "$STAGE/$(dirname "$f")"
    tr -d '\r' < "$f" > "$STAGE/$f"
done
tar -C "$STAGE" -cf - "${FILES[@]}" | $GUEST_SSH "mkdir -p ~/${GUEST_REPO} && tar -C ~/${GUEST_REPO} -xf -" \
    || die "copying sources to the guest failed"
log "copied ${#FILES[@]} file(s) to guest:${GUEST_REPO}"

# 5. kpatch, the NT objects, then the link. Each step stops the script on failure.
$GUEST_SSH "set -e
cd ~/${GUEST_REPO}
python3 tools/ntpers_kpatch.py --tree \$HOME/${GUEST_KDIR}
cd \$HOME/${GUEST_KDIR}
export CCACHE_DIR=\$HOME/kfast/ccache
[ '${CCACHE}' = 1 ] && command -v ccache >/dev/null && export CCACHE_MAXSIZE=4G
nice -n 10 make -j${JOBS} ARCH=x86_64 ${CC_ARG} arch/x86/nt/
nice -n 10 make -j${JOBS} ARCH=x86_64 ${CC_ARG} bzImage
echo INCREMENTAL_OK" || die "guest build failed (see the output above; the manifest is unchanged)"

# 6. the guest now matches the working tree
printf '%s' "$LOCAL_MANIFEST" | $GUEST_SSH "cat > ~/${GUEST_MANIFEST}" \
    || die "build succeeded but the manifest could not be written; the next run will copy again"

log "done: ~/${GUEST_KDIR}/arch/x86/boot/bzImage is up to date"
