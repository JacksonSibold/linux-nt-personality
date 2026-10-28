#!/usr/bin/env python3
import re, pathlib, sys

def_file = pathlib.Path(sys.argv[1])  # syscall_table.def
out_def = pathlib.Path(sys.argv[2])   # ntdll.def

text = def_file.read_text()
entries = re.findall(r'NT_SYSCALL\s*\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+)', text)

with open(out_def, 'w') as out:
    out.write("LIBRARY ntdll\nEXPORTS\n")
    # Nt/Zw pairs
    for name, ssn in entries:
        zw = name.replace("Nt", "Zw", 1) if name.startswith("Nt") else "Zw"+name
        out.write(f"    {name}\n")
        out.write(f"    {zw}={name}\n")
    # Ldr*
    for exp in ["LdrLoadDll", "LdrUnloadDll", "LdrGetDllHandle", "LdrGetProcedureAddress", "LdrAddRefDll",
                "LdrInitializeThunk"]:
        out.write(f"    {exp}\n")
    # Rtl*
    for exp in ["RtlNtStatusToDosError", "RtlGetVersion",
                "RtlVirtualUnwind", "RtlLookupFunctionEntry", "RtlLookupFunctionEntryEx",
                "RtlCaptureContext", "RtlUnwindEx", "RtlRaiseException",
                "RtlAddFunctionTable", "RtlDeleteFunctionTable",
                "RtlDispatchException", "RtlDispatchExceptionWrapper"]:
        out.write(f"    {exp}\n")
    # Exception / VEH
    for exp in ["AddVectoredExceptionHandler", "RemoveVectoredExceptionHandler",
                "AddVectoredContinueHandler", "RemoveVectoredContinueHandler",
                "SetUnhandledExceptionFilter", "UnhandledExceptionFilter",
                "_NtRaiseException",
                "KiUserExceptionDispatcher", "KiUserApcDispatcher",
                "NtCurrentTeb"]:
        out.write(f"    {exp}\n")
    # Misc required for mingw
    out.write(f"    DllMain\n")
    out.write(f"    _DllMainCRTStartup\n")
    out.write(f"    DllMainCRTStartup\n")

# Further exports, one per line in ntdll_exports.txt next to the syscall table. A line is `name` or
# `name=target` (an alias of another export); text after # is a comment.
extra_file = def_file.parent / "ntdll_exports.txt"
if extra_file.exists():
    seen = set()
    with open(out_def, "a") as out:
        for raw in extra_file.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            name = line.split("=", 1)[0].strip()
            if name in seen:
                sys.exit(f"{extra_file}: {name} is listed twice")
            seen.add(name)
            out.write(f"    {line}\n")

# Count the export lines actually written, so the printed total cannot drift
with open(out_def) as check:
    written = sum(1 for line in check if line.startswith("    "))
print(f"Generated {out_def} with {written} exports")
