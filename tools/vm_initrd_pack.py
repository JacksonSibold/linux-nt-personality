#!/usr/bin/env python3
"""
vm_initrd_pack.py - build a gzip-compressed newc initramfs with the standard library only.

This is the packer for hosts without cpio (the Windows host, where the VM is run by
tools/linux-env/run-vm.ps1). On Linux, vm/mkinitramfs.sh builds the same kind of
archive with cpio. The output is deterministic: entries are sorted, timestamps are 0,
and gzip is written without a name or time.

It starts from an optional base archive (passed through unchanged, so a base such as
the M0 initramfs keeps its busybox and /tests_kernel/minimal.exe), then adds trees and
symlinks. A name added later replaces the same name from the base or an earlier add.

  --add SRC=DEST   SRC is a file or a directory on the host. DEST is the path inside the
                   archive, relative to its root. A directory is added recursively, and
                   its symlinks are kept as symlinks. Files get mode 0755.
                   A .sh file has CRLF converted to LF, because the guest sh cannot run
                   a script with carriage returns.
  --link NAME=TGT  add a symlink NAME -> TGT (TGT is stored verbatim).
  --max-kb N       refuse to write when the uncompressed total exceeds N KiB
                   (default 32768). The kernel unpacks the whole archive into RAM.

Usage:
  python3 tools/vm_initrd_pack.py --base BASE.cpio.gz --out OUT.cpio.gz \
      --add tools/linux-env/init-serial.sh=init --add vm=mnt/share/vm \
      --link bin/base64=busybox

Exit status: 0 written, 1 size cap or missing source, 2 usage or read error.
"""
import argparse
import gzip
import io
import os
import pathlib
import stat
import sys
from typing import Dict, List, Optional, Tuple

MAGIC = b"070701"
HEADER_LEN = 110
TRAILER = "TRAILER!!!"
DEFAULT_MAX_KB = 32768


class Entry:
    __slots__ = ("mode", "data")

    def __init__(self, mode: int, data: bytes):
        self.mode = mode
        self.data = data


def _align4(n: int) -> int:
    return (n + 3) & ~3


