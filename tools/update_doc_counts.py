#!/usr/bin/env python3
"""
update_doc_counts.py - keep the counts quoted in the docs equal to the repository.

A count in a Markdown file is wrapped in a block:

    <!-- counts:NAME:begin -->VALUE<!-- counts:NAME:end -->

Every block in the files listed in TARGETS is compared with the value the
repository gives for NAME. --write replaces VALUE where it differs. Values are
counted from the source files; nothing is run:

  unit_modules, unit_tests  tests/unit/test_*.py files, and the functions whose
                            name starts with test_ in them (ast walk, so text
                            inside strings and comments is not counted)
  review_*                  severity totals in docs/review/INDEX.md ("## Reports")
  fix_*                     totals of the "## Fix status" table in the same file
  adr_*                     status column of docs/decisions/INDEX.md
  tracked_files             entries of `git ls-files` (a walk outside a git work tree)

Usage:
  python3 tools/update_doc_counts.py --check    report stale blocks, change nothing
  python3 tools/update_doc_counts.py --write    rewrite stale blocks in place
  python3 tools/update_doc_counts.py --root PATH  use another tree

Exit status: 0 when every block is current (or has been rewritten), 1 when a
block is stale in --check, a block has no matching end marker, a source table
does not add up, or a target file is missing, 2 on usage error.
"""
import argparse
import ast
import os
import pathlib
import re
import subprocess

TARGETS = (
    "README.md",
    "CONTRIBUTING.md",
    "docs/DEVELOPMENT.md",
    "docs/KNOWN-ISSUES.md",
    "docs/status/PROGRESS.md",
)
BLOCK_RE = re.compile(r"<!-- counts:([a-z_]+):begin -->(.*?)<!-- counts:\1:end -->", re.DOTALL)
BEGIN_RE = re.compile(r"<!-- counts:[a-z_]+:begin -->")
END_RE = re.compile(r"<!-- counts:[a-z_]+:end -->")
ADR_ROW_RE = re.compile(r"^\|\s*\[(\d{4})\]\(")
ADR_STATUSES = ("fixed", "open", "blocked", "no-change")
SEVERITIES = ("high", "medium", "low", "info")
FIX_COLUMNS = ("findings", "fixed", "partly", "blocked", "no change", "open")
SKIP_DIRS = {".git", "__pycache__"}


class CountError(Exception):
    """A source file is missing or does not have the expected shape."""


def default_root():
    return pathlib.Path(__file__).resolve().parent.parent


def read_text(path):
    with open(path, encoding="utf-8", newline="") as handle:
        return handle.read()


