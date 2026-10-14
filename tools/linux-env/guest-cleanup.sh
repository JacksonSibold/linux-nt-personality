#!/bin/sh
# guest-cleanup.sh - reclaim disk in the dev guest by deleting reproducible
# build intermediates from finished private build trees.
#
# usage: guest-cleanup.sh [--apply] [--home DIR]
#
#   default      dry run: prints what would be removed per directory and the
#                total. Deletes nothing.
#   --apply      delete the listed files. Refused while any process has its
#                working directory or an open file inside a target directory.
#   --home DIR   use DIR as the home directory (default: $HOME). Used by the
#                unit test; the guest uses its own home.
#
# Deleted: object and kbuild intermediates only (*.o, *.ko, *.a, *.mod,
# *.mod.c, *.cmd, .*.d, *.o.d, .tmp_*). Under diffwork/<tree>/ the build/ and
# obj/ directories are removed too, minus the protected names below.
#
# Never deleted, whatever the path: bzImage, .config and *.config, initramfs
# and cpio images (*.cpio, *.cpio.gz, *initramfs*), disk images (*.img,
# *.qcow2), keys and certificates (id_*, *.pem, *.key), vmlinux and .git dirs.
#
# Only the directories in ALLOW_DIRS are visited, so other agents' trees
# (k32a, main-proof*, loader, kfast, kgaps, integ5, ntpers, ...) are never
# touched. ~/k32a is excluded on purpose (agent a131f40edf71bb003 uses it).
# The kernel trees whose object files total under 2 GB are excluded too.
# ~/syncfix/linux-src is kept whole (KEEP_TREES): it is the built kernel tree for
# the incremental rebuild proof, and its objects are needed for that.

set -u

APPLY=0
HOME_DIR="${HOME:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --apply) APPLY=1 ;;
        --home) shift; HOME_DIR="${1:-}" ;;
        --home=*) HOME_DIR="${1#--home=}" ;;
        -h|--help)
            sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "guest-cleanup: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ -z "$HOME_DIR" ] || [ ! -d "$HOME_DIR" ]; then
    echo "guest-cleanup: home directory not set or missing: '$HOME_DIR'" >&2
    exit 2
fi
cd "$HOME_DIR" || exit 2

# Top-level directories (relative to home) whose intermediates may be removed.
ALLOW_DIRS="prtest prfix pub5 osid pebfix syncfix diffwork"

# Each list function prints "<size>\t<path>" lines, one per file.

# Built trees kept whole: their objects drive an incremental kernel rebuild
# (the mutcap sync.c proof), so deleting them would force a full rebuild.
KEEP_TREES="syncfix/linux-src"

# List object and kbuild intermediates under $1, excluding protected names.
list_objects() {
    find "$1" \( -name .git -o -path "$KEEP_TREES" -o -path "$KEEP_TREES/*" \) -prune -o -type f \
        \( -name '*.o' -o -name '*.ko' -o -name '*.a' -o -name '*.mod' \
           -o -name '*.mod.c' -o -name '*.cmd' -o -name '.*.d' -o -name '*.o.d' \
           -o -name '.tmp_*' \) \
        ! \( -name bzImage -o -name '.config' -o -name '.config.*' -o -name '*.config' \
           -o -name '*.cpio' -o -name '*.cpio.gz' -o -name '*initramfs*' \
           -o -name '*.img' -o -name '*.qcow2' -o -name '*.pem' -o -name '*.key' \
           -o -name 'id_*' -o -name vmlinux -o -name 'vmlinux.*' \) \
        -printf '%s\t%p\n' 2>/dev/null
}

# List every non-protected file under build/ or obj/ directory $1.
list_build_dir() {
    find "$1" \( -name .git -prune \) -o -type f \
        ! \( -name bzImage -o -name '.config' -o -name '.config.*' -o -name '*.config' \
           -o -name '*.cpio' -o -name '*.cpio.gz' -o -name '*initramfs*' \
           -o -name '*.img' -o -name '*.qcow2' -o -name '*.pem' -o -name '*.key' \
           -o -name 'id_*' -o -name vmlinux -o -name 'vmlinux.*' \) \
        -printf '%s\t%p\n' 2>/dev/null
}

# Print the diffwork build/ and obj/ directories, one per line.
diffwork_build_dirs() {
    for tree in diffwork/*/; do
        [ -d "$tree" ] || continue
        for sub in build obj; do
            [ -d "${tree}${sub}" ] && printf '%s\n' "${tree}${sub}"
        done
    done
}

# Print the directories to scan, one per line.
scan_roots() {
    for d in $ALLOW_DIRS; do
        case "$d" in
            diffwork)
                for tree in diffwork/*/; do [ -d "$tree" ] && printf '%s\n' "${tree%/}"; done ;;
            *) [ -d "$d" ] && printf '%s\n' "$d" ;;
        esac
    done
}

# Succeed (print the offender) if a live process has directory $1 as its
# working directory or has a file under it open. One find over /proc.
in_use() {
    target="$1"
    case "$target" in /*) ;; *) target="$HOME_DIR/$target" ;; esac
    hit=$(find /proc -maxdepth 3 \( -path '/proc/[0-9]*/cwd' -o -path '/proc/[0-9]*/fd/*' \) \
            \( -lname "$target" -o -lname "$target/*" \) 2>/dev/null | head -n 1)
    [ -n "$hit" ] || return 1
    echo "$hit"
    return 0
}

LIST=$(mktemp "${TMPDIR:-/tmp}/guest-cleanup.XXXXXX") || exit 2
trap 'rm -f "$LIST"' EXIT

# LIST holds "<size>\t<path>" for every file to remove, de-duplicated.
{
    scan_roots | while IFS= read -r root; do list_objects "$root"; done
    diffwork_build_dirs | while IFS= read -r bd; do list_build_dir "$bd"; done
} | sort -u >"$LIST"

echo "guest-cleanup: home=$HOME_DIR mode=$([ "$APPLY" -eq 1 ] && echo apply || echo dry-run)"
if [ ! -s "$LIST" ]; then
    echo "guest-cleanup: nothing to remove"
    exit 0
fi

# Per-directory totals: a file is charged to its first path component, or to
# diffwork/<tree> for files under diffwork.
awk -F'\t' '
    {
        p = $2; sub(/^\.\//, "", p)
        n2 = split(p, parts, "/")
        top = (parts[1] == "diffwork" && n2 > 2) ? parts[1] "/" parts[2] : parts[1]
        s[top] += $1; t += $1; n++
    }
    END {
        for (k in s) printf "  %-26s %10.1f MB\n", k, s[k] / 1048576
        printf "  %-26s %10.1f MB in %d files\n", "TOTAL", t / 1048576, n
    }' "$LIST"

if [ "$APPLY" -eq 0 ]; then
    echo "guest-cleanup: dry run, nothing deleted (rerun with --apply)"
    exit 0
fi

busy=0
for root in $(scan_roots) $(diffwork_build_dirs); do
    if u=$(in_use "$root"); then echo "guest-cleanup: in use: $u" >&2; busy=1; fi
done
if [ "$busy" -ne 0 ]; then
    echo "guest-cleanup: refusing to delete while a target is in use" >&2
    exit 3
fi

cut -f2- "$LIST" | xargs -d '\n' rm -f --
diffwork_build_dirs | while IFS= read -r bd; do rmdir -- "$bd" 2>/dev/null; done
echo "guest-cleanup: removed $(wc -l <"$LIST" | tr -d ' ') files"
exit 0
