#!/usr/bin/env python3
"""
vm_serial_collect.py - rebuild guest result files from a serial console log.

Host side of the initrd-serial VM transport. The guest side is vm/emit-results.sh.
The stream it produces is:

    @@NTPERS-EMIT-BEGIN@@
    @@NTPERS-FILE <path> <size> <sha256>@@
    <base64 body, any number of lines>
    @@END@@
    ...
    @@NTPERS-EMIT-END@@

Rules, all enforced:
  - A frame is written only when its byte count, its sha256 and its base64 body
    all agree with the header. Any disagreement is an error.
  - Kernel log lines and other console text are ignored outside frames. Inside a
    frame, a line that is not made only of base64 characters is noise: it is
    skipped and counted. A noise line that happens to be base64 characters changes
    the body, so the sha256 check fails. That is reported, never accepted.
  - A header that starts before the previous frame ended truncates that frame (error).
  - The same path may appear twice only with the same size and sha256. Otherwise
    it is an error.
  - Paths must be relative, must not contain '..', empty or '.' components, and must
    lie under the prefix (default ".dodresult/").
  - Nothing is written when there is any error, and nothing is written when the
    emit did not complete. A partial result must not look like a result.

Usage:
  python3 tools/vm_serial_collect.py --log SERIAL.log --out SHARE_DIR [--prefix .dodresult/]

Exit status:
  0  complete emit, every frame verified, files written under --out
  1  one or more frames failed verification (nothing written)
  2  usage or I/O error
  3  the emit did not complete (no @@NTPERS-EMIT-END@@): guest did not finish or VM was killed
"""
import argparse
import base64
import binascii
import dataclasses
import hashlib
import os
import pathlib
import re
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

EMIT_BEGIN = "@@NTPERS-EMIT-BEGIN@@"
EMIT_END = "@@NTPERS-EMIT-END@@"
FRAME_END = "@@END@@"
HEADER_RE = re.compile(r"@@NTPERS-FILE (\S+) (\d+) ([0-9a-f]{64})@@")
BODY_RE = re.compile(r"[A-Za-z0-9+/=]+\Z")
DEFAULT_PREFIX = ".dodresult/"

EXIT_OK = 0
EXIT_VERIFY = 1
EXIT_USAGE = 2
EXIT_INCOMPLETE = 3


@dataclasses.dataclass
class Frame:
    path: str
    size: int
    sha256: str
    line: int
    body: List[str] = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class Collected:
    files: Dict[str, bytes] = dataclasses.field(default_factory=dict)
    errors: List[str] = dataclasses.field(default_factory=list)
    noise_lines: int = 0
    duplicates: int = 0
    frames: int = 0
    complete: bool = False


def check_path(path: str, prefix: str) -> Optional[str]:
    """Return an error message for an unsafe or out-of-prefix path, else None."""
    if path.startswith("/") or "\\" in path or ":" in path:
        return "absolute or non-POSIX path"
    parts = path.split("/")
    if any(p in ("", ".", "..") for p in parts):
        return "empty, '.' or '..' component"
    if not path.startswith(prefix):
        return "outside the result prefix %r" % prefix
    return None


def _finish(frame: Frame, prefix: str, out: Collected) -> None:
    where = "line %d %s" % (frame.line, frame.path)
    problem = check_path(frame.path, prefix)
    if problem:
        out.errors.append("%s: unsafe path (%s)" % (where, problem))
        return
    body = "".join(frame.body)
    try:
        data = base64.b64decode(body, validate=True)
    except (binascii.Error, ValueError) as exc:
        out.errors.append("%s: base64 body does not decode (%s)" % (where, exc))
        return
    if len(data) != frame.size:
        out.errors.append("%s: size mismatch (header %d, body %d)"
                          % (where, frame.size, len(data)))
        return
    digest = hashlib.sha256(data).hexdigest()
    if digest != frame.sha256:
        out.errors.append("%s: sha256 mismatch (header %s, body %s)"
                          % (where, frame.sha256, digest))
        return
    out.frames += 1
    if frame.path in out.files:
        previous = out.files[frame.path]
        if previous == data:
            out.duplicates += 1
        else:
            out.errors.append("%s: conflicting duplicate path" % where)
        return
    out.files[frame.path] = data