def tracked_files(root):
    """Repository-relative paths from git, or every file under root outside git."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            capture_output=True, check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        names = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                full = os.path.join(dirpath, name)
                names.append(os.path.relpath(full, root).replace(os.sep, "/"))
        return names
    return [n for n in out.decode("utf-8", "replace").split("\0") if n]


def count_unit_tests(root):
    """(number of test_*.py modules, number of test_* functions in them)."""
    unit = root / "tests" / "unit"
    if not unit.is_dir():
        raise CountError("tests/unit is missing")
    modules = sorted(unit.glob("test_*.py"))
    tests = 0
    for path in modules:
        try:
            tree = ast.parse(read_text(path), filename=str(path))
        except SyntaxError as exc:
            raise CountError(f"{path.name}: {exc}") from None
        tests += sum(
            1
            for node in ast.walk(tree)
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name.startswith("test_")
        )
    return len(modules), tests


def table_after(text, heading):
    """Rows (lists of cell strings, header first) of the table under a '## ' heading."""
    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if line.strip() == heading]
    if not starts:
        raise CountError(f"no '{heading}' heading")
    rows = []
    for line in lines[starts[0] + 1:]:
        stripped = line.strip()
        if stripped.startswith("|"):
            cells = [c.strip() for c in stripped.strip("|").split("|")]
            if not all(re.fullmatch(r":?-+:?", c) for c in cells):
                rows.append(cells)
        elif rows or stripped.startswith("#"):
            break
    if not rows:
        raise CountError(f"no table under '{heading}'")
    return rows


def number(cell, where):
    text = cell.replace("*", "").strip()
    if not text.isdigit():
        raise CountError(f"{where}: expected a number, found {cell!r}")
    return int(text)


def summed_table(text, heading, names):
    """Count the body rows of a table and sum the named columns, checked against its Total row."""
    rows = table_after(text, heading)
    header, body = rows[0], rows[1:]
    lower = [h.lower() for h in header]
    missing = [n for n in names if n not in lower]
    if missing:
        raise CountError(f"'{heading}': no column {missing[0]!r}")
    idx = {n: lower.index(n) for n in names}
    if any(len(r) <= max(idx.values()) for r in rows):
        raise CountError(f"'{heading}': a row is shorter than the header")
    totals = [r for r in body if r[0].strip("*") == "Total"]
    reports = [r for r in body if r[0].strip("*") != "Total"]
    if len(totals) != 1:
        raise CountError(f"'{heading}': expected one Total row")
    sums = {n: sum(number(r[idx[n]], heading) for r in reports) for n in names}
    for n in names:
        given = number(totals[0][idx[n]], heading)
        if given != sums[n]:
            raise CountError(f"'{heading}': column {n!r} sums to {sums[n]}, Total row says {given}")
    return len(reports), sums


def review_facts(root):
    text = read_text(root / "docs" / "review" / "INDEX.md")
    report_count, sev = summed_table(text, "## Reports", SEVERITIES)
    fix_count, fix = summed_table(text, "## Fix status", FIX_COLUMNS)
    if sum(fix[c] for c in FIX_COLUMNS[1:]) != fix["findings"]:
        raise CountError("'## Fix status': fixed, partly, blocked, no change and open do not add up to findings")
    return {
        "review_reports": report_count,
        "review_findings": sum(sev.values()),
        "review_high": sev["high"],
        "review_medium": sev["medium"],
        "review_low": sev["low"],
        "review_info": sev["info"],
        "fix_reports": fix_count,
        "fix_findings": fix["findings"],
        "fix_fixed": fix["fixed"],
        "fix_partly": fix["partly"],
        "fix_blocked": fix["blocked"],
        "fix_no_change": fix["no change"],
        "fix_open": fix["open"],
    }


def adr_facts(root):
    text = read_text(root / "docs" / "decisions" / "INDEX.md")
    status_of = {}
    for line in text.splitlines():
        match = ADR_ROW_RE.match(line.strip())
        if not match:
            continue
        number_text = match.group(1)
        if number_text in status_of:
            raise CountError(f"decision {number_text} is listed twice")
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        status = cells[2].split()[0] if len(cells) > 2 and cells[2].split() else ""
        if status not in ADR_STATUSES:
            raise CountError(f"decision {number_text}: unknown status {cells[2]!r}")
        status_of[number_text] = status
    if not status_of:
        raise CountError("no decision rows in docs/decisions/INDEX.md")
    facts = {"adr_total": len(status_of)}
    for status in ADR_STATUSES:
        key = "adr_" + status.replace("-", "_")
        facts[key] = sum(1 for s in status_of.values() if s == status)
    return facts


def compute_values(root):
    """Every block value, as the string that goes into the docs."""
    modules, tests = count_unit_tests(root)
    values = {"unit_modules": modules, "unit_tests": tests}
    values.update(review_facts(root))
    values.update(adr_facts(root))
    values["tracked_files"] = len(tracked_files(root))
    return {name: str(value) for name, value in values.items()}


def sync(root, values, write):
    """Return (problems, stale, changed). Rewrites target files only when write is true."""
    problems, stale, changed = [], [], []
    for rel in TARGETS:
        path = root / rel
        if not path.is_file():
            problems.append(f"{rel}: target file is missing")
            continue
        text = read_text(path)
        begins = len(BEGIN_RE.findall(text))
        if begins != len(END_RE.findall(text)) or begins != len(BLOCK_RE.findall(text)):
            problems.append(f"{rel}: a counts:begin marker has no matching counts:end marker")
            continue

        def replace(match, rel=rel):
            name, current = match.group(1), match.group(2)
            if name not in values:
                problems.append(f"{rel}: unknown counts block '{name}'")
                return match.group(0)
            want = values[name]
            if current != want:
                stale.append(f"{rel}: block '{name}' reads {current!r}, repository gives {want!r}")
            return f"<!-- counts:{name}:begin -->{want}<!-- counts:{name}:end -->"

        new_text = BLOCK_RE.sub(replace, text)
        if write and new_text != text:
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(new_text)
            changed.append(rel)
    return problems, stale, changed


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check or rewrite the generated counts in the docs.")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="report stale blocks; change no file")
    mode.add_argument("--write", action="store_true", help="rewrite stale blocks in place")
    parser.add_argument("--root", type=pathlib.Path, default=None,
                        help="repository root (default: the tree this script is in)")
    args = parser.parse_args(argv)
    root = args.root or default_root()

    try:
        values = compute_values(root)
    except (CountError, OSError, UnicodeDecodeError) as exc:
        print(f"FAIL [doc-counts] {exc}")
        return 1

    problems, stale, _changed = sync(root, values, args.write)
    for line in problems:
        print(f"FAIL [doc-counts] {line}")
    for line in stale:
        print(f"updated {line}" if args.write else f"FAIL [doc-counts] {line}")
    if problems or (stale and not args.write):
        return 1
    if not stale:
        print("doc counts are current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
