#!/usr/bin/env python3
"""
pe_info.py - print a summary of a PE (Portable Executable) file.

Shows the machine type, PE32 or PE32+ format, timestamp, subsystem, image
base, entry point, section table, DLL characteristics and the import summary
(imported DLLs and function count). Parsing is bounds-checked: a malformed or
truncated file produces an error message, not a traceback.

Usage:
  python3 tools/pe_info.py FILE
  python3 tools/pe_info.py --selftest     # build a fixture PE in memory and check the parser

Exit status: 0 success, 1 not a readable PE file, 2 usage error.
"""
import argparse
import os
import struct
import sys
import time

MAX_SECTIONS = 96            # Windows loader limit
MAX_IMPORT_DESCRIPTORS = 4096
MAX_THUNKS_PER_DLL = 1 << 16

MACHINES = {
    0x014C: "x86 (i386)",
    0x0200: "IA-64",
    0x01C4: "ARM Thumb-2",
    0x8664: "x86-64 (AMD64)",
    0xAA64: "ARM64",
}
SUBSYSTEMS = {
    1: "native", 2: "windows gui", 3: "windows console", 5: "os/2 cui",
    7: "posix cui", 9: "windows ce gui", 10: "efi application",
    11: "efi boot service driver", 12: "efi runtime driver", 14: "xbox",
}
FILE_FLAGS = [(0x0002, "EXECUTABLE_IMAGE"), (0x0020, "LARGE_ADDRESS_AWARE"),
              (0x0100, "32BIT_MACHINE"), (0x2000, "DLL")]
DLL_FLAGS = [(0x0040, "ASLR"), (0x0100, "NX"), (0x0400, "NO_SEH"),
             (0x4000, "CFG")]


class PEError(Exception):
    pass


def unpack(data, off, fmt):
    """struct.unpack_from with a bounds check (fmt uses little-endian)."""
    size = struct.calcsize("<" + fmt)
    if off < 0 or off + size > len(data):
        raise PEError(f"truncated file: need {size} bytes at offset 0x{off:X}")
    return struct.unpack_from("<" + fmt, data, off)


def read_cstring(data, off, limit=512):
    end = data.find(b"\x00", off, off + limit)
    if end < 0:
        raise PEError(f"unterminated string at offset 0x{off:X}")
    return data[off:end].decode("ascii", "replace")


def flags_text(value, table):
    names = [name for bit, name in table if value & bit]
    return " ".join(names) if names else "-"


def rva_to_offset(sections, rva):
    """File offset for an RVA, or None if it falls outside raw section data."""
    for s in sections:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
            delta = rva - s["va"]
            if delta < s["rawsize"]:
                return s["rawptr"] + delta
            return None
    return None


def parse_imports(data, sections, rva, pe32):
    """Return a list of (dll_name, function_count) from the import directory."""
    off = rva_to_offset(sections, rva)
    if off is None:
        raise PEError(f"import directory RVA 0x{rva:X} is not in file-backed section data")
    thunk_fmt, thunk_size = ("I", 4) if pe32 else ("Q", 8)
    dlls = []
    for i in range(MAX_IMPORT_DESCRIPTORS):
        oft, _ts, _fc, name_rva, ft = unpack(data, off + 20 * i, "5I")
        if not (oft or _ts or _fc or name_rva or ft):
            break
        name_off = rva_to_offset(sections, name_rva)
        if name_off is None:
            raise PEError(f"import name RVA 0x{name_rva:X} is not in file-backed data")
        name = read_cstring(data, name_off)
        thunk_off = rva_to_offset(sections, oft or ft)
        if thunk_off is None:
            raise PEError(f"thunk RVA for {name} is not in file-backed data")
        count = 0
        for j in range(MAX_THUNKS_PER_DLL):
            (value,) = unpack(data, thunk_off + j * thunk_size, thunk_fmt)
            if value == 0:
                break
            count += 1
        else:
            raise PEError(f"import table for {name} is implausibly long")
        dlls.append((name, count))
    else:
        raise PEError("too many import descriptors")
    return dlls


