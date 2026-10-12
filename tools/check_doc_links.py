#!/usr/bin/env python3
"""
check_doc_links.py - check relative links and anchors in Markdown files.

For every tracked *.md file (or every *.md under the root when the root is not
a git work tree) the checker reads inline links, images, reference definitions
and href/src attributes, and reports:

  - a relative target that does not exist. The match is case-sensitive, so a
    link that only works on a case-insensitive filesystem is reported too;
  - a target that resolves outside the repository;
  - a `#anchor` that is not a heading (or an HTML id/name) in the target page.
    Headings are slugged the way GitHub does it, with -1, -2 suffixes for
    repeated headings.

External links (any URL scheme), links inside fenced or inline code, and
fragments on non-Markdown files are not checked.

Usage:
  python3 tools/check_doc_links.py [--root PATH] [--quiet]
  python3 tools/check_doc_links.py --selftest

Exit status: 0 when every link resolves, 1 when a link is broken, 2 on usage
error. --selftest exits 0 when its built-in fixture gives the expected result.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from urllib.parse import unquote

SKIP_DIRS = {".git", "__pycache__"}
FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")
ATX_RE = re.compile(r"^\s{0,3}#{1,6}\s+(.*?)\s*#*\s*$")
INLINE_CODE_RE = re.compile(r"`[^`]*`")
INLINE_LINK_RE = re.compile(r"!?\[(?:[^\[\]]|\[[^\[\]]*\])*\]\(\s*(<[^>]*>|[^\s)]+)")
REF_DEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]*>|\S+)")
ATTR_RE = re.compile(r"""\b(?:href|src)\s*=\s*["']([^"']+)["']""")
HTML_ANCHOR_RE = re.compile(r"""<a\s[^>]*?\b(?:name|id)\s*=\s*["']([^"']+)["']""", re.IGNORECASE)
ID_RE = re.compile(r"""\bid\s*=\s*["']([^"']+)["']""")
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.\-]*:")


def repo_root_default():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def slugify(heading):
    """GitHub-style anchor for one heading's text (before duplicate numbering)."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def scan_lines(text):
    """Yield (lineno, line, in_code) for every line; in_code is True inside fences."""
    fence = None
    for no, line in enumerate(text.splitlines(), 1):
        m = FENCE_RE.match(line)
        if fence is None:
            if m:
                fence = m.group(1)
                yield no, line, True
            else:
                yield no, line, False
            continue
        s = line.strip()
        if s and set(s) == {fence[0]} and len(s) >= len(fence):
            fence = None
        yield no, line, True


def anchors_in(text):
    """Return the set of fragment identifiers a page defines."""
    anchors = set()
    seen = {}
    for _, line, in_code in scan_lines(text):
        if in_code:
            continue
        m = ATX_RE.match(line)
        if m:
            slug = slugify(m.group(1))
            if slug in seen:
                seen[slug] += 1
                anchors.add(f"{slug}-{seen[slug]}")
            else:
                seen[slug] = 0
                anchors.add(slug)
        for rx in (HTML_ANCHOR_RE, ID_RE):
            anchors.update(rx.findall(line))
    return anchors


def iter_links(text):
    """Yield (lineno, target) for every link-like target outside code."""
    for no, line, in_code in scan_lines(text):
        if in_code:
            continue
        plain = INLINE_CODE_RE.sub("", line)
        for m in INLINE_LINK_RE.finditer(plain):
            yield no, m.group(1).strip("<>")
        m = REF_DEF_RE.match(plain)
        if m:
            yield no, m.group(1).strip("<>")
        for m in ATTR_RE.finditer(plain):
            yield no, m.group(1)


def split_target(target):
    """Return (path, fragment) for a link to check, or None to skip it."""
    if not target or SCHEME_RE.match(target) or target.startswith("//"):
        return None
    path, _, frag = target.partition("#")
    path = path.split("?", 1)[0]
    return unquote(path), unquote(frag)


def exists_exact(root, rel):
    """True when every component of rel exists with exactly this spelling."""
    cur = root
    for part in rel.split("/"):
        if part in ("", "."):
            continue
        try:
            names = os.listdir(cur)
        except OSError:
            return False
        if part not in names:
            return False
        cur = os.path.join(cur, part)
    return True


def list_markdown(root):
    """Repo-relative POSIX paths of Markdown files, from git when possible."""
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
    return sorted(
        n for n in names
        if n.lower().endswith(".md") and os.path.isfile(os.path.join(root, n))
    )