def read_newc(blob: bytes) -> Dict[str, Entry]:
    """Parse a newc archive (uncompressed bytes) into name -> Entry, in archive order."""
    entries: Dict[str, Entry] = {}
    offset = 0
    while True:
        if offset + HEADER_LEN > len(blob):
            raise ValueError("truncated newc header at offset %d" % offset)
        header = blob[offset:offset + HEADER_LEN]
        if header[:6] != MAGIC:
            raise ValueError("not a newc archive at offset %d" % offset)
        fields = [int(header[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        mode, filesize, namesize = fields[1], fields[6], fields[11]
        name_start = offset + HEADER_LEN
        name = blob[name_start:name_start + namesize - 1].decode("utf-8")
        data_start = _align4(name_start + namesize)
        data = blob[data_start:data_start + filesize]
        offset = _align4(data_start + filesize)
        if name == TRAILER:
            return entries
        entries[name] = Entry(mode, data)


def load_base(path: Optional[pathlib.Path]) -> Dict[str, Entry]:
    if path is None:
        return {}
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return read_newc(raw)


def _normal_name(dest: str) -> str:
    name = dest.replace("\\", "/").strip("/")
    if not name or any(part in ("", ".", "..") for part in name.split("/")):
        raise ValueError("bad archive path %r" % dest)
    return name


def _parents(name: str) -> List[str]:
    parts = name.split("/")
    return ["/".join(parts[:i]) for i in range(1, len(parts))]


def add_tree(entries: Dict[str, Entry], src: pathlib.Path, dest: str, log) -> None:
    """Add a file, symlink or directory tree from the host under dest."""
    dest = _normal_name(dest)
    if src.is_symlink():
        put_link(entries, dest, os.readlink(src), log)
        return
    if src.is_file():
        put_file(entries, dest, src, log)
        return
    if not src.is_dir():
        raise FileNotFoundError("missing source %s" % src)
    put_dir(entries, dest, log)
    for root, dirs, files in os.walk(src):
        dirs.sort()
        files.sort()
        root_path = pathlib.Path(root)
        for name in dirs + files:
            host = root_path / name
            rel = host.relative_to(src).as_posix()
            target = dest + "/" + rel
            if host.is_symlink():
                put_link(entries, target, os.readlink(host), log)
            elif host.is_dir():
                put_dir(entries, target, log)
            elif host.is_file():
                put_file(entries, target, host, log)


def _put(entries: Dict[str, Entry], name: str, entry: Entry, log) -> None:
    for parent in _parents(name):
        if parent not in entries:
            entries[parent] = Entry(stat.S_IFDIR | 0o755, b"")
    if name in entries:
        log("replaced %s" % name)
    entries[name] = entry


def put_dir(entries: Dict[str, Entry], name: str, log) -> None:
    if name not in entries or not stat.S_ISDIR(entries[name].mode):
        _put(entries, name, Entry(stat.S_IFDIR | 0o755, b""), log)


def put_link(entries: Dict[str, Entry], name: str, target: str, log) -> None:
    _put(entries, name, Entry(stat.S_IFLNK | 0o777, target.encode("utf-8")), log)


def put_file(entries: Dict[str, Entry], name: str, host: pathlib.Path, log) -> None:
    data = host.read_bytes()
    if host.suffix == ".sh":
        data = data.replace(b"\r\n", b"\n")
    _put(entries, name, Entry(stat.S_IFREG | 0o755, data), log)


def _newc_entry(name: str, entry: Entry, ino: int) -> bytes:
    name_bytes = name.encode("utf-8") + b"\0"
    fields = [
        ino, entry.mode, 0, 0, 1, 0, len(entry.data),
        0, 0, 0, 0, len(name_bytes), 0,
    ]
    header = MAGIC + b"".join(b"%08x" % f for f in fields)
    out = bytearray(header + name_bytes)
    out += b"\0" * (_align4(len(out)) - len(out))
    out += entry.data
    out += b"\0" * (_align4(len(out)) - len(out))
    return bytes(out)


def write_newc(entries: Dict[str, Entry]) -> bytes:
    out = bytearray()
    for ino, name in enumerate(sorted(entries), 1):
        out += _newc_entry(name, entries[name], ino)
    out += _newc_entry(TRAILER, Entry(0, b""), 0)
    return bytes(out)


def payload_bytes(entries: Dict[str, Entry]) -> int:
    return sum(len(e.data) for e in entries.values())


def build(base: Optional[pathlib.Path], adds: List[Tuple[pathlib.Path, str]],
          links: List[Tuple[str, str]], max_kb: int, log=print) -> Tuple[Dict[str, Entry], int]:
    entries = load_base(base)
    base_count = len(entries)
    for src, dest in adds:
        add_tree(entries, src, dest, log)
    for name, target in links:
        put_link(entries, _normal_name(name), target, log)
    total_kb = (payload_bytes(entries) + 1023) // 1024
    if total_kb > max_kb:
        raise OverflowError("uncompressed initramfs %d KiB exceeds cap %d KiB (--max-kb)"
                            % (total_kb, max_kb))
    return entries, base_count


def _split_pair(text: str, what: str) -> Tuple[str, str]:
    if "=" not in text:
        raise argparse.ArgumentTypeError("%s must be FROM=TO, got %r" % (what, text))
    left, right = text.split("=", 1)
    if not left or not right:
        raise argparse.ArgumentTypeError("%s has an empty side: %r" % (what, text))
    return left, right


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Build a newc initramfs without cpio.")
    parser.add_argument("--base", type=pathlib.Path, default=None,
                        help="base archive (.cpio or .cpio.gz) passed through unchanged")
    parser.add_argument("--out", required=True, type=pathlib.Path, help="output .cpio.gz")
    parser.add_argument("--add", action="append", default=[], metavar="SRC=DEST",
                        type=lambda t: _split_pair(t, "--add"))
    parser.add_argument("--link", action="append", default=[], metavar="NAME=TARGET",
                        type=lambda t: _split_pair(t, "--link"))
    parser.add_argument("--max-kb", type=int, default=DEFAULT_MAX_KB,
                        help="uncompressed size cap in KiB (default: %(default)s)")
    args = parser.parse_args(argv)
    if not args.add and not args.link and args.base is None:
        print("vm_initrd_pack: nothing to pack (give --base, --add or --link)", file=sys.stderr)
        return 2
    adds = [(pathlib.Path(s), d) for s, d in args.add]
    try:
        entries, base_count = build(args.base, adds, args.link, args.max_kb,
                                    log=lambda m: print("vm_initrd_pack: " + m))
    except (FileNotFoundError, OverflowError) as exc:
        # Missing source or payload over the cap: the archive is not written.
        print("vm_initrd_pack: %s" % exc, file=sys.stderr)
        return 1
    except (OSError, ValueError) as exc:
        # Unreadable or malformed base archive, or a bad archive path.
        print("vm_initrd_pack: %s" % exc, file=sys.stderr)
        return 2
    newc = write_newc(entries)
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, mtime=0, compresslevel=9) as gz:
        gz.write(newc)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(buf.getvalue())
    print("vm_initrd_pack: %d entries (%d from base), payload %d KiB, uncompressed %d bytes, "
          "wrote %s (%d bytes)"
          % (len(entries), base_count, (payload_bytes(entries) + 1023) // 1024, len(newc),
             args.out, len(buf.getvalue())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