def parse_pe(data):
    """Parse PE headers from bytes. Returns a dict; raises PEError."""
    if data[:2] != b"MZ":
        raise PEError("not a PE file: missing MZ signature")
    (e_lfanew,) = unpack(data, 0x3C, "I")
    if unpack(data, e_lfanew, "4s")[0] != b"PE\x00\x00":
        raise PEError(f"not a PE file: missing PE signature at 0x{e_lfanew:X}")

    coff = e_lfanew + 4
    machine, nsec, timestamp, _symptr, _nsyms, opt_size, characteristics = \
        unpack(data, coff, "HHIIIHH")
    if nsec > MAX_SECTIONS:
        raise PEError(f"implausible section count {nsec}")

    opt = coff + 20
    (magic,) = unpack(data, opt, "H")
    if magic == 0x10B:
        pe32 = True
        dirs_off = opt + 96
        (num_dirs,) = unpack(data, opt + 92, "I")
        (image_base,) = unpack(data, opt + 28, "I")
    elif magic == 0x20B:
        pe32 = False
        dirs_off = opt + 112
        (num_dirs,) = unpack(data, opt + 108, "I")
        (image_base,) = unpack(data, opt + 24, "Q")
    else:
        raise PEError(f"unknown optional header magic 0x{magic:04X}")

    (entry_rva, _basecode) = unpack(data, opt + 16, "II")
    (size_of_image,) = unpack(data, opt + 56, "I")
    subsystem, dll_flags = unpack(data, opt + 68, "HH")

    dirs = []
    for k in range(min(num_dirs, 16)):
        dirs.append(unpack(data, dirs_off + 8 * k, "II"))
    import_rva, import_size = dirs[1] if len(dirs) > 1 else (0, 0)

    sections_off = opt + opt_size
    sections = []
    for k in range(nsec):
        (name, vsize, va, rawsize, rawptr, _r1, _r2, _n1, _n2, chars) = \
            unpack(data, sections_off + 40 * k, "8sIIIIIIHHI")
        sections.append({
            "name": name.rstrip(b"\x00").decode("ascii", "replace"),
            "vsize": vsize, "va": va, "rawsize": rawsize,
            "rawptr": rawptr, "chars": chars,
        })

    info = {
        "format": "PE32" if pe32 else "PE32+",
        "machine": MACHINES.get(machine, "unknown"),
        "machine_id": machine,
        "timestamp": timestamp,
        "characteristics": characteristics,
        "subsystem": SUBSYSTEMS.get(subsystem, "unknown"),
        "subsystem_id": subsystem,
        "image_base": image_base,
        "entry_rva": entry_rva,
        "size_of_image": size_of_image,
        "dll_flags": dll_flags,
        "sections": sections,
        "imports": None,
        "import_note": None,
    }
    if import_rva == 0:
        info["imports"] = []
    else:
        try:
            info["imports"] = parse_imports(data, sections, import_rva, pe32)
        except PEError as exc:
            info["import_note"] = str(exc)
    return info


