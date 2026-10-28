#!/bin/sh
# tools/linux-env/probes-initramfs.sh - build the differential probes for the NT personality and an
# initramfs whose /init runs each probe in turn, framed by serial markers, then powers off.
#
# Two modes:
#   default (VM):    the *_direct probes with an NTPERS_VM branch, built with the VM stdout shim
#                    (tools/linux-env/ntpers_vm_stdout.c). Freestanding, no DLLs in the image.
#   NONVM=1:         the probes with no NTPERS_VM branch (the 15 non-direct probes), built with the
#                    default CRT. The image carries the PE DLLs they import under /windows/system32:
#                    ntdll.dll, kernel32.dll, msvcrt.dll, advapi32.dll, user32.dll, win32u.dll and gdi32.dll from src/ntdll/out (build
#                    with `make -C src/ntdll all kernel32 msvcrt advapi32 win32u user32 gdi32`, serially). A probe whose
#                    imports are not in the image fails the load (STATUS_DLL_NOT_FOUND, S5).
#
# Runs inside the guest (Debian 12), needs x86_64-w64-mingw32-gcc, busybox-static (with the timeout
# applet), cpio and gzip.
#
# Probe stdout is the NT console handle -11 (see tools/linux-env/ntpers_vm_stdout.c). The kernel
# prints it on the serial console. Each probe is framed on the console by:
#   NTPERS_PROBE_BEGIN <probe>
#   ...probe stdout (printk lines, prefixed "nt console[-11]: ")...
#   NTPERS_PROBE_RC <probe> <exit status>
#   NTPERS_PROBE_END <probe>
# tools/linux-env/probe_results.py split turns the serial log into one JSON file per probe.
#
# Usage: [NONVM=1] sh probes-initramfs.sh REPO_DIR OUT_CPIO_GZ [PROBE ...]
#   PROBE names are without .c (default: every probes/*_direct.c with an NTPERS_VM branch in VM mode,
#   every other probes/probe_*.c that is not freestanding in NONVM mode).
#   Per-probe argv and stdin come from tools/linux-env/probes-argv.manifest (PROBES_MANIFEST overrides
#   it). probe_peloader runs as "--batch" with its path list on stdin, as capture_golden.py runs it.
set -eu

REPO="${1:?usage: probes-initramfs.sh REPO_DIR OUT_CPIO_GZ [PROBE...]}"
OUT="${2:?usage: probes-initramfs.sh REPO_DIR OUT_CPIO_GZ [PROBE...]}"
shift 2
PROBES_SRC="$REPO/tests/differential/probes"
SHIM="$REPO/tools/linux-env/ntpers_vm_stdout.c"
CC="${CC:-x86_64-w64-mingw32-gcc}"
BUSYBOX="${BUSYBOX:-/bin/busybox}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-900}"
NONVM="${NONVM:-0}"
NTDLL_OUT="$REPO/src/ntdll/out"
# DLLs packed for NONVM mode: ntdll and the Win32 shims. advapi32 is the registry shim; user32 has wsprintf.
SYSTEM_DLLS="ntdll kernel32 msvcrt advapi32 user32 win32u gdi32"

if [ "$NONVM" = 1 ]; then
    echo "probes-initramfs: NONVM mode (system DLLs from $NTDLL_OUT)" >&2
else
    [ -f "$SHIM" ] || { echo "probes-initramfs: missing $SHIM" >&2; exit 1; }
