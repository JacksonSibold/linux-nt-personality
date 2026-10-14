#!/usr/bin/env python3
"""
import_gap.py - measure the gap between the imports of PE files and what this
personality provides.

Given directories or files (for example a game install that the owner has
locally, or a driver package), the tool reads each PE file as bytes, parses its
import directory and its delay-load import directory, groups the imported
functions by DLL, and compares them with:

  user-mode DLLs   the export tables of src/ntdll/*.def (one DLL per file,
                   named by its LIBRARY line or by the file stem); an NT service
                   export (Nt*/Zw*) is a gap when docs/status/SERVICES.md does
                   not classify it IMPLEMENTED
  kernel-mode      ntoskrnl / hal imports of .sys files are checked against the
                   kernel surface generated from src/kernel/nt/*.c (see
                   --write-ntoskrnl-surface)

Nothing is run or loaded. Input bytes are parsed only; no input is copied,
uploaded or executed. Reports are written to docs/status/ as Markdown and JSON.
Import names from the inputs appear in the report, so the owner decides
whether a generated report is committed.

Usage:
  python3 tools/import_gap.py PATH [PATH ...] [--list FILE] [--root REPO]
        [--out-md FILE] [--out-json FILE] [--max-md-functions N]
        [--max-size-mb N] [--no-write]
  python3 tools/import_gap.py --write-ntoskrnl-surface [--root REPO]

PATH may be a file or a directory (walked recursively for PE extensions).
Exit status: 0 report written (or surface written), 1 no PE file could be
read, 2 usage error.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import sys

_HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("pe_info", _HERE / "pe_info.py")
pe_info = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pe_info)

PE_EXTENSIONS = (".exe", ".dll", ".sys", ".ocx", ".cpl", ".drv", ".scr", ".ax")
STRIP_SUFFIXES = (".dll", ".exe", ".drv", ".ocx", ".cpl", ".ax", ".scr")
IMPORT_DIR = 1
DELAY_DIR = 13
DELAY_DESC_COUNT_LIMIT = 4096
GAP_STATUSES = ("missing-dll", "missing-function", "not-implemented", "ordinal")
KERNEL_KEYS = ("ntoskrnl", "hal")
DEFAULT_MAX_SIZE_MB = 1024
DEFAULT_MD_FUNCTIONS = 60

KERNEL_PREFIXES = ("Io", "Mm", "Ke", "Ex", "Ps", "Ob", "Rtl", "Hal", "Fs", "Zw",
                   "Nt", "Se", "Cm", "Po", "Pp", "Wdm", "Fl", "Ci")
KERNEL_DEF_RE = re.compile(
    r"^[A-Za-z_][\w \t\*]*?\b((?:%s)[A-Z]\w*)\s*\(" % "|".join(KERNEL_PREFIXES))
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")

APISET_RE = re.compile(r"^(api-ms-win-|ext-ms-)")
CRYPT_FUNC_RE = re.compile(r"^(Crypt|Cert|CryptoAPI|Bcrypt|Ncrypt)")
DRIVER_DLLS = {"ntoskrnl", "hal", "fltmgr", "wdfldr", "wdf01000", "ndis", "ci",
               "classpnp", "storport", "ksecdd", "cng", "tdi", "netio", "win32k",
               "dxgkrnl", "dxgmms1", "dxgmms2", "videoprt", "watchdog"}
ANTICHEAT_RE = re.compile(r"easyanticheat|beservice|battleye|xigncode|anticheat|vanguard|^vg[ck]$|^eac")
CRYPTO_DLLS = {"bcrypt", "bcryptprimitives", "crypt32", "ncrypt", "cryptsp",
               "cryptbase", "cryptnet", "rsaenh", "wintrust", "schannel",
               "secur32", "sspicli", "cryptui"}
COM_DLLS = {"ole32", "oleaut32", "combase", "rpcrt4", "rpcns4", "propsys"}
GRAPHICS_RE = re.compile(r"^(d3d|dxgi|d2d1|dwrite|dcomp|ddraw|dxva2|d3dx|d3dcompiler|"
                         r"opengl32|vulkan|nvapi|amd_ags|atiadl|dxcore)")
AUDIO_RE = re.compile(r"^(dsound|xaudio2|x3daudio|xapofx|xactengine|winmm|mmdevapi|"
                      r"avrt|audioses|msacm|dsdmo|mfplat|mfreadwrite|wmvcore)")
INPUT_DLLS = {"hid", "hidclass", "wintab32", "rawinput"}
INPUT_RE = re.compile(r"^(dinput|xinput)")
NET_DLLS = {"ws2_32", "wsock32", "mswsock", "winhttp", "wininet", "dnsapi",
            "iphlpapi", "httpapi", "websocket", "webio", "napinsp", "nsi",
            "netapi32", "wsdapi", "wldap32"}
WINDOW_DLLS = {"user32", "gdi32", "gdi32full", "comctl32", "comdlg32", "dwmapi",
               "uxtheme", "imm32", "msctf", "winspool"}
SHELL_DLLS = {"shell32", "shcore", "shlwapi", "shfolder"}
CORE_DLLS = {"kernel32", "kernelbase", "ntdll", "msvcrt", "ucrtbase", "advapi32",
             "version", "setupapi", "psapi", "userenv", "mpr"}
CORE_RE = re.compile(r"^(vcruntime\d+|msvcp\d+)")
APISET_RULES = [
    ("crypto", re.compile(r"crypt|cert|security-cryptoapi")),
    ("com", re.compile(r"winrt|-com-|core-com")),
    ("networking", re.compile(r"winsock|socket|dns|http|-net")),
    ("graphics", re.compile(r"d3d|dxgi|d2d|dwrite|display")),
    ("audio", re.compile(r"audio|multimedia|xaudio")),
    ("input", re.compile(r"-input|hid|xinput")),
    ("shell", re.compile(r"shell")),
    ("window", re.compile(r"window|-ui-|gdi|dwm|uxtheme|winspool")),
]


class ImportGapError(Exception):
    """An input cannot be read as a PE file with a parsable import table."""


def norm_dll(name):
    """Lower-case DLL key without a PE extension (driver .sys keeps its suffix)."""
    key = name.strip().strip('"').lower()
    for suffix in STRIP_SUFFIXES:
        if key.endswith(suffix):
            return key[: -len(suffix)]
    return key


def service_name(func):
    """The NT service a Zw* alias resolves to; other names are returned unchanged."""
    return "Nt" + func[2:] if func.startswith("Zw") else func


# --- PE import parsing -----------------------------------------------------

def read_thunk_names(data, sections, thunk_rva, pe32):
    fmt, size, flag = ("I", 4, 0x80000000) if pe32 else ("Q", 8, 1 << 63)
    off = pe_info.rva_to_offset(sections, thunk_rva)
    if off is None:
        raise pe_info.PEError(f"thunk RVA 0x{thunk_rva:X} is not in file-backed data")
    names = []
    for j in range(pe_info.MAX_THUNKS_PER_DLL):
        (value,) = pe_info.unpack(data, off + j * size, fmt)
        if value == 0:
            return names
        if value & flag:
            names.append(f"#{value & 0xFFFF}")
            continue
        name_off = pe_info.rva_to_offset(sections, value)
        if name_off is None:
            raise pe_info.PEError(f"import name RVA 0x{value:X} is not in file-backed data")
        names.append(pe_info.read_cstring(data, name_off + 2))   # skip the hint word
    raise pe_info.PEError("import thunk table is implausibly long")


def read_import_directory(data, sections, rva, pe32):
    off = pe_info.rva_to_offset(sections, rva)
    if off is None:
        raise pe_info.PEError(f"import directory RVA 0x{rva:X} is not in file-backed data")
    out = []
    for i in range(pe_info.MAX_IMPORT_DESCRIPTORS):
        oft, ts, fc, name_rva, ft = pe_info.unpack(data, off + 20 * i, "5I")
        if not (oft or ts or fc or name_rva or ft):
            return out
        name_off = pe_info.rva_to_offset(sections, name_rva)
        if name_off is None:
            raise pe_info.PEError(f"import name RVA 0x{name_rva:X} is not in file-backed data")
        dll = pe_info.read_cstring(data, name_off)
        for fn in read_thunk_names(data, sections, oft or ft, pe32):
            out.append((dll, fn, False))
    raise pe_info.PEError("too many import descriptors")


def read_delay_directory(data, sections, rva, pe32, image_base):
    off = pe_info.rva_to_offset(sections, rva)
    if off is None:
        raise pe_info.PEError(f"delay-load directory RVA 0x{rva:X} is not in file-backed data")
    out = []
    for i in range(DELAY_DESC_COUNT_LIMIT):
        fields = pe_info.unpack(data, off + 32 * i, "8I")
        attrs, dll_rva, _mh, iat_rva, int_rva = fields[:5]
        if not any(fields):
            return out
        if not (attrs & 1):   # attribute bit 0 clear: the addresses are VAs
            dll_rva = (dll_rva - image_base) & 0xFFFFFFFF
            int_rva = (int_rva - image_base) & 0xFFFFFFFF
        name_off = pe_info.rva_to_offset(sections, dll_rva)
        if name_off is None:
            raise pe_info.PEError(f"delay-load name RVA 0x{dll_rva:X} is not in file-backed data")
        dll = pe_info.read_cstring(data, name_off)
        for fn in read_thunk_names(data, sections, int_rva, pe32):
            out.append((dll, fn, True))
    raise pe_info.PEError("too many delay-load descriptors")


def read_imports(data, info):
    pe32 = info["format"] == "PE32"
    dirs = info["data_dirs"]
    entries = []
    if len(dirs) > IMPORT_DIR and dirs[IMPORT_DIR][0]:
        entries += read_import_directory(data, info["sections"], dirs[IMPORT_DIR][0], pe32)
    if len(dirs) > DELAY_DIR and dirs[DELAY_DIR][0]:
        entries += read_delay_directory(data, info["sections"], dirs[DELAY_DIR][0],
                                        pe32, info["image_base"])
    return entries


def read_module(path, rel, max_bytes):
    if path.stat().st_size > max_bytes:
        raise ImportGapError("larger than --max-size-mb")
    data = path.read_bytes()
    try:
        info = pe_info.parse_pe(data)
        imports = read_imports(data, info)
    except pe_info.PEError as exc:
        raise ImportGapError(str(exc))
    return {
        "path": rel,
        "file": path.name,
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
        "format": info["format"],
        "machine": info["machine"],
        "imports": imports,
    }


# --- personality surface ---------------------------------------------------

def parse_def_exports(text):
    """Return (library, exports) for a .def file, or None when it has no EXPORTS."""
    if not re.search(r"^\s*EXPORTS\b", text, re.M | re.I):
        return None
    lib = re.search(r"^\s*LIBRARY\s+\"?([^\s\"]+)", text, re.M | re.I)
    exports = set()
    in_exports = False
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        head = line.split()[0].upper()
        if head == "LIBRARY":
            continue
        if head == "EXPORTS":
            in_exports = True
            continue
        if in_exports:
            exports.add(line.split()[0].split("=", 1)[0])
    return (lib.group(1) if lib else None), exports


def parse_services(text):
    """Map NT service name to True when every area is IMPLEMENTED."""
    services = {}
    for line in text.splitlines():
        m = re.match(r"^\|\s*0x[0-9a-fA-F]+\s*\|\s*`([A-Za-z0-9_]+)`\s*\|\s*\d+\s*\|\s*(.+?)\s*\|", line)
        if m:
            parts = [p.strip() for p in m.group(2).split("/")]
            services[m.group(1)] = all(p.startswith("IMPLEMENTED") for p in parts)
    return services


def kernel_surface_names(root):
    """Function names defined (not static) in src/kernel/nt/*.c, test sources excluded."""
    names = set()
    for path in sorted((pathlib.Path(root) / "src" / "kernel" / "nt").glob("*.c")):
        if path.name.endswith("_test.c"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        text = LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", text))
        for line in text.splitlines():
            if line.lstrip().startswith("static") or line.rstrip().endswith(";"):
                continue
            m = KERNEL_DEF_RE.match(line)
            if m:
                names.add(m.group(1))
    return names


def load_surface(root):
    root = pathlib.Path(root)
    dlls = {}
    for path in sorted((root / "src" / "ntdll").glob("*.def")):
        parsed = parse_def_exports(path.read_text(encoding="utf-8", errors="replace"))
        if parsed is None:
            continue
        lib, exports = parsed
        key = norm_dll(lib or path.stem)
        dlls[key] = {"display": lib or path.stem,
                     "source": path.relative_to(root).as_posix(), "exports": exports}
    services_path = root / "docs" / "status" / "SERVICES.md"
    services = {}
    if services_path.exists():
        services = parse_services(services_path.read_text(encoding="utf-8", errors="replace"))
    all_exports = set()
    for entry in dlls.values():
        all_exports |= entry["exports"]
    kernel = kernel_surface_names(root)
    return {"dlls": dlls, "all_exports": all_exports, "services": services, "kernel": kernel}


# --- classification --------------------------------------------------------

def dll_category(key):
    if key.endswith(".sys") or key in DRIVER_DLLS or ANTICHEAT_RE.search(key):
        return "anti-cheat/driver"
    if APISET_RE.match(key):
        for name, rule in APISET_RULES:
            if rule.search(key):
                return name
        return "core"
    if key in CRYPTO_DLLS:
        return "crypto"
    if key in COM_DLLS:
        return "com"
    if GRAPHICS_RE.match(key):
        return "graphics"
    if AUDIO_RE.match(key):
        return "audio"
    if key in INPUT_DLLS or INPUT_RE.match(key):
        return "input"
    if key in NET_DLLS:
        return "networking"
    if key in WINDOW_DLLS:
        return "window"
    if key in SHELL_DLLS:
        return "shell"
    if key in CORE_DLLS or CORE_RE.match(key):
        return "core"
    return "other"


def function_category(key, func):
    if key == "advapi32" and CRYPT_FUNC_RE.match(func):
        return "crypto"
    return dll_category(key)


def classify_function(key, func, surface, shipped):
    if key in KERNEL_KEYS:
        if func.startswith("#"):
            return "ordinal"
        return "provided" if func in surface["kernel"] else "missing-function"
    if func.startswith("#"):
        if key in surface["dlls"] or APISET_RE.match(key):
            return "ordinal"
        return "bundled" if key in shipped else "missing-dll"
    if key in surface["dlls"]:
        if func not in surface["dlls"][key]["exports"]:
            return "missing-function"
        if key == "ntdll":
            svc = service_name(func)
            if svc in surface["services"] and not surface["services"][svc]:
                return "not-implemented"
        return "provided"
    if APISET_RE.match(key):
        return "provided" if func in surface["all_exports"] else "missing-function"
    if key in shipped:
        return "bundled"
    return "missing-dll"


def dll_status(key, funcs, surface, shipped):
    if key in surface["dlls"] or key in KERNEL_KEYS or APISET_RE.match(key):
        gaps = [f for f in funcs if f["status"] in GAP_STATUSES]
        return "provided" if not gaps else "partial"
    if key in shipped:
        return "bundled"
    return "missing-dll"


# --- report ----------------------------------------------------------------

def build_report(modules, skipped, surface):
    shipped = {norm_dll(m["file"]) for m in modules}
    funcs = {}        # (key, func) -> {"modules": set, "delay": bool}
    display = {}
    for m in modules:
        for dll, fn, delay in m["imports"]:
            key = norm_dll(dll)
            display.setdefault(key, dll)
            rec = funcs.setdefault((key, fn), {"modules": set(), "delay": False})
            rec["modules"].add(m["path"])
            rec["delay"] = rec["delay"] or delay

    by_dll = {}
    for (key, fn), rec in funcs.items():
        status = classify_function(key, fn, surface, shipped)
        by_dll.setdefault(key, []).append({
            "name": fn, "category": function_category(key, fn), "status": status,
            "modules": len(rec["modules"]), "delay": rec["delay"],
            "_mods": rec["modules"],
        })

    cat_stats = {}
    dll_entries = []
    for key, flist in by_dll.items():
        flist.sort(key=lambda f: (-f["modules"], f["name"]))
        mods = set()
        for f in flist:
            mods |= f.pop("_mods")
        cat_name = dll_category(key)
        status = dll_status(key, flist, surface, shipped)
        gap_count = sum(1 for f in flist if f["status"] in GAP_STATUSES)
        for f in flist:
            cat = cat_stats.setdefault(f["category"], {"dlls": set(), "functions": 0,
                                                       "gap": 0, "modules": set()})
            cat["functions"] += 1
            cat["dlls"].add(key)
            if f["status"] in GAP_STATUSES:
                cat["gap"] += 1
        dll_entries.append({
            "name": display[key], "key": key, "category": cat_name, "status": status,
            "modules": len(mods), "functions_imported": len(flist),
            "functions_gap": gap_count, "functions": flist,
        })

    # modules that import at least one gap function, per function category
    for m in modules:
        seen = set()
        for dll, fn, _delay in m["imports"]:
            key = norm_dll(dll)
            if (key, fn) in seen:
                continue
            seen.add((key, fn))
            status = classify_function(key, fn, surface, shipped)
            if status in GAP_STATUSES:
                cat = cat_stats.setdefault(function_category(key, fn),
                                           {"dlls": set(), "functions": 0, "gap": 0,
                                            "modules": set()})
                cat["modules"].add(m["path"])
    dll_entries.sort(key=lambda e: (-e["modules"], -e["functions_gap"], e["key"]))
    for rank, entry in enumerate(dll_entries, 1):
        entry["rank"] = rank
        entry["name"] = display[entry["key"]]

    categories = []
    for name, cat in cat_stats.items():
        categories.append({"category": name, "dlls": len(cat["dlls"]),
                           "functions": cat["functions"], "gap_functions": cat["gap"],
                           "modules_with_gap": len(cat["modules"])})
    categories.sort(key=lambda c: (-c["gap_functions"], c["category"]))

    total_funcs = sum(e["functions_imported"] for e in dll_entries)
    gap_funcs = sum(e["functions_gap"] for e in dll_entries)
    summary = {
        "modules_read": len(modules),
        "modules_skipped": len(skipped),
        "dlls": len(dll_entries),
        "dlls_provided": sum(1 for e in dll_entries if e["status"] == "provided"),
        "dlls_partial": sum(1 for e in dll_entries if e["status"] == "partial"),
        "dlls_missing": sum(1 for e in dll_entries if e["status"] == "missing-dll"),
        "dlls_bundled": sum(1 for e in dll_entries if e["status"] == "bundled"),
        "functions": total_funcs,
        "functions_gap": gap_funcs,
        "functions_delay_load": sum(1 for e in dll_entries for f in e["functions"] if f["delay"]),
    }
    return {
        "schema": 1,
        "generator": "tools/import_gap.py",
        "inputs": [{k: m[k] for k in ("path", "format", "machine", "size", "sha256")}
                   | {"imports": len(m["imports"]),
                      "delay_imports": sum(1 for x in m["imports"] if x[2])}
                   for m in modules],
        "skipped": skipped,
        "summary": summary,
        "categories": categories,
        "dlls": dll_entries,
    }


def _cell(text):
    return str(text).replace("|", "\\|")


def render_markdown(report, max_functions):
    s = report["summary"]
    lines = [
        "# Game import gap",
        "",
        "<!-- Generated by tools/import_gap.py. Do not edit by hand. -->",
        "",
        "This report measures how far the personality's exports (`src/ntdll/*.def`),",
        "its NT service classes (`docs/status/SERVICES.md`) and its kernel surface",
        "(`src/kernel/nt/*.c`, see `docs/status/NTOSKRNL-SURFACE.md`) fall short of the",
        "imports of the PE files it was given. It is a measuring stick, not a",
        "compatibility result: an export that exists may still be incomplete, and a",
        "name match says nothing about behaviour.",
        "",
        "## Summary",
        "",
        "| Measure | Value |",
        "|---|---:|",
        f"| Modules read | {s['modules_read']} |",
        f"| Modules skipped | {s['modules_skipped']} |",
        f"| DLLs and driver modules imported | {s['dlls']} |",
        f"| provided | {s['dlls_provided']} |",
        f"| partial | {s['dlls_partial']} |",
        f"| missing | {s['dlls_missing']} |",
        f"| bundled with the inputs | {s['dlls_bundled']} |",
        f"| Unique functions imported | {s['functions']} |",
        f"| Functions in the gap | {s['functions_gap']} |",
        f"| Delay-load functions | {s['functions_delay_load']} |",
        "",
        "## Categories",
        "",
        "| Category | DLLs | Functions | Gap functions | Modules with a gap |",
        "|---|---:|---:|---:|---:|",
    ]
    for c in report["categories"]:
        lines.append(f"| {_cell(c['category'])} | {c['dlls']} | {c['functions']} | "
                     f"{c['gap_functions']} | {c['modules_with_gap']} |")
    gap_dlls = [e for e in report["dlls"] if e["status"] in ("missing-dll", "partial")]
    lines += [
        "",
        "## Missing and partial DLLs, ranked by importing modules",
        "",
        "| Rank | DLL | Category | Status | Importing modules | Functions | Gap |",
        "|---:|---|---|---|---:|---:|---:|",
    ]
    for e in gap_dlls:
        lines.append(f"| {e['rank']} | {_cell(e['name'])} | {e['category']} | {e['status']} | "
                     f"{e['modules']} | {e['functions_imported']} | {e['functions_gap']} |")
    if not gap_dlls:
        lines.append("| - | none | - | - | - | - | - |")
    lines += ["", "## Functions in the gap, by DLL", ""]
    for e in gap_dlls:
        gap = [f for f in e["functions"] if f["status"] in GAP_STATUSES]
        lines += [f"### {e['rank']}. `{e['name']}` ({e['category']}, {e['status']}, "
                  f"{e['modules']} module(s))", "",
                  "| Function | Category | Status | Modules | Delay-load |",
                  "|---|---|---|---:|---|"]
        for f in gap[:max_functions]:
            lines.append(f"| `{_cell(f['name'])}` | {f['category']} | {f['status']} | "
                         f"{f['modules']} | {'yes' if f['delay'] else 'no'} |")
        if len(gap) > max_functions:
            lines.append(f"| ... | {len(gap) - max_functions} more in the JSON report | | | |")
        lines.append("")
    bundled = [e["name"] for e in report["dlls"] if e["status"] == "bundled"]
    lines += ["## Bundled DLLs", "",
              "Imported DLLs that the personality does not provide but that are among the",
              "inputs. They are assumed to ship with the software and are left out of the gap.", ""]
    lines += [f"- `{_cell(n)}`" for n in bundled] or ["- none"]
    lines += ["", "## Skipped inputs", ""]
    lines += [f"- `{_cell(x['path'])}`: {_cell(x['reason'])}" for x in report["skipped"]] or ["- none"]
    lines += ["", "## Input files", "",
              "| File | Format | Machine | Imports | Delay-load | SHA-256 |",
              "|---|---|---|---:|---:|---|"]
    for i in report["inputs"]:
        lines.append(f"| {_cell(i['path'])} | {i['format']} | {i['machine']} | {i['imports']} | "
                     f"{i['delay_imports']} | `{i['sha256']}` |")
    lines += ["", "## Method and limits", "",
              "- Import names come from the import directory and the delay-load directory.",
              "  Ordinal-only imports are reported as `#n` with status `ordinal`.",
              "- A DLL is `provided` when its `src/ntdll/*.def` file exports the name. An",
              "  `Nt*`/`Zw*` export is `not-implemented` when SERVICES.md does not classify",
              "  the service IMPLEMENTED in every area.",
              "- Kernel-mode `ntoskrnl`/`hal` imports are checked against names defined",
              "  non-statically in `src/kernel/nt/*.c`. A definition is not an ABI match.",
              "- API-set DLLs (`api-ms-win-*`, `ext-ms-*`) are checked against the union of",
              "  all exports, which ignores which DLL the loader would actually pick.",
              "- A DLL that is among the inputs and not provided is `bundled`, not a gap.",
              "  Do not include system DLLs as inputs when measuring the system gap.",
              "- Only the import tables are read. Resources, forwarders and runtime loading",
              "  are not modelled.", ""]
    return "\n".join(lines)


def render_surface(names):
    groups = {}
    for name in sorted(names):
        for prefix in KERNEL_PREFIXES:
            if name.startswith(prefix):
                groups.setdefault(prefix, []).append(name)
                break
    lines = [
        "# Kernel surface (ntoskrnl-style names)",
        "",
        "<!-- Generated by tools/import_gap.py --write-ntoskrnl-surface. Do not edit by hand. -->",
        "",
        "Names defined without `static` in `src/kernel/nt/*.c` (test sources excluded,",
        "comments removed) that match the ntoskrnl naming families. These are what the",
        "kernel side of the personality defines, not what a Windows kernel exports. A",
        "driver import is `provided` only when it appears here.",
        "",
        f"Total: {len(names)} names.",
        "",
        "| Prefix | Names |",
        "|---|---:|",
    ]
    for prefix in KERNEL_PREFIXES:
        if prefix in groups:
            lines.append(f"| {prefix} | {len(groups[prefix])} |")
    lines.append("")
    for prefix in KERNEL_PREFIXES:
        if prefix in groups:
            lines += [f"## {prefix}", ""]
            lines += [f"- `{n}`" for n in groups[prefix]]
            lines.append("")
    return "\n".join(lines)


# --- command line ----------------------------------------------------------

def collect(paths, list_file):
    raw = list(paths)
    if list_file:
        for line in pathlib.Path(list_file).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                raw.append(line)
    items, seen = [], set()
    for p in raw:
        path = pathlib.Path(p)
        if path.is_dir():
            for dirpath, dirnames, filenames in os.walk(path):
                dirnames.sort()
                for f in sorted(filenames):
                    full = pathlib.Path(dirpath) / f
                    if f.lower().endswith(PE_EXTENSIONS):
                        rel = pathlib.PurePath(os.path.relpath(full, path)).as_posix()
                        items.append((full, rel))
        elif path.is_file():
            items.append((path, path.name))
        else:
            raise SystemExit(f"import_gap: error: no such file or directory: {p}")
    unique = []
    for full, rel in items:
        key = str(full.resolve()).lower()
        if key not in seen:
            seen.add(key)
            unique.append((full, rel))
    return unique


def write_outputs(report, md_path, json_path, max_functions):
    for out in (md_path, json_path):
        out.parent.mkdir(parents=True, exist_ok=True)
    md_path.write_text(render_markdown(report, max_functions), encoding="utf-8")
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main(argv=None):
    default_root = _HERE.parent
    parser = argparse.ArgumentParser(description="Import gap of PE files against this personality.")
    parser.add_argument("paths", nargs="*", help="PE files or directories to read")
    parser.add_argument("--list", dest="list_file", help="text file with one path per line")
    parser.add_argument("--root", default=str(default_root), help="repository root")
    parser.add_argument("--out-md", help="Markdown report (default docs/status/GAME-IMPORT-GAP.md)")
    parser.add_argument("--out-json", help="JSON report (default docs/status/GAME-IMPORT-GAP.json)")
    parser.add_argument("--max-md-functions", type=int, default=DEFAULT_MD_FUNCTIONS,
                        help="functions listed per DLL in the Markdown report")
    parser.add_argument("--max-size-mb", type=int, default=DEFAULT_MAX_SIZE_MB,
                        help="skip inputs larger than this")
    parser.add_argument("--no-write", action="store_true", help="print the summary only")
    parser.add_argument("--write-ntoskrnl-surface", action="store_true",
                        help="write docs/status/NTOSKRNL-SURFACE.md from src/kernel/nt and exit")
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    if args.write_ntoskrnl_surface:
        names = kernel_surface_names(root)
        out = root / "docs" / "status" / "NTOSKRNL-SURFACE.md"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(render_surface(names), encoding="utf-8")
        print(f"import_gap: wrote {out.relative_to(root).as_posix()} ({len(names)} names)")
        return 0

    if not args.paths and not args.list_file:
        parser.error("give at least one PATH or --list FILE")
    items = collect(args.paths, args.list_file)
    max_bytes = args.max_size_mb * 1024 * 1024
    modules, skipped = [], []
    for full, rel in items:
        try:
            modules.append(read_module(full, rel, max_bytes))
        except (ImportGapError, OSError) as exc:
            skipped.append({"path": rel, "reason": str(exc)})
    if not modules:
        print("import_gap: error: no PE file could be read", file=sys.stderr)
        return 1

    surface = load_surface(root)
    report = build_report(modules, skipped, surface)
    if not args.no_write:
        md = pathlib.Path(args.out_md) if args.out_md else root / "docs" / "status" / "GAME-IMPORT-GAP.md"
        js = pathlib.Path(args.out_json) if args.out_json else root / "docs" / "status" / "GAME-IMPORT-GAP.json"
        write_outputs(report, md, js, args.max_md_functions)
    s = report["summary"]
    print(f"import_gap: {s['modules_read']} module(s) read, {s['modules_skipped']} skipped; "
          f"{s['functions_gap']} of {s['functions']} unique function(s) in the gap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