def format_info(path, size, info):
    lines = []
    add = lines.append
    add(f"file:            {path}")
    add(f"size:            {size} bytes")
    add(f"format:          {info['format']}")
    add(f"machine:         {info['machine']} (0x{info['machine_id']:04X})")
    stamp = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(info["timestamp"]))
    add(f"timestamp:       {stamp} (raw {info['timestamp']})")
    add(f"characteristics: {flags_text(info['characteristics'], FILE_FLAGS)}")
    add(f"subsystem:       {info['subsystem']} ({info['subsystem_id']})")
    add(f"image base:      0x{info['image_base']:X}")
    entry = info["entry_rva"]
    if entry:
        add(f"entry point:     RVA 0x{entry:X} (VA 0x{info['image_base'] + entry:X})")
    else:
        add("entry point:     none (0)")
    add(f"image size:      0x{info['size_of_image']:X}")
    add(f"dll flags:       {flags_text(info['dll_flags'], DLL_FLAGS)}")
    add(f"sections ({len(info['sections'])}):")
    add("  name      vsize      va         rawsize    rawptr     perms")
    for s in info["sections"]:
        c = s["chars"]
        perms = ("r" if c & 0x40000000 else "-") + ("w" if c & 0x80000000 else "-") \
            + ("x" if c & 0x20000000 else "-")
        add(f"  {s['name']:<8}  0x{s['vsize']:08X} 0x{s['va']:08X} "
            f"0x{s['rawsize']:08X} 0x{s['rawptr']:08X} {perms}")
    if info["imports"] is None:
        add(f"imports:         unavailable ({info['import_note']})")
    else:
        dlls = info["imports"]
        total = sum(n for _, n in dlls)
        add(f"imports:         {len(dlls)} DLL(s), {total} function(s)")
        for name, n in dlls:
            add(f"  {name}: {n}")
    return "\n".join(lines)


def build_fixture(pe32=False):
    """Build a tiny valid PE (PE32+ by default) that imports two functions."""
    ptr_fmt, ptr_size = ("I", 4) if pe32 else ("Q", 8)
    opt_fixed = 96 if pe32 else 112
    opt_size = opt_fixed + 16 * 8
    e_lfanew = 0x40
    sec_table = e_lfanew + 4 + 20 + opt_size

    if pe32:
        opt_head = struct.pack("<HBBIIIII", 0x10B, 14, 0, 0x200, 0x200, 0, 0x1000, 0x1000)
        opt_head += struct.pack("<II", 0x400000, 0x1000)          # BaseOfData, ImageBase
    else:
        opt_head = struct.pack("<HBBIIIII", 0x20B, 14, 0, 0x200, 0x200, 0, 0x1000, 0x1000)
        opt_head += struct.pack("<Q", 0x140000000)                # ImageBase
    opt_tail = struct.pack("<IIHHHHHHIIIIHH", 0x1000, 0x200, 6, 0, 0, 0, 6, 0,
                           0, 0x3000, 0x200, 0, 3, 0x0140)
    stack_heap = struct.pack("<" + ptr_fmt * 4, 0x100000, 0x1000, 0x100000, 0x1000)
    loader_and_count = struct.pack("<II", 0, 16)
    opt = opt_head + opt_tail + stack_heap + loader_and_count
    dirs = bytearray(16 * 8)
    struct.pack_into("<II", dirs, 8, 0x2000, 40)                  # import directory
    opt = opt + bytes(dirs)
    assert len(opt) == opt_size, (len(opt), opt_size)

    # .idata: import directory at RVA 0x2000 (raw 0x400)
    idata = bytearray(0x200)
    ilt_rva, iat_rva, name_rva = 0x2040, 0x2060, 0x2080
    hint1_rva, hint2_rva = 0x2090, 0x20A0
    struct.pack_into("<5I", idata, 0, ilt_rva, 0, 0, name_rva, iat_rva)  # descriptor 0
    # descriptor 1 is all zeros (terminator)
    thunks = [hint1_rva, hint2_rva, 0]
    for base in (ilt_rva, iat_rva):
        for k, v in enumerate(thunks):
            struct.pack_into("<" + ptr_fmt, idata, base - 0x2000 + k * ptr_size, v)
    idata[name_rva - 0x2000:name_rva - 0x2000 + 13] = b"KERNEL32.dll\x00"
    idata[hint1_rva - 0x2000:hint1_rva - 0x2000 + 2 + 12] = b"\x00\x00ExitProcess\x00"
    idata[hint2_rva - 0x2000:hint2_rva - 0x2000 + 2 + 10] = b"\x00\x00WriteFile\x00"

    text = bytes([0xCC]) * 0x200
    headers = bytearray(0x200)
    headers[0:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, e_lfanew)
    headers[e_lfanew:e_lfanew + 4] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", headers, e_lfanew + 4,
                     0x14C if pe32 else 0x8664, 2, 1700000000, 0, 0, opt_size, 0x0022)
    headers[e_lfanew + 24:e_lfanew + 24 + opt_size] = opt
    sec = struct.pack("<8sIIIIIIHHI", b".text", 0x20, 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0x60000020)
    sec += struct.pack("<8sIIIIIIHHI", b".idata", 0x100, 0x2000, 0x200, 0x400, 0, 0, 0, 0, 0xC0000040)
    headers[sec_table:sec_table + len(sec)] = sec
    return bytes(headers) + text + bytes(idata)


