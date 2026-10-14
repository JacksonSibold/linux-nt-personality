#!/usr/bin/env python3
"""Report test sources under src/ that no Makefile builds or runs.

A test source is a .c file under the scan root (default src/) whose name is
test-like: `*_test.c`, `test_*.c` or `*_tests.c`. It counts as referenced when
its stem appears as a whole word on a non-comment line of any Makefile in the
repository (Makefile, GNUmakefile, makefile, *.mk). A word that only appears
in a comment does not count.

This is a report, not a fix: it lists orphans and does not wire them into any
Makefile. Wiring a test is a separate step, after it has built and passed.

Exit codes: 0 no orphans, 1 orphans listed, 2 the scan root does not exist.
"""
import argparse
import pathlib
import re
import sys

TEST_NAME = re.compile(r"^(?:.+_tests?|test_.+|.+_tests)\.c$")
SKIP_DIRS = {".git", "build", "build-cov", "build-asan", "build-tsan", "node_modules", "__pycache__"}
MAKEFILE_NAMES = {"Makefile", "GNUmakefile", "makefile"}


def is_test_source(name):
    return bool(TEST_NAME.match(name))


def is_makefile(path):
    return path.name in MAKEFILE_NAMES or path.suffix == ".mk"


def _walk(root, keep):
    for path in sorted(root.rglob("*")):
        if any(part in SKIP_DIRS for part in path.relative_to(root).parts[:-1]):
            continue
        if path.is_file() and keep(path):
            yield path


def find_tests(root, scan_root):
    base = root / scan_root
    return [p for p in _walk(base, lambda p: is_test_source(p.name))]


def code_lines(text):
    """Lines of a Makefile with comment-only lines and trailing comments removed."""
    out = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        out.append(line.split(" #", 1)[0] if " #" in line else line)
    return out


def makefile_text(root):
    parts = []
    for path in _walk(root, is_makefile):
        parts.append("\n".join(code_lines(path.read_text(encoding="utf-8", errors="replace"))))
    return "\n".join(parts)


def find_orphans(root, scan_root="src"):
    """Return (orphans, total): test sources under scan_root that no Makefile names."""
    tests = find_tests(root, scan_root)
    text = makefile_text(root)
    orphans = []
    for path in tests:
        stem = path.stem
        if not re.search(r"(?<![\w.-])" + re.escape(stem) + r"(?![\w-])", text):
            orphans.append(path.relative_to(root).as_posix())
    return orphans, len(tests)


def main(argv=None):
    here = pathlib.Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(here), help="repository root")
    ap.add_argument("--scan", default="src", help="directory under the root to scan for test sources")
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root).resolve()
    if not (root / args.scan).is_dir():
        print(f"scan root not found: {root / args.scan}", file=sys.stderr)
        return 2
    orphans, total = find_orphans(root, args.scan)
    for rel in orphans:
        print(f"orphan {rel}")
    print(f"orphans: {len(orphans)} of {total} test sources under {args.scan}/")
    return 1 if orphans else 0


if __name__ == "__main__":
    sys.exit(main())
