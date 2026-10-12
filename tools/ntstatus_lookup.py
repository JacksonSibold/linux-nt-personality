#!/usr/bin/env python3
"""
ntstatus_lookup.py - look up NTSTATUS codes and names in the project tables.

Data sources (read from the repository, no kernel or Windows needed):
  src/ntdll/ntstatus.h          STATUS_* name -> value definitions
  src/ntdll/ntstatus_to_dos.c   NtStatusToDosErrorTable (the table that
                                gen_ntstatus_table.py turns into the kernel
                                copy used by src/kernel/nt/ntstatus_kernel.c)

Each query may be a symbolic name (STATUS_ACCESS_VIOLATION, or just
ACCESS_VIOLATION), a hex value (0xC0000005) or a decimal value. For a value,
all known names are printed. The DOS error comes from the table when listed,
otherwise from the same fallback rule the table code uses.

Usage:
  python3 tools/ntstatus_lookup.py STATUS_BUFFER_OVERFLOW 0xC0000005
  python3 tools/ntstatus_lookup.py --search TOO_SMALL
  python3 tools/ntstatus_lookup.py --repo PATH ...

Exit status: 0 all queries found, 1 some query not found or input file
missing/unparseable, 2 usage error.
"""
import argparse
import os
import re
import sys

ERROR_MR_MID_NOT_FOUND = 317
DEFINE_RE = re.compile(
    r"^#define\s+(STATUS_\w+)\s+\(\(NTSTATUS\)\s*(0x[0-9A-Fa-f]+|\d+)L?\s*\)",
    re.MULTILINE,
)
TABLE_RE = re.compile(
    r"static\s+const\s+NTSTATUS_TO_DOS\s+NtStatusToDosErrorTable\[\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
ENTRY_RE = re.compile(r"\{\s*([^,{}]+?)\s*,\s*(\d+)L?\s*\}")
SEVERITY = {0: "SUCCESS", 1: "INFORMATIONAL", 2: "WARNING", 3: "ERROR"}


class LookupFailure(Exception):
    pass


def default_repo():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_text(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError as exc:
        raise LookupFailure(f"cannot read {path}: {exc.strerror}") from None


def parse_literal(text):
    return int(text, 16) if text.lower().startswith("0x") else int(text, 10)


def load_defines(header_path):
    """Return (name -> value, value -> [names]) from ntstatus.h."""
    by_name = {}
    by_value = {}
    for name, lit in DEFINE_RE.findall(read_text(header_path)):
        value = parse_literal(lit) & 0xFFFFFFFF
        if name not in by_name:
            by_name[name] = value
            by_value.setdefault(value, []).append(name)
    if not by_name:
        raise LookupFailure(f"no STATUS_ defines found in {header_path}")
    return by_name, by_value


def load_table(source_path, by_name):
    """Return value -> DOS error from NtStatusToDosErrorTable."""
    m = TABLE_RE.search(read_text(source_path))
    if not m:
        raise LookupFailure(f"NtStatusToDosErrorTable not found in {source_path}")
    table = {}
    for status_expr, dos in ENTRY_RE.findall(m.group(1)):
        expr = status_expr.strip()
        if expr in by_name:
            value = by_name[expr]
        else:
            try:
                value = parse_literal(expr) & 0xFFFFFFFF
            except ValueError:
                continue  # symbol not defined in ntstatus.h; skip
        table.setdefault(value, int(dos))
    if not table:
        raise LookupFailure(f"no entries parsed from {source_path}")
    return table


def fallback_dos(value):
    """Mirror of RtlNtStatusToDosErrorFallback in ntstatus_to_dos.c."""
    st = value & 0xFFFFFFFF
    if not st:
        return 0
    if st & 0x20000000:
        return st
    if (st & 0xF0000000) == 0xD0000000:
        st &= ~0x10000000
    hi = (st >> 16) & 0xFFFF
    if hi in (0xC001, 0x8007, 0xC007):
        return st & 0xFFFF
    return ERROR_MR_MID_NOT_FOUND


def parse_query(text, by_name):
    """Return (value, name_or_None) for a query, or raise ValueError."""
    upper = text.strip().upper()
    for candidate in (upper, "STATUS_" + upper):
        if candidate in by_name:
            return by_name[candidate], candidate
    try:
        return parse_literal(text.strip()) & 0xFFFFFFFF, None
    except ValueError:
        raise ValueError(f"not a STATUS_ name or number: {text!r}") from None


def describe(value, by_value, table):
    lines = [f"0x{value:08X}  ({value})"]
    names = by_value.get(value, [])
    lines.append("  name:     " + (", ".join(names) if names else "(no symbol in ntstatus.h)"))
    severity = SEVERITY[(value >> 30) & 0x3]
    customer = "yes" if value & 0x20000000 else "no"
    lines.append(f"  severity: {severity}  customer-code: {customer}")
    if value in table:
        lines.append(f"  DOS:      {table[value]}  (table)")
    else:
        lines.append(f"  DOS:      {fallback_dos(value)}  (fallback rule)")
    return "\n".join(lines)


def search(needle, by_name):
    needle = needle.upper()
    return sorted((n, v) for n, v in by_name.items() if needle in n)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Look up NTSTATUS codes and names.")
    parser.add_argument("queries", nargs="*", help="STATUS_ name, hex or decimal value")
    parser.add_argument("--search", metavar="TEXT",
                        help="list STATUS_ names containing TEXT (case-insensitive)")
    parser.add_argument("--repo", default=default_repo(),
                        help="repository root (default: parent of tools/)")
    args = parser.parse_args(argv)
    if not args.queries and args.search is None:
        parser.error("give at least one query or --search TEXT")

    repo = os.path.abspath(args.repo)
    try:
        by_name, by_value = load_defines(os.path.join(repo, "src", "ntdll", "ntstatus.h"))
        table = load_table(os.path.join(repo, "src", "ntdll", "ntstatus_to_dos.c"), by_name)
    except LookupFailure as exc:
        print(f"ntstatus_lookup: error: {exc}", file=sys.stderr)
        return 1

    status = 0
    if args.search is not None:
        hits = search(args.search, by_name)
        for name, value in hits:
            print(f"0x{value:08X}  {name}")
        if not hits:
            print(f"no names contain {args.search!r}", file=sys.stderr)
            status = 1

    for query in args.queries:
        try:
            value, _ = parse_query(query, by_name)
        except ValueError as exc:
            print(f"ntstatus_lookup: error: {exc}", file=sys.stderr)
            status = 1
            continue
        if args.search is not None or len(args.queries) > 1:
            print()
        print(describe(value, by_value, table))
    return status


if __name__ == "__main__":
    sys.exit(main())