def read_text(root, rel):
    with open(os.path.join(root, rel), "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def check_target(root, src, target, anchor_cache):
    """Return a problem description for one link target, or None if it is fine."""
    parts = split_target(target)
    if parts is None:
        return None
    path, frag = parts
    if path == "":
        dest = src
    elif path.startswith("/"):
        dest = os.path.normpath(os.path.join(root, path.lstrip("/")))
        dest = os.path.relpath(dest, root).replace(os.sep, "/")
    else:
        joined = os.path.normpath(os.path.join(root, os.path.dirname(src), path))
        dest = os.path.relpath(joined, root).replace(os.sep, "/")
    if dest == ".." or dest.startswith("../"):
        return "points outside the repository"
    if not exists_exact(root, dest):
        return f"no such file: {dest}"
    if not frag or not dest.lower().endswith(".md"):
        return None
    full = os.path.join(root, dest)
    if os.path.isdir(full):
        return None
    if dest not in anchor_cache:
        anchor_cache[dest] = anchors_in(read_text(root, dest))
    if frag not in anchor_cache[dest]:
        return f"no anchor #{frag} in {dest}"
    return None


def check_tree(root, files=None):
    """Check links in the given Markdown files (default: all found under root).

    Returns (problems, files_checked, links_checked), where problems is a list
    of (rel_path, lineno, target, detail).
    """
    if files is None:
        files = list_markdown(root)
    problems = []
    anchor_cache = {}
    links = 0
    for rel in files:
        for no, target in iter_links(read_text(root, rel)):
            if split_target(target) is None:
                continue
            links += 1
            detail = check_target(root, rel, target, anchor_cache)
            if detail:
                problems.append((rel, no, target, detail))
    return problems, len(files), links


SELFTEST_FILES = {
    "index.md": (
        "# Index\n"
        "\n"
        "See [sub](sub/page.md#real-heading), [dup](sub/page.md#repeat-1).\n"
        "Broken: [missing](gone.md) and [badanchor](sub/page.md#nope).\n"
        "Same page: [top](#index) and [nowhere](#absent).\n"
        "Case: [wrongcase](Sub/page.md).\n"
        "Escape: [out](../../outside.md).\n"
        "Image: ![pic](img/pic.png) and directory [dir](sub/).\n"
        "Reference: [ref][r] and broken reference [bref][b].\n"
        "\n"
        "[r]: sub/page.md#real-heading\n"
        "[b]: nowhere.md\n"
        "\n"
        "External: [web](https://example.com/#x) and [mail](mailto:a@b.c).\n"
        "Code: `[inline](gone.md)`\n"
        "\n"
        "```\n"
        "[fenced](gone.md)\n"
        "# not a heading\n"
        "```\n"
        "\n"
        "Fragment on a non-page: [code](img/pic.png#L3).\n"
    ),
    "sub/page.md": (
        "# Page\n"
        "\n"
        "## Real heading\n"
        "\n"
        "## Repeat\n"
        "\n"
        "## Repeat\n"
        "\n"
        "Back to [index](../index.md#index), bad [up](../../../x.md).\n"
        "Anchor by id: <a id=\"custom-id\"></a>\n"
        "\n"
        "~~~\n"
        "## Hidden in fence\n"
        "~~~\n"
    ),
    "img/pic.png": "not really a png\n",
}

SELFTEST_EXPECTED = {
    ("index.md", "gone.md"),
    ("index.md", "sub/page.md#nope"),
    ("index.md", "#absent"),
    ("index.md", "Sub/page.md"),
    ("index.md", "../../outside.md"),
    ("index.md", "nowhere.md"),
    ("sub/page.md", "../../../x.md"),
}


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        for rel, body in SELFTEST_FILES.items():
            full = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(body)
        problems, nfiles, _ = check_tree(tmp, sorted(
            k for k in SELFTEST_FILES if k.endswith(".md")))
        got = {(rel, target) for rel, _, target, _ in problems}
        if got != SELFTEST_EXPECTED:
            missing = sorted(SELFTEST_EXPECTED - got)
            extra = sorted(got - SELFTEST_EXPECTED)
            print(f"selftest FAIL: missing {missing}, unexpected {extra}",
                  file=sys.stderr)
            return 1
        if nfiles != 2:
            print(f"selftest FAIL: expected 2 Markdown files, got {nfiles}",
                  file=sys.stderr)
            return 1
    print("selftest ok: 7 expected broken links found, 0 false positives")
    return 0


def run(root, quiet):
    files = list_markdown(root)
    problems, nfiles, nlinks = check_tree(root, files)
    if not quiet:
        for rel, no, target, detail in problems:
            print(f"FAIL [doc-link] {rel}:{no}: ({target}) {detail}")
    print(f"checked {nfiles} Markdown files, {nlinks} links, "
          f"{len(problems)} broken")
    return 1 if problems else 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check relative links and anchors in Markdown files.")
    parser.add_argument("--root", default=repo_root_default(),
                        help="repository root (default: parent of tools/)")
    parser.add_argument("--quiet", action="store_true",
                        help="print only the summary line")
    parser.add_argument("--selftest", action="store_true",
                        help="check the built-in fixture and exit")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"check_doc_links: error: not a directory: {root}", file=sys.stderr)
        return 2
    return run(root, args.quiet)


if __name__ == "__main__":
    sys.exit(main())
