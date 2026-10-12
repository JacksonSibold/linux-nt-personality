#!/usr/bin/env python3
"""
service_inventory.py - classify the handler of every NT system service.

The service table is src/kernel/nt/nt_service_list.h, one line per service:

    NT_SERVICE(0x0046, NtYieldExecution, 0)

For each service the handler definitions in src/kernel/nt/*.c are looked up
as `NTSTATUS nt_svc_<Name>(...) { ... }`. Test sources (*_test.c) and
prototypes (no body) are ignored. The result is one of:

  IMPLEMENTED           a strong definition with a real body
  STUB-NOT-IMPLEMENTED  a strong definition whose body is nothing but
                        `return STATUS_NOT_IMPLEMENTED;` or
                        `return STATUS_NOT_SUPPORTED;` (casts of unused
                        arguments to void are allowed)
  WEAK-FALLBACK         only a `__attribute__((weak))` definition exists, so
                        the table entry is the weak fallback
  MISSING               no definition at all; the table entry is NULL

Linker rules apply: a strong definition overrides a weak one, so a service
with both is classified by its strong definition. Only the first body that
ends in `}` is read, with comments and string literals removed first.

Each service is placed in an area chosen from its name (first matching
pattern wins). The area is only for grouping in the report.

Usage:
  python3 tools/service_inventory.py                    summary on stdout
  python3 tools/service_inventory.py --markdown         Markdown report on stdout
  python3 tools/service_inventory.py --markdown --out docs/status/SERVICES.md
  python3 tools/service_inventory.py --root PATH        scan another tree
  python3 tools/service_inventory.py --selftest         built-in fixtures

Exit status: 0 success, 1 a source file cannot be read or the selftest fails,
2 usage error. Standard library only; nothing in the tree is modified unless
--out is given.
"""
import argparse
import os
import re
import sys
import tempfile

SERVICE_LIST = "src/kernel/nt/nt_service_list.h"
HANDLER_DIR = "src/kernel/nt"

IMPLEMENTED = "IMPLEMENTED"
STUB = "STUB-NOT-IMPLEMENTED"
WEAK = "WEAK-FALLBACK"
MISSING = "MISSING"
STATUSES = (IMPLEMENTED, STUB, WEAK, MISSING)

SERVICE_RE = re.compile(
    r"^\s*NT_SERVICE\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*(\w+)\s*,\s*(\d+)\s*\)\s*$",
    re.M)
DEF_RE = re.compile(
    r"(?P<attr>__attribute__\s*\(\(\s*weak\s*\)\)\s*)?"
    r"\bNTSTATUS\s+nt_svc_(?P<name>\w+)\s*\(", re.S)
VOID_CAST_RE = re.compile(r"\(\s*void\s*\)\s*[\w.\[\]]+\s*;")
STUB_RETURN_RE = re.compile(
    r"^return\s+(STATUS_NOT_IMPLEMENTED|STATUS_NOT_SUPPORTED)\s*;$")

# Ordered: the first pattern that matches the service name gives the area.
AREA_PATTERNS = (
    ("system", r"TimerResolution|SystemInformation|SystemTime|PerformanceCounter|"
               r"DelayExecution|YieldExecution|RtlNtStatusToDosError"),
    ("process-thread", r"Process|Thread|Job|Debug|Context|Apc|Alert|Continue|"
                       r"RaiseException|RaiseHardError"),
    ("memory", r"VirtualMemory|Section"),
    ("io", r"File|Volume|EaFile|Directory|SymbolicLink|Device|FsControl|Cancel"),
    ("sync", r"Event|Mutant|Semaphore|Timer|Keyed|IoCompletion|WaitFor|"
             r"SignalAndWait"),
    ("security", r"Token|Privilege|AccessCheck|Security|AuditAlarm"),
    ("registry", r"Key|Value"),
    ("ports", r"Port"),
    ("object", r"Object|Close|Duplicate|Temporary|Permanent"),
)
AREA_RES = tuple((area, re.compile(pat)) for area, pat in AREA_PATTERNS)
OTHER_AREA = "other"


