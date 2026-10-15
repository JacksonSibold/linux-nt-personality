#!/usr/bin/env python3
"""
kernel_gate.py - check a built NT kernel against what the repository says it contains.

The shell wrapper tests/kernel/check_kernel_complete.sh finds vmlinux and built-in.a,
writes `nm` and `ar t` output to files, and calls this script. The expectations are
read from the repository, not written into the gate:

  services      tools/service_inventory.py, per-service kernel_class. Every entry of
                src/kernel/nt/nt_service_list.h whose kernel-branch class is
                IMPLEMENTED, REFUSES or STUB-NOT-IMPLEMENTED has a strong definition
                in the __KERNEL__ branch, so vmlinux must define nt_svc_<Name> as a
                strong (non-weak) symbol. WEAK-FALLBACK, UNREACHABLE-WRAPPER-MISSING
                and MISSING services have no strong kernel-branch handler and are
                reported as kernel gaps, not expected.
  objects       the obj-$(CONFIG_NT_PERSONALITY) += X.o lines of arch/x86/nt/Makefile.
                With CONFIG_NT_PERSONALITY=y in the kernel .config, each one must be a
                member of arch/x86/nt/built-in.a.
  entry points  the fixed infrastructure symbols the personality is reached through.
                They are names, not counts.

Verdict: FAIL when
  - an expected service symbol is absent from nm or defined only as a weak fallback
    (a shortfall: the inventory says the kernel branch implements it, the binary
    does not have it);
  - an expected object is not a member of built-in.a;
  - an entry point is not a strong symbol;
  - the inventory finds no services, the Makefile lists no objects, or the object
    check is required but built-in.a is missing.
PASS otherwise. Kernel gaps and stubs are listed in the report and never fail the
gate on their own: the table routes them to NULL or to the not-implemented return,
so a tree with gaps still boots. A shortfall is a service that should be there.

Usage:
  python3 tools/kernel_gate.py --repo ROOT --nm FILE [--ar FILE] [--config FILE] [--json]

  --nm      output of `nm vmlinux` (required)
  --ar      output of `ar t arch/x86/nt/built-in.a` (absent file: an error when the
            kernel .config enables the personality)
  --config  kernel .config. Without it, or with CONFIG_NT_PERSONALITY not =y, the
            object check is skipped and the report says so.
  --json    print the report as JSON instead of text

Exit status: 0 PASS, 1 FAIL, 2 usage or input error.
Standard library only. Nothing is modified.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import service_inventory as si  # noqa: E402

SERVICE_PREFIX = "nt_svc_"
MAKEFILE = "arch/x86/nt/Makefile"
STRONG_CLASSES = (si.IMPLEMENTED, si.REFUSES, si.STUB)
GAP_CLASSES = (si.WEAK, si.UNREACHABLE, si.MISSING)

# Entry points the personality is reached through: not a count, a fixed set of names
# that must be strong symbols in any build with the personality.
ENTRY_SYMBOLS = (
    "do_nt_syscall",
    "nt_service_table",
    "nt_enter_personality",
    "nt_exit_personality",
    "nt_process_exit_handles",
    "nt_banner_init",
    "ObCreateHandle",
    "ObReferenceObjectByHandle",
    "nt_create_event",
    "se_access_check",
)

# nm symbol-type letters. Upper and lower case mean global and local; W/V are weak.
STRONG_TYPES = frozenset("TtDdBbRrSsGgCc")
WEAK_TYPES = frozenset("WwVv")
RANK = {"undefined": 0, "other": 1, "weak": 2, "strong": 3}
BINARY_LABEL = {
    "strong": "defined",
    "weak": "defined, weak binding",
    "other": "defined, not a code or data symbol",
    "undefined": "absent",
}

OBJ_LINE_RE = re.compile(
    r"^\s*obj-\$\(CONFIG_NT_PERSONALITY\)\s*\+=\s*([A-Za-z0-9_.-]+\.o)\s*$", re.M)
CONFIG_NT_RE = re.compile(r"^CONFIG_NT_PERSONALITY=y\s*$", re.M)


def parse_nm(text):
    """Map symbol name -> "strong", "weak", "other" or "undefined".

    nm prints `addr type name` for a defined symbol and `type name` (no address)
    for an undefined one, including an unresolved weak reference. When a name
    appears more than once, the strongest definition wins.
    """
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3:
            _addr, kind, name = parts
            if kind in STRONG_TYPES:
                cls = "strong"
            elif kind in WEAK_TYPES:
                cls = "weak"
            else:
                cls = "other"
        elif len(parts) == 2:
            _kind, name = parts
            cls = "undefined"
        else:
            continue
        prev = out.get(name)
        if prev is None or RANK[cls] > RANK[prev]:
            out[name] = cls
    return out


def parse_ar(text):
    """Member names printed by `ar t`, in archive order, as base names.

    Some archives record members with their directory (the kernel's built-in.a
    lists /path/to/arch/x86/nt/dispatch.o), so the directory is dropped before
    comparing with the object names in the Makefile.
    """
    members = []
    for line in text.splitlines():
        name = line.strip()
        if name:
            members.append(name.replace("\\", "/").rsplit("/", 1)[-1])
    return members


def parse_makefile_objects(text):
    """Object names from the obj-$(CONFIG_NT_PERSONALITY) lines, in file order."""
    return OBJ_LINE_RE.findall(text)


def config_enables_personality(text):
    return bool(text is not None and CONFIG_NT_RE.search(text))


def evaluate(rows, nm_text, makefile_text, ar_text, config_text):
    """Compare the expectations with the binary. Returns the report dict.

    rows is the output of service_inventory.run_inventory(). ar_text is None when
    the archive is absent. config_text is None when there is no .config.
    """
    symbols = parse_nm(nm_text)
    present, shortfalls, gaps, stubs = [], [], [], []
    expected_names = set()
    for r in rows:
        name = r["name"]
        if r["kernel_class"] in STRONG_CLASSES:
            expected_names.add(SERVICE_PREFIX + name)
            got = symbols.get(SERVICE_PREFIX + name, "undefined")
            if got == "strong":
                present.append(name)
            else:
                shortfalls.append({"name": name, "kernel_class": r["kernel_class"],
                                   "kernel_file": r["kernel_file"],
                                   "binary": BINARY_LABEL[got]})
            if r["kernel_class"] == si.STUB:
                stubs.append({"name": name, "kernel_file": r["kernel_file"]})
        else:
            gaps.append({"name": name, "kernel_class": r["kernel_class"],
                         "binary": BINARY_LABEL[symbols.get(SERVICE_PREFIX + name, "undefined")]})
    extra_symbols = sorted(s[len(SERVICE_PREFIX):] for s, cls in symbols.items()
                           if s.startswith(SERVICE_PREFIX) and cls == "strong"
                           and s not in expected_names)

    entry_missing = [s for s in ENTRY_SYMBOLS if symbols.get(s) != "strong"]

    objects = {"checked": False, "expected": [], "missing": [], "extra": [], "error": None,
               "note": None}
    expected_objs = parse_makefile_objects(makefile_text or "")
    objects["expected"] = expected_objs
    if not config_enables_personality(config_text):
        objects["note"] = ("skipped: no .config" if config_text is None
                           else "skipped: CONFIG_NT_PERSONALITY is not y")
    elif ar_text is None:
        objects["error"] = "built-in.a not found, and CONFIG_NT_PERSONALITY=y"
    else:
        members = parse_ar(ar_text)
        objects["checked"] = True
        objects["missing"] = [o for o in expected_objs if o not in members]
        objects["extra"] = [m for m in members if m not in expected_objs]

    problems = []
    if not rows:
        problems.append("the service inventory found no services")
    if not expected_objs:
        problems.append("%s lists no obj-$(CONFIG_NT_PERSONALITY) objects" % MAKEFILE)
    if shortfalls:
        problems.append("%d expected service symbol(s) not strong in the binary" % len(shortfalls))
    if entry_missing:
        problems.append("%d entry point(s) not strong: %s" % (len(entry_missing),
                                                              " ".join(entry_missing)))
    if objects["error"]:
        problems.append(objects["error"])
    if objects["missing"]:
        problems.append("%d expected object(s) missing from built-in.a" % len(objects["missing"]))

    return {
        "verdict": "FAIL" if problems else "PASS",
        "problems": problems,
        "services": {
            "table": len(rows),
            "expected": len(expected_names),
            "expected_by_class": {c: sum(1 for r in rows if r["kernel_class"] == c)
                                  for c in STRONG_CLASSES},
            "present": len(present),
            "shortfalls": shortfalls,
            "gaps": gaps,
            "stubs": stubs,
            "extra_symbols": extra_symbols,
        },
        "entry_points": {"expected": list(ENTRY_SYMBOLS), "missing": entry_missing},
        "objects": objects,
    }


def render_text(report, nm_label, repo):
    svc = report["services"]
    lines = ["NT kernel completeness gate",
             "  repo:     %s" % repo,
             "  nm:       %s" % nm_label,
             "  table:    %d services (src/kernel/nt/nt_service_list.h)" % svc["table"],
             "  expected: %d kernel-branch strong handlers (%s)" % (
                 svc["expected"],
                 ", ".join("%s %d" % (c, n) for c, n in svc["expected_by_class"].items())),
             "  present:  %d strong nt_svc_* symbols in the binary" % svc["present"]]
    lines.append("SHORTFALL: %d expected service(s) not strong in the binary" % len(svc["shortfalls"]))
    for s in svc["shortfalls"]:
        lines.append("  %-32s %-12s %-24s %s" % (s["name"], s["kernel_class"], s["binary"],
                                                  s["kernel_file"]))
    lines.append("KERNEL GAPS: %d service(s) with no kernel-branch strong handler (not failing)" % len(svc["gaps"]))
    for g in svc["gaps"]:
        lines.append("  %-32s %-24s %s" % (g["name"], g["kernel_class"], g["binary"]))
    lines.append("KERNEL STUBS: %d expected service(s) that return not-implemented (not failing)" % len(svc["stubs"]))
    for s in svc["stubs"]:
        lines.append("  %-32s %s" % (s["name"], s["kernel_file"]))
    if svc["extra_symbols"]:
        lines.append("note: %d strong nt_svc_* symbol(s) not in the table: %s" % (
            len(svc["extra_symbols"]), " ".join(svc["extra_symbols"])))
    ep = report["entry_points"]
    lines.append("entry points: %d expected, %d not strong%s" % (
        len(ep["expected"]), len(ep["missing"]),
        (": " + " ".join(ep["missing"])) if ep["missing"] else ""))
    ob = report["objects"]
    if ob["checked"]:
        lines.append("built-in.a: %d expected objects from %s, %d missing%s" % (
            len(ob["expected"]), MAKEFILE, len(ob["missing"]),
            (": " + " ".join(ob["missing"])) if ob["missing"] else ""))
        if ob["extra"]:
            lines.append("note: built-in.a has members not in %s: %s" % (MAKEFILE, " ".join(ob["extra"])))
    else:
        lines.append("built-in.a: %s" % (ob["error"] or ob["note"]))
    if report["verdict"] == "PASS":
        lines.append("VERDICT: PASS")
    else:
        lines.append("VERDICT: FAIL (%s)" % "; ".join(report["problems"]))
    return "\n".join(lines) + "\n"


def read_optional(path):
    if path is None or not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--repo", default=os.path.dirname(HERE),
                        help="repository root (default: the parent of tools/)")
    parser.add_argument("--nm", required=True, help="output of nm vmlinux")
    parser.add_argument("--ar", help="output of ar t built-in.a")
    parser.add_argument("--config", help="kernel .config")
    parser.add_argument("--label", default=None, help="name of the vmlinux in the report")
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    args = parser.parse_args(argv)

    try:
        with open(args.nm, "r", encoding="utf-8", errors="replace") as fh:
            nm_text = fh.read()
        rows = si.run_inventory(args.repo)
        with open(os.path.join(args.repo, MAKEFILE), "r", encoding="utf-8", errors="replace") as fh:
            makefile_text = fh.read()
    except OSError as exc:
        print("kernel_gate: %s" % exc, file=sys.stderr)
        return 2
    report = evaluate(rows, nm_text, makefile_text, read_optional(args.ar),
                      read_optional(args.config))
    if args.json:
        sys.stdout.write(json.dumps(report, indent=2) + "\n")
    else:
        sys.stdout.write(render_text(report, args.label or args.nm, args.repo))
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
