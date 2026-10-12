#!/usr/bin/env python3
"""
check_repo.py - repository hygiene checks for the ntpers snapshot.

Checks (aligned with PUBLICATION_CHECKS.md, which covers Python syntax and
repository hygiene):

  binary-size   no tracked binary file larger than 1 MiB
  py-compile    every tracked .py file compiles (no bytecode is written)
  header-guard  every .c with a same-named .h beside it has an include guard
                (#ifndef/#define pair or #pragma once) in that .h
  crlf-mix      no text file mixes CRLF and bare-LF line endings

Files are taken from `git ls-files` when run inside a git work tree, so
untracked build products are ignored; otherwise the tree is walked.

Usage:
  python3 tools/check_repo.py [--root PATH] [--quiet]

Exit status: 0 when all checks pass, 1 when any check fails, 2 on usage error.
"""
import argparse
import os
import re
import subprocess
import sys

MAX_BINARY_BYTES = 1024 * 1024
BINARY_PROBE_BYTES = 8192
GUARD_RE = re.compile(
    r"^[ \t]*#[ \t]*ifndef[ \t]+(\w+)[ \t]*\r?\n[ \t]*#[ \t]*define[ \t]+\1\b",
    re.MULTILINE,
)
SKIP_DIRS = {".git", "__pycache__"}


def repo_root_default():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def list_files(root):
    """Return repo-relative POSIX paths, from git if possible, else by walking."""
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "-z"],
            capture_output=True, check=True,
        ).stdout
        names = [n for n in out.decode("utf-8", "replace").split("\0") if n]
    except (OSError, subprocess.CalledProcessError):
        names = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                full = os.path.join(dirpath, fn)
                names.append(os.path.relpath(full, root).replace(os.sep, "/"))
    return sorted(n for n in names if os.path.isfile(os.path.join(root, n)))


def is_binary(head):
    return b"\x00" in head


def check_file(root, rel, failures):
    """Run the per-file checks. Returns True if the file was text (read in full)."""
    full = os.path.join(root, rel)
    size = os.path.getsize(full)
    with open(full, "rb") as fh:
        head = fh.read(BINARY_PROBE_BYTES)
    if is_binary(head):
        if size > MAX_BINARY_BYTES:
            failures.append(("binary-size", rel,
                             f"binary is {size} bytes (limit {MAX_BINARY_BYTES})"))
        return False

    with open(full, "rb") as fh:
        data = fh.read()

    if rel.endswith(".py"):
        try:
            compile(data, rel, "exec", dont_inherit=True)
        except SyntaxError as exc:
            failures.append(("py-compile", rel,
                             f"line {exc.lineno}: {exc.msg}"))

    crlf = data.count(b"\r\n")
    bare_lf = data.count(b"\n") - crlf
    if crlf and bare_lf:
        failures.append(("crlf-mix", rel,
                         f"{crlf} CRLF lines and {bare_lf} bare-LF lines"))
    return True


def check_header_guards(root, files, failures):
    tracked = set(files)
    for rel in files:
        if not rel.endswith(".c"):
            continue
        hdr = rel[:-2] + ".h"
        if hdr not in tracked:
            continue  # no matching header: guard check not applicable
        with open(os.path.join(root, hdr), "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
        if re.search(r"^[ \t]*#[ \t]*pragma[ \t]+once", text, re.MULTILINE):
            continue
        if GUARD_RE.search(text):
            continue
        failures.append(("header-guard", hdr,
                         f"no include guard (needed by {rel})"))


def run(root, quiet):
    files = list_files(root)
    failures = []
    text_count = 0
    for rel in files:
        if check_file(root, rel, failures):
            text_count += 1
    check_header_guards(root, files, failures)

    if not quiet:
        for check, path, detail in failures:
            print(f"FAIL [{check}] {path}: {detail}")
    print(f"checked {len(files)} files ({text_count} text), "
          f"{len(failures)} failure(s)")
    return 1 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Repository hygiene checks.")
    parser.add_argument("--root", default=repo_root_default(),
                        help="repository root (default: parent of tools/)")
    parser.add_argument("--quiet", action="store_true",
                        help="print only the summary line")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"check_repo: error: not a directory: {root}", file=sys.stderr)
        return 2
    return run(root, args.quiet)


if __name__ == "__main__":
    sys.exit(main())
