#!/usr/bin/env python3
"""
gen_ntstatus_table.py - generate kernel NTSTATUS->DOS table from userspace source of truth.

Reads src/ntdll/ntstatus_to_dos.c and emits src/kernel/nt/ntstatus_table.inc
containing the sorted table entries. This ensures the kernel table cannot drift
from the userspace one; any change to the userspace table must be re-generated.

Usage:
  python3 src/kernel/nt/gen_ntstatus_table.py \
    --input src/ntdll/ntstatus_to_dos.c \
    --output src/kernel/nt/ntstatus_table.inc
"""
import re, pathlib, sys, argparse

parser = argparse.ArgumentParser(description="Generate kernel NTSTATUS->DOS table")
parser.add_argument("--input", required=True, help="Input userspace ntstatus_to_dos.c")
parser.add_argument("--output", required=True, help="Output inc file")
args = parser.parse_args()

inp = pathlib.Path(args.input)
outp = pathlib.Path(args.output)

text = inp.read_text()

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

# Extract individual entries: { STATUS_XXX, 123L },
# We keep lines verbatim but verify sorting and parse for validation.
entry_re = re.compile(r'\{\s*([^,]+?)\s*,\s*(\d+)L\s*\},?')
entries = entry_re.findall(body)
print(f"Found {len(entries)} entries in userspace table")

# Verify table is sorted by STATUS value would require evaluating STATUS_ defines,
# but we can at least verify count matches expected 527.
if len(entries) != 527:
    print(f"WARN: Expected 527 entries, got {len(entries)}", file=sys.stderr)
    # Don't fail - allow but warn

# Also extract fallback defines for completeness
# We will emit a file that simply re-exports the array under kernel name
# and includes necessary headers.

# For kernel, we want to generate an .inc that can be included by ntstatus_kernel.c
# The .inc will contain the raw entries suitable for static const array.

# Build output
with open(outp, 'w') as out:
    out.write("/* AUTO-GENERATED - DO NOT EDIT\n")
    out.write(f" * Generated from {inp} by gen_ntstatus_table.py\n")
    out.write(f" * Entries: {len(entries)}\n")
    out.write(f" * Source of truth: src/ntdll/ntstatus_to_dos.c\n")
    out.write(" */\n")
    out.write("\n")
    # Emit entries - we preserve symbolic NTSTATUS names so that header changes propagate
    # The body already contains symbolic names, so we emit as-is but clean up
    # Reconstruct from original body lines that matched entry pattern
    # To preserve comments and formatting, just extract the block again
    # We'll emit the body lines stripped of leading/trailing whitespace variations
    lines = body.strip().splitlines()
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("/*") or stripped.startswith("*") or stripped.startswith("//"):
            continue
        # Keep lines that look like entries
        if "STATUS_" in stripped or "0x" in stripped or stripped.startswith("{"):
            out.write(stripped + "\n")

print(f"Generated {outp} with {len(entries)} entries")