def selftest():
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    def expect_error(data, needle, label):
        try:
            parse_pe(data)
        except PEError as exc:
            check(needle in str(exc), f"{label}: wrong message {exc}")
        else:
            failures.append(f"{label}: no error raised")

    for pe32 in (False, True):
        tag = "PE32" if pe32 else "PE32+"
        info = parse_pe(build_fixture(pe32))
        check(info["format"] == tag, f"{tag}: format {info['format']}")
        check(info["machine_id"] == (0x14C if pe32 else 0x8664), f"{tag}: machine")
        check(info["subsystem"] == "windows console", f"{tag}: subsystem")
        check(info["entry_rva"] == 0x1000, f"{tag}: entry")
        check([s["name"] for s in info["sections"]] == [".text", ".idata"], f"{tag}: sections")
        check(info["imports"] == [("KERNEL32.dll", 2)], f"{tag}: imports {info['imports']}")
        check(info["import_note"] is None, f"{tag}: unexpected import note")
        check(bool(info["dll_flags"] & 0x40), f"{tag}: ASLR flag")
        text = format_info("fixture", 0, info)
        check("KERNEL32.dll: 2" in text, f"{tag}: formatted imports")

    good = build_fixture()
    expect_error(b"ZZ" + good[2:], "missing MZ", "bad magic")
    expect_error(good[:0x30], "truncated", "truncated header")
    expect_error(good[:0x100], "truncated", "truncated optional header")
    bad_pe = bytearray(good)
    bad_pe[0x40:0x44] = b"XX\x00\x00"
    expect_error(bytes(bad_pe), "missing PE signature", "bad PE signature")
    bad_magic = bytearray(good)
    bad_magic[0x58:0x5A] = struct.pack("<H", 0x1234)
    expect_error(bytes(bad_magic), "unknown optional header", "bad optional magic")

    # Import directory pointing outside file-backed data: parse succeeds, note set.
    no_imp = bytearray(good)
    import_dir_entry = 0x40 + 4 + 20 + 112 + 8   # PE32+ DataDirectory[1].VirtualAddress
    struct.pack_into("<I", no_imp, import_dir_entry, 0x9000)
    info = parse_pe(bytes(no_imp))
    check(info["imports"] is None and info["import_note"], "unreadable imports noted")

    if failures:
        for f in failures:
            print(f"selftest FAIL: {f}", file=sys.stderr)
        return 1
    print("selftest: all PE parser checks passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Summarize a PE file's headers and imports.")
    parser.add_argument("path", nargs="?", help="PE file to inspect")
    parser.add_argument("--selftest", action="store_true",
                        help="run parser checks on built-in fixtures and exit")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.path:
        parser.error("a file path is required (or use --selftest)")

    try:
        with open(args.path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        print(f"pe_info: error: cannot read {args.path}: {exc.strerror}", file=sys.stderr)
        return 1
    try:
        info = parse_pe(data)
    except PEError as exc:
        print(f"pe_info: error: {args.path}: {exc}", file=sys.stderr)
        return 1
    print(format_info(os.path.basename(args.path), len(data), info))
    return 0


if __name__ == "__main__":
    sys.exit(main())
