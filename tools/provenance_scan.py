#!/usr/bin/env python3
"""
provenance_scan.py - factual provenance inventory for the tracked files.

For every file reported by `git ls-files` the scan records:

  path, size, binary flag
  SPDX tags found anywhere in the file (line, expression, syntax check)
  header notice lines that name a holder, in the first 40 lines
  counts of upstream-kernel and licence-family mentions in the whole file
  whether the file is a patch (.patch/.diff with diff headers)
  whether the top of the file carries Windows-header markers
  whether the file says it is generated or must not be edited
  a heuristic class:

    original             a notice is present and no third-party or kernel
                         marker was found
    linux-derived-patch  a patch file, or a kernel-style header marker or a
                         licence tag of the kernel family (non-prose files only)
    generated            a generated or do-not-edit marker in the header
    third-party-notice   a licence, copying, notice or credits file
    unknown              no marker found; origin is not guessed

The classes are heuristics for a human reviewer. They are not a statement of
authorship or licence, and they do not grant any licence. The SPDX check is
syntactic only; it does not look identifiers up in the SPDX list.

Usage:
  python3 tools/provenance_scan.py                       summary counts
  python3 tools/provenance_scan.py --markdown docs/PROVENANCE.md
  python3 tools/provenance_scan.py --json PATH           records (PATH "-" = stdout)
  python3 tools/provenance_scan.py --check               fail on malformed tags only
  python3 tools/provenance_scan.py --selftest
  python3 tools/provenance_scan.py --root PATH           scan another tree

Exit status: 0 success, 1 a malformed tag (--check) or a selftest failure,
2 usage error.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HEADER_LINES = 40
GENERATED_LINES = 15
BINARY_PROBE_BYTES = 8192
MAX_COPYRIGHT = 5
MAX_CLIP = 160
SKIP_DIRS = {".git", "__pycache__"}
# The report is generated from the scan, so it is excluded to keep it stable.
SELF_REPORT = "docs/PROVENANCE.md"

ORIGINAL = "original"
LINUX = "linux-derived-patch"
GENERATED = "generated"
NOTICE = "third-party-notice"
UNKNOWN = "unknown"
CLASSES = (ORIGINAL, LINUX, GENERATED, NOTICE, UNKNOWN)

# Built from parts so this file does not contain the literal tag text.
SPDX_MARK = "SPDX-" + "License-Identifier:"
SPDX_RE = re.compile(re.escape(SPDX_MARK) + r"(.*)$")
TRAILER_RE = re.compile(r"\s*(\*/|-->|\*\))\s*$")
TOKEN_RE = re.compile(r"\(|\)|[^\s()]+")
OPERATORS = ("AND", "OR", "WITH")
ID_RE = re.compile(
    r"^(DocumentRef-[A-Za-z0-9.\-+]+:)?"
    r"(LicenseRef-[A-Za-z0-9.\-]+|[A-Za-z0-9][A-Za-z0-9.\-]*\+?)$"
)
COPYRIGHT_RE = re.compile(r"(?i)\bcopyright\b|\(c\)\s*\d{4}")
LINUX_RE = re.compile(r"(?i)linux kernel")
GPL_RE = re.compile(r"(?i)\b[LA]?GPL\b")
HEADER_KERNEL_RE = re.compile(r"(?i)linux kernel|\b[LA]?GPL\b")
DIFF_HDR_RE = re.compile(r"^--- .+\n\+\+\+ ", re.M)
GENERATED_RE = re.compile(r"(?i)auto-?generated|generated (by|from|automatically)|do not edit")
NOTICE_NAME_RE = re.compile(
    r"^(LICEN[CS]E|COPYING|NOTICE|THIRD[-_ ]?PARTY|CREDITS|AUTHORS)(\b|[._-])",
    re.IGNORECASE,
)
WINDOWS_TEXT_RE = re.compile(
    r"Microsoft Corporation|Copyright \(c\) Microsoft|\bReactOS\b|\bWine\b"
)
PROSE_SUFFIXES = (".md", ".txt", ".rst")
WINDOWS_HEADER_NAMES = {
    "windows.h", "winnt.h", "ntdef.h", "winternl.h", "ntstatus.h",
    "winbase.h", "winuser.h", "ntddk.h", "wdm.h",
}


def repo_root_default():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def list_files(root, use_git=True):
    """Return repo-relative POSIX paths, from git when possible, else by walking."""
    names = None
    if use_git:
        try:
            out = subprocess.run(
                ["git", "-C", root, "ls-files", "-z"],
                capture_output=True, check=True,
            ).stdout
            names = [n for n in out.decode("utf-8", "replace").split("\0") if n]
        except (OSError, subprocess.CalledProcessError):
            names = None
    if names is None:
        names = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                full = os.path.join(dirpath, fn)
                names.append(os.path.relpath(full, root).replace(os.sep, "/"))
    names = [n.replace(os.sep, "/") for n in names if n != SELF_REPORT]
    return sorted(n for n in names if os.path.isfile(os.path.join(root, n)))


class _Parser:
    """Recursive-descent parser for the SPDX license expression grammar."""

    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None

    def take(self):
        tok = self.peek()
        self.pos += 1
        return tok

    def expr(self):
        self.term()
        while self.peek() in ("AND", "OR"):
            self.take()
            self.term()

    def term(self):
        tok = self.peek()
        if tok is None:
            raise ValueError("unexpected end of expression")
        if tok == "(":
            self.take()
            self.expr()
            if self.peek() != ")":
                raise ValueError("missing closing parenthesis")
            self.take()
            return
        self.simple_id()
        if self.peek() == "WITH":
            self.take()
            self.simple_id()

    def simple_id(self):
        tok = self.take()
        if tok is None:
            raise ValueError("unexpected end of expression")
        if tok in OPERATORS or tok in ("(", ")"):
            raise ValueError("unexpected token %r" % tok)
        if not ID_RE.match(tok):
            raise ValueError("invalid license identifier %r" % tok)


def parse_expression(text):
    """Return (normalised_expression, None) if well formed, else (text, error)."""
    value = TRAILER_RE.sub("", text).strip()
    if not value:
        return value, "empty expression"
    tokens = TOKEN_RE.findall(value)
    parser = _Parser(tokens)
    try:
        parser.expr()
        if parser.pos != len(tokens):
            raise ValueError("unexpected token %r" % tokens[parser.pos])
    except ValueError as exc:
        return value, str(exc)
    return value, None


def clip(text):
    text = text.strip()
    return text if len(text) <= MAX_CLIP else text[:MAX_CLIP - 3] + "..."


def classify(rec):
    """Return (class, reasons) for a scanned record. Heuristic only."""
    if rec["notice_file"]:
        return NOTICE, ["notice-type file name"]
    if rec["generated"]:
        return GENERATED, ["generated or do-not-edit marker in first %d lines" % GENERATED_LINES]
    if rec["patch"]:
        return LINUX, ["patch file with diff headers; upstream base not verified"]
    if rec["header_kernel_style"]:
        return LINUX, ["Linux/GPL marker in header or GPL SPDX tag; origin not verified"]
    if rec["windows_marker"]:
        return UNKNOWN, ["Windows-header marker; needs human review"]
    if rec["spdx"] or rec["copyright"]:
        return ORIGINAL, ["notice present; no third-party or kernel marker"]
    return UNKNOWN, ["no provenance marker"]


def scan_file(root, rel):
    """Return the provenance record for one tracked file."""
    with open(os.path.join(root, rel), "rb") as fh:
        data = fh.read()
    base = rel.rsplit("/", 1)[-1]
    rec = {
        "path": rel,
        "size": len(data),
        "binary": False,
        "spdx": [],
        "copyright": [],
        "linux_kernel_mentions": 0,
        "gpl_mentions": 0,
        "patch": False,
        "header_kernel_style": False,
        "windows_marker": False,
        "generated": False,
        "notice_file": bool(NOTICE_NAME_RE.match(base)) or "LICENSES" in rel.split("/"),
    }
    if b"\x00" in data[:BINARY_PROBE_BYTES]:
        rec["binary"] = True
        rec["class"], rec["reasons"] = classify(rec)
        return rec

    text = data.decode("utf-8", "replace")
    lines = text.splitlines()
    head = lines[:HEADER_LINES]
    head_text = "\n".join(head)

    for num, line in enumerate(lines, 1):
        match = SPDX_RE.search(line)
        if match:
            expr, error = parse_expression(match.group(1))
            rec["spdx"].append({"line": num, "expression": expr, "error": error})

    # Prose files mention kernels, Windows and copyright without containing
    # their code, so header-text markers are only read from non-prose files.
    prose = rel.endswith(PROSE_SUFFIXES)

    if not prose:
        for num, line in enumerate(head, 1):
            if len(rec["copyright"]) >= MAX_COPYRIGHT:
                break
            if COPYRIGHT_RE.search(line):
                rec["copyright"].append({"line": num, "text": clip(line.strip(" \t/*#;-!<>"))})

    rec["linux_kernel_mentions"] = len(LINUX_RE.findall(text))
    rec["gpl_mentions"] = len(GPL_RE.findall(text))
    gpl_tagged = any(GPL_RE.search(t["expression"]) for t in rec["spdx"])
    rec["header_kernel_style"] = (not prose) and (
        bool(HEADER_KERNEL_RE.search(head_text)) or gpl_tagged
    )
    rec["generated"] = bool(GENERATED_RE.search("\n".join(lines[:GENERATED_LINES])))
    rec["patch"] = rel.endswith((".patch", ".diff")) and bool(DIFF_HDR_RE.search(text))
    rec["windows_marker"] = (base in WINDOWS_HEADER_NAMES) or (
        not prose and bool(WINDOWS_TEXT_RE.search(head_text))
    )
    rec["class"], rec["reasons"] = classify(rec)
    return rec


def scan_tree(root, use_git=True):
    return [scan_file(root, rel) for rel in list_files(root, use_git=use_git)]


def malformed_tags(records):
    """Yield (path, line, error) for every SPDX tag with a syntax error."""
    for rec in records:
        for tag in rec["spdx"]:
            if tag["error"]:
                yield rec["path"], tag["line"], tag["error"]


def summary_lines(records):
    total_tags = sum(len(r["spdx"]) for r in records)
    bad = sum(1 for _ in malformed_tags(records))
    counts = {c: 0 for c in CLASSES}
    for rec in records:
        counts[rec["class"]] += 1
    lines = ["provenance: %d tracked files" % len(records)]
    for cls in CLASSES:
        lines.append("  %-20s %d" % (cls, counts[cls]))
    lines.append("  SPDX tags: %d, malformed: %d" % (total_tags, bad))
    return lines


def _cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def _review_rows(records):
    return [
        r for r in records
        if r["class"] == LINUX or r["windows_marker"]
        or r["linux_kernel_mentions"] or r["gpl_mentions"]
    ]


def _notice_or_unknown(records):
    return [
        r for r in records
        if r["class"] in (UNKNOWN, NOTICE) or r["spdx"] or r["copyright"]
    ]


def render_markdown(records):
    counts = {c: 0 for c in CLASSES}
    for rec in records:
        counts[rec["class"]] += 1
    tags = [t for r in records for t in r["spdx"]]
    tag_values = {}
    for t in tags:
        key = t["expression"] or "(empty)"
        tag_values[key] = tag_values.get(key, 0) + 1
    out = []
    add = out.append
    add("# Provenance inventory")
    add("")
    add("Generated by `tools/provenance_scan.py --markdown docs/PROVENANCE.md` from the "
        "files reported by `git ls-files`. Regenerate it with that command; do not edit it by hand.")
    add("")
    add("This is a factual inventory. It is not legal advice, it does not add or change any "
        "licence grant, and it does not establish authorship. The classes are heuristics:")
    add("`unknown` means the scan found no marker in the file, not that the file is unlicensed "
        "or third-party. `original` means no third-party or kernel marker was found, not that "
        "the project wrote the file. The report excludes itself.")
    add("")
    add("## Summary")
    add("")
    add("| Class | Files |")
    add("|---|---:|")
    for cls in CLASSES:
        add("| %s | %d |" % (cls, counts[cls]))
    add("| **total** | **%d** |" % len(records))
    add("")
    add("| Measure | Value |")
    add("|---|---:|")
    add("| Binary files | %d |" % sum(1 for r in records if r["binary"]))
    add("| Files with at least one SPDX tag | %d |" % sum(1 for r in records if r["spdx"]))
    add("| SPDX tags found | %d |" % len(tags))
    add("| Malformed SPDX tags | %d |" % sum(1 for _ in malformed_tags(records)))
    add("| Files with a copyright line in the header | %d |"
        % sum(1 for r in records if r["copyright"]))
    add("| Patch files (`.patch`/`.diff` with diff headers) | %d |"
        % sum(1 for r in records if r["patch"]))
    add("| Files with Windows-header markers | %d |" % sum(1 for r in records if r["windows_marker"]))
    add("| Files mentioning \"Linux kernel\" or GPL | %d |"
        % sum(1 for r in records if r["linux_kernel_mentions"] or r["gpl_mentions"]))
    add("")
    add("SPDX tag values as written (count of tags):")
    add("")
    for key in sorted(tag_values):
        add("- `%s`: %d" % (_cell(key), tag_values[key]))
    add("")
    add("## Files for human review")
    add("")
    add("Files classed `linux-derived-patch`, files with a Windows-header marker, and files "
        "that mention \"Linux kernel\" or GPL anywhere in their text. Whether any of them derive "
        "from upstream kernel or Windows code is not established by this scan.")
    add("")
    add("| Path | Class | SPDX | Linux kernel / GPL mentions | Windows marker | Reasons |")
    add("|---|---|---|---:|---|---|")
    for r in _review_rows(records):
        spdx = ", ".join(t["expression"] for t in r["spdx"]) or "-"
        add("| `%s` | %s | %s | %d / %d | %s | %s |" % (
            _cell(r["path"]), r["class"], _cell(spdx),
            r["linux_kernel_mentions"], r["gpl_mentions"],
            "yes" if r["windows_marker"] else "no",
            _cell("; ".join(r["reasons"])),
        ))
    add("")
    add("## Files with notices or unknown provenance")
    add("")
    add("Files that have an SPDX tag or copyright line, that are third-party notices, or whose "
        "class is `unknown`. Copyright text is the first matching header line, clipped.")
    add("")
    add("| Path | Size | Class | SPDX | Copyright (first header line) |")
    add("|---|---:|---|---|---|")
    for r in _notice_or_unknown(records):
        spdx = ", ".join(t["expression"] for t in r["spdx"]) or "-"
        copy = r["copyright"][0]["text"] if r["copyright"] else "-"
        add("| `%s` | %d | %s | %s | %s |" % (
            _cell(r["path"]), r["size"], r["class"], _cell(spdx), _cell(copy)))
    add("")
    return "\n".join(out)


def run_check(root, use_git=True):
    records = scan_tree(root, use_git=use_git)
    failures = list(malformed_tags(records))
    for path, line, error in failures:
        print("FAIL [spdx-malformed] %s:%d: %s" % (path, line, error))
    total = sum(len(r["spdx"]) for r in records)
    print("provenance check: %d files, %d SPDX tags, %d malformed"
          % (len(records), total, len(failures)))
    return 1 if failures else 0


def selftest():
    """Check the parser and classifier on an in-memory fixture. Returns a list of failures."""
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    valid = ["MIT", "GPL-2.0-only OR MIT", "GPL-2.0 WITH Linux-syscall-note",
             "(MIT OR Apache-2.0) AND BSD-3-Clause", "LicenseRef-Foo", "GPL-2.0+",
             "DocumentRef-x:LicenseRef-y"]
    invalid = ["", "MIT AND", "MIT OR)", "(MIT", "MIT BSD", "WITH MIT", "GPL-2.0 WITH",
               "MIT ! BSD", "()"]
    for expr in valid:
        expect(parse_expression(expr)[1] is None, "valid expression rejected: %r" % expr)
    for expr in invalid:
        expect(parse_expression(expr)[1] is not None, "invalid expression accepted: %r" % expr)

    cases = {
        "a.c": ("/* " + SPDX_MARK + " GPL-2.0 */\n/* Linux kernel style */\n", LINUX),
        "b.c": ("/* Copyright (c) 2020 Example Author */\nint x;\n", ORIGINAL),
        "bad.c": ("// " + SPDX_MARK + " GPL-2.0 AND (MIT\n", LINUX),
        "LICENSE": ("MIT License text\n", NOTICE),
        "gen.inc": ("/* AUTO-GENERATED - DO NOT EDIT */\n{ 1 },\n", GENERATED),
        "p/x.patch": ("--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n", LINUX),
        "ntstatus.h": ("/* Copyright (c) Microsoft Corporation */\n", UNKNOWN),
        "plain.txt": ("nothing to see\n", UNKNOWN),
    }
    with tempfile.TemporaryDirectory() as tmp:
        for rel, (text, _) in cases.items():
            full = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "wb") as fh:
                fh.write(text.encode("utf-8"))
        with open(os.path.join(tmp, "bin.dat"), "wb") as fh:
            fh.write(b"\x00\x01\x02")
        records = {r["path"]: r for r in scan_tree(tmp, use_git=False)}
    expect(set(records) == set(cases) | {"bin.dat"}, "file listing mismatch")
    for rel, (_, want) in cases.items():
        got = records.get(rel, {}).get("class")
        expect(got == want, "%s classed %r, expected %r" % (rel, got, want))
    expect(records["bin.dat"]["binary"] and records["bin.dat"]["class"] == UNKNOWN,
           "binary file not recorded as unknown")
    expect(len(records["bad.c"]["spdx"]) == 1 and records["bad.c"]["spdx"][0]["error"],
           "malformed tag not reported")
    expect(records["a.c"]["spdx"][0]["expression"] == "GPL-2.0", "tag value not extracted")
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description="Provenance inventory for tracked files.")
    parser.add_argument("--root", default=repo_root_default(), help="tree to scan")
    parser.add_argument("--markdown", metavar="PATH", help="write the report to PATH")
    parser.add_argument("--json", metavar="PATH", help="write records as JSON ('-' = stdout)")
    parser.add_argument("--check", action="store_true",
                        help="fail only on malformed SPDX tags")
    parser.add_argument("--selftest", action="store_true", help="run the built-in fixture")
    args = parser.parse_args(argv)

    if args.selftest:
        failures = selftest()
        for item in failures:
            print("FAIL [selftest] %s" % item)
        print("provenance selftest: %s" % ("ok" if not failures else "%d failure(s)" % len(failures)))
        return 1 if failures else 0

    if args.check:
        return run_check(args.root)

    records = scan_tree(args.root)
    if args.json:
        text = json.dumps(records, indent=2, sort_keys=True) + "\n"
        if args.json == "-":
            sys.stdout.write(text)
        else:
            with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_markdown(records))
    if not args.json and not args.markdown:
        print("\n".join(summary_lines(records)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
