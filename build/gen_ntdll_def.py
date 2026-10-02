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
    for exp in ["LdrLoadDll", "LdrUnloadDll", "LdrGetDllHandle", "LdrGetProcedureAddress",
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
                "NtRaiseException", "_NtRaiseException",
                "KiUserExceptionDispatcher", "KiUserApcDispatcher",
                "NtCurrentTeb"]:
        out.write(f"    {exp}\n")
    # Misc required for mingw
    out.write(f"    DllMain\n")
    out.write(f"    _DllMainCRTStartup@12\n")
    out.write(f"    DllMainCRTStartup\n")

print(f"Generated {out_def} with {len(entries)*2 + 20} exports")