fi
[ -x "$BUSYBOX" ] || { echo "probes-initramfs: missing $BUSYBOX (install busybox-static)" >&2; exit 1; }
command -v "$CC" >/dev/null 2>&1 || { echo "probes-initramfs: missing $CC" >&2; exit 1; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/proc" "$W/sys" "$W/dev" "$W/tests_kernel" "$W/build" "$W/tmp"

# Probe list: explicit arguments, else the mode's default set.
if [ $# -gt 0 ]; then
    LIST="$*"
elif [ "$NONVM" = 1 ]; then
    LIST=""
    for f in "$PROBES_SRC"/probe_*.c; do
        case "$f" in *_direct.c) continue ;; esac
        # A VM branch is a conditional on NTPERS_VM or VM_BUILD; a comment that names them is not one.
        grep -qE "^[[:space:]]*#[[:space:]]*if.*(NTPERS_VM|VM_BUILD)" "$f" && continue
        # A freestanding probe (its own _start, raw syscalls) does not use ntdll or the CRT.
        if grep -qE "void _start\(" "$f" 2>/dev/null; then
            echo "probes-initramfs: skip $(basename "$f") (freestanding _start)" >&2
            continue
        fi
        LIST="$LIST $(basename "$f" .c)"
    done
else
    LIST=""
    for f in "$PROBES_SRC"/*_direct.c; do
        grep -qE "NTPERS_VM|VM_BUILD" "$f" || continue
        LIST="$LIST $(basename "$f" .c)"
    done
fi
[ -n "$LIST" ] || { echo "probes-initramfs: no probes" >&2; exit 1; }

# Per-probe argv and stdin (tools/linux-env/probes-argv.manifest, or PROBES_MANIFEST). A line is
# "probe | argv words | stdin file". Prints "argv|stdin" for probe $1, nothing if it has no line.
MANIFEST="${PROBES_MANIFEST:-$REPO/tools/linux-env/probes-argv.manifest}"
manifest_entry() {
    [ -f "$MANIFEST" ] || return 0
    awk -F'|' -v p="$1" '
        /^[ \t]*#/ || NF == 0 { next }
        { n = $1; gsub(/^[ \t]+|[ \t]+$/, "", n)
          if (n == p) {
              a = $2; s = $3
              gsub(/^[ \t]+|[ \t]+$/, "", a); gsub(/^[ \t]+|[ \t]+$/, "", s)
              print a "|" s
              exit
          } }' "$MANIFEST"
}

if [ "$NONVM" = 1 ]; then
    for d in $SYSTEM_DLLS; do
        [ -f "$NTDLL_OUT/$d.dll" ] || { echo "probes-initramfs: missing $NTDLL_OUT/$d.dll" >&2; exit 1; }
    done
    mkdir -p "$W/windows/system32"
    for d in $SYSTEM_DLLS; do
        cp -L "$NTDLL_OUT/$d.dll" "$W/windows/system32/$d.dll"
        chmod 755 "$W/windows/system32/$d.dll"
    done
    for P in $LIST; do
        SRC="$PROBES_SRC/$P.c"
        [ -f "$SRC" ] || { echo "probes-initramfs: missing $SRC" >&2; exit 1; }
        "$CC" -w -Os -o "$W/tests_kernel/$P.exe" "$SRC"
        chmod 755 "$W/tests_kernel/$P.exe"
        echo "probes-initramfs: built $P.exe ($(wc -c < "$W/tests_kernel/$P.exe") bytes)" >&2
    done
else
    # Compile the shim once.
    "$CC" -c -Os -ffreestanding -fno-builtin -fno-stack-protector -o "$W/build/ntpers_vm_stdout.o" "$SHIM"

    for P in $LIST; do
        SRC="$PROBES_SRC/$P.c"
        [ -f "$SRC" ] || { echo "probes-initramfs: missing $SRC" >&2; exit 1; }
        "$CC" -DNTPERS_VM -DVM_BUILD \
            -Dlinux_open=ntpers_vm_open -Dlinux_write=ntpers_vm_write -Dlinux_close=ntpers_vm_close \
            -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp \
            -Os -ffreestanding -fno-builtin -fno-stack-protector -w \
            -o "$W/tests_kernel/$P.exe" "$SRC" "$W/build/ntpers_vm_stdout.o"
        chmod 755 "$W/tests_kernel/$P.exe"
        echo "probes-initramfs: built $P.exe ($(wc -c < "$W/tests_kernel/$P.exe") bytes)" >&2
    done
fi

# Stdin files named by the manifest go into the image next to the probe.
for P in $LIST; do
    ENTRY="$(manifest_entry "$P")"
    STDIN_SRC="${ENTRY#*|}"
    if [ -n "$STDIN_SRC" ]; then
        [ -f "$REPO/$STDIN_SRC" ] || { echo "probes-initramfs: missing stdin $REPO/$STDIN_SRC for $P" >&2; exit 1; }
        cp "$REPO/$STDIN_SRC" "$W/tests_kernel/$P.stdin"
        chmod 644 "$W/tests_kernel/$P.stdin"
    fi
done

cp -L "$BUSYBOX" "$W/bin/busybox"
chmod 755 "$W/bin/busybox"
for applet in sh mount echo sleep timeout; do
    ln -s busybox "$W/bin/$applet"
done

{
    echo '#!/bin/sh'
    echo 'mount -t proc proc /proc'
    echo 'mount -t sysfs sysfs /sys'
    # The NT path C:\tmp maps to /tmp in the personality (src/kernel/nt/io.c with g_kshim_root "/"),
    # and the probes create their scratch files there. Windows has C:\tmp; this provides it.
    echo 'mount -t tmpfs tmpfs /tmp'
    # Markers go through /dev/kmsg so they share the printk sequence with the probe's console
    # output (the probe's stdout is printk'd asynchronously; a plain echo can overtake it).
    echo 'mknod /dev/kmsg c 1 11 2>/dev/null'
    echo 'mark() { echo "$1" > /dev/kmsg 2>/dev/null || echo "$1"; }'
    if [ "$NONVM" = 1 ]; then
        echo 'mark "=== NT non-VM probes initramfs init (system DLLs in /windows/system32) ==="'
    else
        echo 'mark "=== NT probes initramfs init (no 9p share) ==="'
    fi
    for P in $LIST; do
        ENTRY="$(manifest_entry "$P")"
        PARGS="${ENTRY%%|*}"
        REDIR=""
        if [ -f "$W/tests_kernel/$P.stdin" ]; then REDIR="< /tests_kernel/$P.stdin"; fi
        echo "mark \"NTPERS_PROBE_BEGIN $P\""
        echo "timeout $PROBE_TIMEOUT /tests_kernel/$P.exe $PARGS $REDIR"
        echo "RC=\$?"
        echo "mark \"NTPERS_PROBE_RC $P \$RC\""
        echo "mark \"NTPERS_PROBE_END $P\""
    done
    echo 'mark "NTPERS_ALL_DONE"'
    echo 'echo o > /proc/sysrq-trigger'
    echo 'sleep 5'
} > "$W/init"
chmod 755 "$W/init"

# Fixed mtime-free archive: newc cpio, gzip without the name field (-n). The NONVM tree is root-owned
# (binfmt_pe's ntdll search refuses a file that is not root-owned, K-5).
rm -rf "$W/build"
if [ "$NONVM" = 1 ]; then
    (cd "$W" && find . | LC_ALL=C sort | cpio -o -H newc --owner 0:0 --quiet | gzip -9 -n) > "$OUT"
else
    (cd "$W" && find . -path ./build -prune -o -print | LC_ALL=C sort | cpio -o -H newc --quiet | gzip -9 -n) > "$OUT"
fi
echo "probes-initramfs: wrote $OUT ($(wc -c < "$OUT") bytes) mode:$([ "$NONVM" = 1 ] && echo nonvm || echo vm) probes:$LIST" >&2
