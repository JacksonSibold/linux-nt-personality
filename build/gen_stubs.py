#!/usr/bin/env python3
import re, pathlib, sys
src = pathlib.Path(sys.argv[1])  # syscall_table.def
dst = pathlib.Path(sys.argv[2])  # output .S

text = src.read_text()
entries = re.findall(r'NT_SYSCALL\s*\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+)', text)

with open(dst, 'w') as out:
    out.write("/* auto-generated syscall stubs - do not edit */\n")
    out.write("    .text\n    .align 16\n")
    # Generate each Nt stub with exact 11-byte shape and also Zw alias
    for name, ssn in entries:
        # Nt stub
        out.write(f"    .globl {name}\n")
        # also make Zw alias via .set after defining Nt
        zw = name.replace("Nt", "Zw", 1) if name.startswith("Nt") else "Zw" + name
        # Define Nt label
        out.write(f"{name}:\n")
        out.write(f"    mov %rcx, %r10\n")
        out.write(f"    mov ${ssn}, %eax\n")
        out.write(f"    syscall\n")
        out.write(f"    ret\n")
        # Zw alias: create global alias that points to same address
        out.write(f"    .globl {zw}\n")
        out.write(f"    .set {zw}, {name}\n")
    out.write("\n")

print(f"Generated {len(entries)} stubs to {dst}")