def strip_comments_and_strings(text):
    """Blank out comments and string/char literals, keeping line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c == "/" and nxt == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * max(0, j - i - 2) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_close(text, open_idx, open_ch, close_ch):
    """Index of the bracket matching text[open_idx], or -1."""
    depth = 0
    for k in range(open_idx, len(text)):
        if text[k] == open_ch:
            depth += 1
        elif text[k] == close_ch:
            depth -= 1
            if depth == 0:
                return k
    return -1


def body_is_stub(body):
    """True when the body only casts arguments to void and returns a stub status."""
    rest = VOID_CAST_RE.sub("", body)
    stmts = [s.strip() for s in rest.split(";") if s.strip()]
    if len(stmts) != 1:
        return False
    return bool(STUB_RETURN_RE.match(stmts[0] + ";"))


def scan_definitions(src_text):
    """Yield (name, is_weak, body_or_None) for each nt_svc_ definition in one file."""
    text = strip_comments_and_strings(src_text)
    for m in DEF_RE.finditer(text):
        paren = text.find("(", m.end() - 1)
        close_paren = match_close(text, paren, "(", ")")
        if close_paren < 0:
            continue
        after = close_paren + 1
        while after < len(text) and text[after] in " \t\r\n":
            after += 1
        if after >= len(text) or text[after] != "{":
            continue  # a prototype, not a definition
        close_brace = match_close(text, after, "{", "}")
        if close_brace < 0:
            continue
        body = text[after + 1:close_brace]
        yield m.group("name"), bool(m.group("attr")), body


def collect_handlers(root):
    """Map service name -> {"strong": (file, stub?) or None, "weak": file or None}."""
    handlers = {}
    hdir = os.path.join(root, HANDLER_DIR)
    if not os.path.isdir(hdir):
        raise OSError(f"no handler directory: {hdir}")
    for fname in sorted(os.listdir(hdir)):
        if not fname.endswith(".c") or fname.endswith("_test.c"):
            continue
        path = os.path.join(hdir, fname)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for name, is_weak, body in scan_definitions(text):
            entry = handlers.setdefault(name, {"strong": None, "weak": None})
            rel = f"{HANDLER_DIR}/{fname}"
            if is_weak:
                entry["weak"] = entry["weak"] or rel
            elif entry["strong"] is None:
                entry["strong"] = (rel, body_is_stub(body))
    return handlers


def parse_services(root):
    """Return a list of dicts for every NT_SERVICE line, in table order."""
    path = os.path.join(root, SERVICE_LIST)
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    services = []
    for m in SERVICE_RE.finditer(text):
        services.append({
            "ssn": int(m.group(1), 16),
            "name": m.group(2),
            "argc": int(m.group(3)),
        })
    return services


def area_of(name):
    for area, rx in AREA_RES:
        if rx.search(name):
            return area
    return OTHER_AREA


def classify(services, handlers):
    rows = []
    for svc in services:
        h = handlers.get(svc["name"], {"strong": None, "weak": None})
        if h["strong"] is not None:
            file, is_stub = h["strong"]
            status = STUB if is_stub else IMPLEMENTED
        elif h["weak"] is not None:
            file, status = h["weak"], WEAK
        else:
            file, status = "", MISSING
        rows.append({**svc, "status": status, "file": file, "area": area_of(svc["name"])})
    return rows


def summarize(rows):
    counts = {s: 0 for s in STATUSES}
    for r in rows:
        counts[r["status"]] += 1
    return counts


def area_order(rows):
    seen = []
    for area, _ in AREA_PATTERNS:
        seen.append(area)
    seen.append(OTHER_AREA)
    return [a for a in seen if any(r["area"] == a for r in rows)]


def render_text(rows):
    counts = summarize(rows)
    lines = [f"services: {len(rows)}"]
    for s in STATUSES:
        lines.append(f"  {s}: {counts[s]}")
    return "\n".join(lines) + "\n"


def render_markdown(rows):
    counts = summarize(rows)
    out = []
    out.append("# NT service handler inventory")
    out.append("")
    out.append("<!-- Generated by tools/service_inventory.py --markdown. Do not edit by hand. -->")
    out.append("")
    out.append("Each service in `src/kernel/nt/nt_service_list.h` is classified by the")
    out.append("handler found in `src/kernel/nt/*.c`. The rules are in the tool's docstring.")
    out.append("")
    out.append("## Counts")
    out.append("")
    out.append("| Status | Services |")
    out.append("|---|---:|")
    out.append(f"| total | {len(rows)} |")
    for s in STATUSES:
        out.append(f"| {s} | {counts[s]} |")
    out.append("")
    out.append("## By area")
    out.append("")
    out.append("| Area | Total | IMPLEMENTED | STUB-NOT-IMPLEMENTED | WEAK-FALLBACK | MISSING |")
    out.append("|---|---:|---:|---:|---:|---:|")
    for area in area_order(rows):
        sub = [r for r in rows if r["area"] == area]
        c = summarize(sub)
        out.append(f"| {area} | {len(sub)} | {c[IMPLEMENTED]} | {c[STUB]} | {c[WEAK]} | {c[MISSING]} |")
    out.append("")
    for area in area_order(rows):
        sub = [r for r in rows if r["area"] == area]
        out.append(f"## Area: {area}")
        out.append("")
        out.append("| SSN | Service | Argc | Status | Handler file |")
        out.append("|---|---|---:|---|---|")
        for r in sub:
            f = f"`{r['file']}`" if r["file"] else ""
            out.append(f"| 0x{r['ssn']:04x} | `{r['name']}` | {r['argc']} | {r['status']} | {f} |")
        out.append("")
    return "\n".join(out).rstrip("\n") + "\n"


def run_inventory(root):
    services = parse_services(root)
    handlers = collect_handlers(root)
    return classify(services, handlers)


# --- self test --------------------------------------------------------------

FIXTURE_LIST = """\
NT_SERVICE(0x0046, NtYieldExecution, 0)
NT_SERVICE(0x0031, NtQueryPerformanceCounter, 2)
NT_SERVICE(0x0040, NtOpenEvent, 3)
NT_SERVICE(0x0036, NtQuerySystemInformation, 4)
NT_SERVICE(0x0000, NtAccessCheck, 8)
NT_SERVICE(0x0002, NtAcceptConnectPort, 9)
NT_SERVICE(0x0001, NtWeakOnly, 1)
NT_SERVICE(0x0003, NtTrulyAbsent, 2)
"""

FIXTURE_IMPL = """\
#include "nt_internal.h"
/* NTSTATUS nt_svc_NtAccessCheck(void) { return STATUS_NOT_IMPLEMENTED; } */
NTSTATUS nt_svc_NtYieldExecution(void)
{
    cond_resched();
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtQueryPerformanceCounter(PLARGE_INTEGER c, PLARGE_INTEGER f)
{
    (void)c; (void)f;
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS nt_svc_NtAccessCheck(void);
NTSTATUS nt_svc_NtQuerySystemInformation(ULONG a, PVOID b, ULONG c, PULONG d)
{
    if (!b) return STATUS_INVALID_PARAMETER;
    return STATUS_NOT_SUPPORTED;
}
"""

FIXTURE_WEAK = """\
__attribute__((weak)) NTSTATUS nt_svc_NtOpenEvent(void) { return STATUS_NOT_IMPLEMENTED; }
__attribute__((weak)) NTSTATUS nt_svc_NtWeakOnly(void) { return STATUS_SUCCESS; }
"""

FIXTURE_TEST = """\
NTSTATUS nt_svc_NtTrulyAbsent(void) { return STATUS_SUCCESS; }
"""

FIXTURE_STRONG_OVER_WEAK = """\
__attribute__((weak)) NTSTATUS nt_svc_NtAcceptConnectPort(void) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS nt_svc_NtAcceptConnectPort(void) { return STATUS_NOT_IMPLEMENTED; }
"""

EXPECTED = {
    "NtYieldExecution": IMPLEMENTED,
    "NtQueryPerformanceCounter": STUB,
    "NtOpenEvent": WEAK,
    "NtQuerySystemInformation": IMPLEMENTED,
    "NtAccessCheck": MISSING,
    "NtAcceptConnectPort": STUB,
    "NtWeakOnly": WEAK,
    "NtTrulyAbsent": MISSING,
}


def selftest():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    # Pure helpers.
    check(body_is_stub(" return STATUS_NOT_IMPLEMENTED; "), "stub: plain return")
    check(body_is_stub(" (void)a; (void)b; return STATUS_NOT_SUPPORTED; "), "stub: void casts")
    check(not body_is_stub(" if (x) return STATUS_NOT_IMPLEMENTED; return STATUS_SUCCESS; "),
          "stub: conditional is not a stub")
    check(not body_is_stub(" return STATUS_SUCCESS; "), "stub: success is not a stub")
    check(area_of("NtQueryTimerResolution") == "system", "area: timer resolution")
    check(area_of("NtSetTimer") == "sync", "area: timer is sync")
    check(area_of("NtFoo") == OTHER_AREA, "area: fallback")
    check(strip_comments_and_strings('"}" /* } */ x').strip().startswith('"'),
          "strip: keeps literal delimiters")

    with tempfile.TemporaryDirectory() as tmp:
        hdir = os.path.join(tmp, HANDLER_DIR)
        os.makedirs(hdir)
        os.makedirs(os.path.join(tmp, os.path.dirname(SERVICE_LIST)), exist_ok=True)
        with open(os.path.join(tmp, SERVICE_LIST), "w", encoding="utf-8") as fh:
            fh.write(FIXTURE_LIST)
        files = {
            "impl.c": FIXTURE_IMPL,
            "weak.c": FIXTURE_WEAK,
            "conn_test.c": FIXTURE_TEST,
            "override.c": FIXTURE_STRONG_OVER_WEAK,
        }
        for name, text in files.items():
            with open(os.path.join(hdir, name), "w", encoding="utf-8") as fh:
                fh.write(text)
        rows = run_inventory(tmp)
        got = {r["name"]: r["status"] for r in rows}
        check(len(rows) == len(EXPECTED), f"row count {len(rows)}")
        for name, want in EXPECTED.items():
            check(got.get(name) == want, f"{name}: want {want}, got {got.get(name)}")
        counts = summarize(rows)
        check(counts[IMPLEMENTED] == 2 and counts[STUB] == 2 and counts[WEAK] == 2
              and counts[MISSING] == 2, f"counts {counts}")
        md = render_markdown(rows)
        check("## Area: system" in md and "| 0x0046 | `NtYieldExecution` |" in md,
              "markdown content")
        check(render_markdown(rows) == md, "markdown is deterministic")

    for msg in fails:
        print(f"FAIL [selftest] {msg}")
    if fails:
        print(f"selftest: {len(fails)} failed")
        return True
    print("selftest: all checks passed")
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    fmt = parser.add_mutually_exclusive_group()
    fmt.add_argument("--markdown", action="store_true", help="emit Markdown")
    parser.add_argument("--out", help="write output to this file instead of stdout")
    parser.add_argument("--root", default=None, help="repository root (default: this repo)")
    parser.add_argument("--selftest", action="store_true", help="run built-in checks and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return 1 if selftest() else 0

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        rows = run_inventory(root)
    except OSError as exc:
        print(f"service_inventory: {exc}", file=sys.stderr)
        return 1
    text = render_markdown(rows) if args.markdown else render_text(rows)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