def collect_lines(lines, prefix: str = DEFAULT_PREFIX) -> Collected:
    """Parse serial log lines and verify every frame. Pure function, no I/O."""
    out = Collected()
    frame: Optional[Frame] = None
    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r\n")
        if frame is None:
            match = HEADER_RE.search(line)
            if match:
                frame = Frame(match.group(1), int(match.group(2)), match.group(3), number)
            elif EMIT_END in line:
                out.complete = True
            continue

        match = HEADER_RE.search(line)
        if match:
            out.errors.append("line %d %s: truncated frame (next header before @@END@@)"
                              % (frame.line, frame.path))
            frame = Frame(match.group(1), int(match.group(2)), match.group(3), number)
        elif FRAME_END in line:
            _finish(frame, prefix, out)
            frame = None
        elif EMIT_END in line:
            out.errors.append("line %d %s: truncated frame (emit ended before @@END@@)"
                              % (frame.line, frame.path))
            frame = None
            out.complete = True
        elif line and BODY_RE.match(line):
            frame.body.append(line)
        elif line:
            out.noise_lines += 1
    if frame is not None:
        out.errors.append("line %d %s: truncated frame (log ended before @@END@@)"
                          % (frame.line, frame.path))
    return out


def write_files(files: Dict[str, bytes], out_dir: pathlib.Path) -> None:
    """Write verified files under out_dir atomically (temp file, then rename)."""
    root = out_dir.resolve()
    for rel, data in sorted(files.items()):
        target = (root / rel).resolve()
        if os.path.commonpath([str(root), str(target)]) != str(root):
            raise ValueError("path escapes output directory: %s" % rel)
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=str(target.parent), prefix=".collect-")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(data)
            os.replace(tmp, str(target))
        except BaseException:
            if os.path.exists(tmp):
                os.unlink(tmp)
            raise


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Rebuild guest result files from a serial log.")
    parser.add_argument("--log", required=True, type=pathlib.Path, help="serial console log")
    parser.add_argument("--out", required=True, type=pathlib.Path,
                        help="directory the files are written under (the share root)")
    parser.add_argument("--prefix", default=DEFAULT_PREFIX,
                        help="required path prefix for every file (default: %(default)s)")
    args = parser.parse_args(argv)
    try:
        text = args.log.read_bytes().decode("utf-8", errors="replace")
    except OSError as exc:
        print("vm_serial_collect: cannot read %s: %s" % (args.log, exc), file=sys.stderr)
        return EXIT_USAGE
    result = collect_lines(text.splitlines(), args.prefix)
    for message in result.errors:
        print("vm_serial_collect: ERROR %s" % message, file=sys.stderr)
    summary = ("vm_serial_collect: frames=%d files=%d duplicates=%d noise_lines=%d complete=%s"
               % (result.frames, len(result.files), result.duplicates, result.noise_lines,
                  "yes" if result.complete else "no"))
    if not result.complete:
        print(summary)
        print("vm_serial_collect: emit incomplete, nothing written", file=sys.stderr)
        return EXIT_INCOMPLETE
    if result.errors:
        print(summary)
        print("vm_serial_collect: %d verification error(s), nothing written" % len(result.errors),
              file=sys.stderr)
        return EXIT_VERIFY
    try:
        write_files(result.files, args.out)
    except (OSError, ValueError) as exc:
        print("vm_serial_collect: write failed: %s" % exc, file=sys.stderr)
        return EXIT_USAGE
    print(summary)
    for rel in sorted(result.files):
        print("vm_serial_collect: wrote %s (%d bytes)" % (rel, len(result.files[rel])))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
