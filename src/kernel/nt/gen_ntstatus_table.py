#!/usr/bin/env python3
"""
gen_ntstatus_table.py - generate kernel NTSTATUS->DOS table from userspace source of truth.

Reads src/ntdll/ntstatus_to_dos.c and emits src/kernel/nt/ntstatus_table.inc
containing the table entries. This ensures the kernel table cannot drift
from the userspace one; any change to the userspace table must be re-generated.

Every line of the table body that starts with "{" must parse as an entry, or the
script fails and writes nothing. Entries may use a symbolic name or a cast hex
literal, and values may carry U/L suffixes (5L, 1168LL, 0x10L).

Usage:
  python3 src/kernel/nt/gen_ntstatus_table.py \
    --input src/ntdll/ntstatus_to_dos.c \
    --output src/kernel/nt/ntstatus_table.inc [--expect N]
"""
import re, pathlib, sys, argparse

parser = argparse.ArgumentParser(description="Generate kernel NTSTATUS->DOS table")
parser.add_argument("--input", required=True, help="Input userspace ntstatus_to_dos.c")
parser.add_argument("--output", required=True, help="Output inc file")
parser.add_argument("--expect", type=int, default=None,
                    help="Fail unless exactly this many entries are found")
args = parser.parse_args()

inp = pathlib.Path(args.input)
outp = pathlib.Path(args.output)

text = inp.read_text(encoding="utf-8")

# Extract the table body between the array definition and the closing };
m = re.search(
    r'static\s+const\s+NTSTATUS_TO_DOS\s+NtStatusToDosErrorTable\[\]\s*=\s*\{(.*?)\};',
    text,
    re.DOTALL
)
if not m:
    print("ERROR: Could not locate NtStatusToDosErrorTable in input", file=sys.stderr)
    sys.exit(1)

body = m.group(1)

# One entry per line: { NAME, VALUE }, with an optional trailing comma.
# NAME is a STATUS_ identifier or a (NTSTATUS) cast hex literal.
# VALUE is a decimal or hex integer with optional U/L suffixes.
entry_re = re.compile(
    r'\{\s*'
    r'((?:[A-Za-z_][A-Za-z0-9_]*)|(?:\(NTSTATUS\)\s*)?0[xX][0-9A-Fa-f]+[uUlL]*)'
    r'\s*,\s*'
    r'((?:0[xX][0-9A-Fa-f]+)|(?:\d+))[uUlL]*'
    r'\s*\},?'
)

# Lines of the body are entries, blank, or comments. Every line that starts
# with "{" must parse, otherwise the count and the output would disagree.
entries = []
out_lines = []
for line in body.splitlines():
    stripped = line.strip()
    if not stripped or stripped.startswith(("/*", "*", "//")):
        continue
    if not stripped.startswith("{"):
        if "STATUS_" in stripped or "0x" in stripped:
            print(f"ERROR: cannot parse table entry: {stripped}", file=sys.stderr)
            sys.exit(1)
        continue
    if not entry_re.fullmatch(stripped):
        print(f"ERROR: cannot parse table entry: {stripped}", file=sys.stderr)
        sys.exit(1)
    entries.append(stripped)
    out_lines.append(stripped)
print(f"Found {len(entries)} entries in userspace table")

if args.expect is not None and len(entries) != args.expect:
    print(f"ERROR: expected {args.expect} entries, got {len(entries)}", file=sys.stderr)
    sys.exit(1)

# Build output. Use LF and a POSIX source path so the committed file is the
# same on Windows and Linux.
with open(outp, 'w', encoding='utf-8', newline='\n') as out:
    out.write("/* AUTO-GENERATED - DO NOT EDIT\n")
    out.write(f" * Generated from {inp.as_posix()} by gen_ntstatus_table.py\n")
    out.write(f" * Entries: {len(entries)}\n")
    out.write(f" * Source of truth: src/ntdll/ntstatus_to_dos.c\n")
    out.write(" */\n")
    out.write("\n")
    for line in out_lines:
        out.write(line + "\n")

print(f"Generated {outp} with {len(entries)} entries")
