#!/usr/bin/env python3
"""Write the export list of win32u.dll from the public prototypes in win32k/w32k.h.

    gen_win32u_def.py src/ntdll/win32k/w32k.h out/win32u.def

win32u.dll hosts the one per-process copy of the win32k model. user32.dll and gdi32.dll
import it (decision 0076, addendum), so a DC from GetDC is the same object to gdi32. Every
`w32k_*` function prototype in w32k.h at file scope is exported, in header order.
"""
import pathlib
import re
import sys

PROTO = re.compile(r'^[A-Za-z_][\w \t\*]*?\b(w32k_\w+)\s*\(', re.M)


def exports(header_text):
    """Names of the w32k_* functions declared in the header, in order, without duplicates."""
    text = re.sub(r'/\*.*?\*/', '', header_text, flags=re.S)
    seen = []
    for name in PROTO.findall(text):
        if name not in seen:
            seen.append(name)
    return seen


def render(names):
    return "LIBRARY win32u\nEXPORTS\n" + "".join(f"    {n}\n" for n in names)


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    header = pathlib.Path(argv[1]).read_text()
    pathlib.Path(argv[2]).write_text(render(exports(header)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
