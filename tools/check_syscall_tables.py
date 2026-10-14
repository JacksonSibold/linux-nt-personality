#!/usr/bin/env python3
"""
check_syscall_tables.py - check that every copy of the NT syscall table agrees.

The NT system-service table is written out in several places:

  src/ntdll/syscall_table.def        NT_SYSCALL(Name, Ssn, Arity, types...)
                                     the source read by build/gen_stubs.py and
                                     build/gen_ntdll_def.py
  src/kernel/nt/nt_service_list.h    NT_SERVICE(Ssn, Name, Argc)
  build/generated_stubs.S            one stub per name: mov $Ssn, %eax, plus a
                                     Zw alias with .set
  src/ntdll/ntdll.def                Nt* and Zw* export names; Zw aliases use
                                     ZwX=NtX
  src/kernel/nt/nt_internal.h        NT_NR_SERVICES (must be max SSN + 1)

The checker parses each one and reports:

  missing       a name in the table that a representation does not have
  extra         a name in a representation that the table does not have
  duplicate     a name or an SSN that appears twice in one representation
  ssn-mismatch  a name whose SSN differs from the table
  argc-mismatch a kernel entry whose argument count differs from the table's
                Arity
  argc-types    INFO only: a table entry whose ArgTypes count differs from
                its Arity (no generator reads that column)
  alias         a Zw alias whose target is not the matching Nt name
  nr-services   NT_NR_SERVICES that is not max SSN + 1
  ssn-gap       INFO only: the SSNs below the maximum that no service uses

Unused SSNs are expected (171 services over 0x000-0x1db), so gaps are printed
as information and do not fail the check.

ntdll.def exports one Nt* name that is not a syscall: NtCurrentTeb, added by
build/gen_ntdll_def.py. It is listed in NON_SYSCALL_NT_EXPORTS and is not
reported as extra. Repeated export lines are reported as duplicates.

syscall_stub.S includes syscall_table.def directly, so it has no literal SSNs
of its own and is covered by the table.

Usage:
  python3 tools/check_syscall_tables.py [--root PATH] [--quiet]
  python3 tools/check_syscall_tables.py --selftest

Exit status: 0 when every representation agrees, 1 on any mismatch (each
printed as `FAIL [kind] path: detail`), 2 on usage error. --selftest exits 0
when its built-in fixture gives the expected findings.
"""
import argparse
import os
import re
import sys
import tempfile

TABLE = "src/ntdll/syscall_table.def"
KERNEL_LISTS = ("src/kernel/nt/nt_service_list.h",)
STUBS = "build/generated_stubs.S"
NTDLL_DEF = "src/ntdll/ntdll.def"
NR_SERVICES_HEADERS = ("src/kernel/nt/nt_internal.h",)

REPRESENTATIONS = (TABLE, *KERNEL_LISTS, STUBS, NTDLL_DEF, *NR_SERVICES_HEADERS)

NON_SYSCALL_NT_EXPORTS = {"NtCurrentTeb"}

TABLE_RE = re.compile(
    r"^\s*NT_SYSCALL\s*\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*"
    r"(?:,(.*))?\)\s*$", re.M)
TABLE_LINE_RE = re.compile(r"^\s*NT_SYSCALL\s*\(", re.M)
KERNEL_RE = re.compile(
    r"^\s*NT_SERVICE\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*(\w+)\s*,\s*(\d+)\s*\)\s*$",
    re.M)
KERNEL_LINE_RE = re.compile(r"^\s*NT_SERVICE\s*\(", re.M)
STUB_RE = re.compile(
    r"^(\w+):\n[ \t]+mov %rcx, %r10\n[ \t]+mov \$(0x[0-9a-fA-F]+), %eax\n"
    r"[ \t]+syscall\n[ \t]+ret\n[ \t]+\.globl (\w+)\n[ \t]+\.set (\w+), (\w+)\n",
    re.M)
STUB_LABEL_RE = re.compile(r"^(\w+):\s*$", re.M)
DEF_LINE_RE = re.compile(r"^[ \t]+(\w+)(?:=(\w+))?[ \t]*$", re.M)
NR_RE = re.compile(r"^#define\s+NT_NR_SERVICES\s+(\d+)", re.M)


def zw_name(nt):
    """Name of the Zw alias for an Nt name, as build/gen_stubs.py forms it."""
    return "Zw" + nt[2:] if nt.startswith("Nt") else "Zw" + nt


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as fh:
        return fh.read().replace("\r\n", "\n")


class Report:
    def __init__(self):
        self.findings = []  # (kind, path, detail)
        self.info = []

    def fail(self, kind, path, detail):
        self.findings.append((kind, path, detail))

    def note(self, kind, path, detail):
        self.info.append((kind, path, detail))


def dup_check(report, path, pairs):
    """pairs is a list of (name, ssn). Report repeated names and SSNs."""
    seen_name = {}
    seen_ssn = {}
    for name, ssn in pairs:
        if name in seen_name:
            report.fail("duplicate", path, f"name {name} appears twice")
        seen_name[name] = ssn
        if ssn in seen_ssn and seen_ssn[ssn] != name:
            report.fail("duplicate", path,
                        f"SSN 0x{ssn:04x} used by {seen_ssn[ssn]} and {name}")
        seen_ssn.setdefault(ssn, name)


def dup_check_names(report, path, names):
    """Report export names listed more than once (no SSNs in this file)."""
    seen = set()
    for name in names:
        if name in seen:
            report.fail("duplicate", path, f"export {name} is listed twice")
        seen.add(name)


def compare_names(report, path, table_names, got_names):
    for name in sorted(table_names - got_names):
        report.fail("missing", path, f"{name} is in the table but not here")
    for name in sorted(got_names - table_names):
        report.fail("extra", path, f"{name} is here but not in the table")


def parse_table(text):
    """Return (entries, unparsed) for syscall_table.def.

    entries: list of (name, ssn, arity, ntypes) in file order.
    """
    entries = []
    for m in TABLE_RE.finditer(text):
        types = [t for t in (m.group(4) or "").split(",") if t.strip()]
        entries.append((m.group(1), int(m.group(2), 16), int(m.group(3)),
                        len(types)))
    unparsed = len(TABLE_LINE_RE.findall(text)) - len(entries)
    return entries, unparsed


def parse_kernel(text):
    entries = [(name, int(ssn, 16), int(argc))
               for ssn, name, argc in KERNEL_RE.findall(text)]
    unparsed = len(KERNEL_LINE_RE.findall(text)) - len(entries)
    return entries, unparsed


def parse_stubs(text):
    """Return (stubs, aliases, labels).

    stubs: list of (name, ssn, alias_name, alias_target) for each complete
    stub. labels: every `name:` line, so a stub with a different shape shows.
    """
    stubs = []
    for m in STUB_RE.finditer(text):
        label, ssn, glob, set_name, set_target = m.groups()
        stubs.append((label, int(ssn, 16), glob, set_name, set_target))
    labels = STUB_LABEL_RE.findall(text)
    return stubs, labels


def parse_def(text):
    """Return (names, aliases) for ntdll.def. names is the list of export
    names in file order (repeats kept); aliases maps each ZwX=NtX alias name
    to its target (None if there is no target)."""
    names = []
    aliases = {}
    for name, target in DEF_LINE_RE.findall(text):
        if name in ("LIBRARY", "EXPORTS"):
            continue
        names.append(name)
        if name.startswith("Zw"):
            aliases[name] = target or None
    return names, aliases


def check_tree(root):
    """Return a Report for the tree rooted at root."""
    report = Report()
    try:
        table_text = read(root, TABLE)
        kernel_texts = {p: read(root, p) for p in KERNEL_LISTS}
        stub_text = read(root, STUBS)
        def_text = read(root, NTDLL_DEF)
        nr_texts = {p: read(root, p) for p in NR_SERVICES_HEADERS}
    except OSError as exc:
        report.fail("missing-file", "-", str(exc))
        return report, 0

    table, unparsed = parse_table(table_text)
    if unparsed or not table:
        report.fail("parse", TABLE,
                    f"{len(table)} entries parsed, {unparsed} lines unparsed")
        return report, 0
    dup_check(report, TABLE, [(n, s) for n, s, _, _ in table])
    table_ssn = {n: s for n, s, _, _ in table}
    table_arity = {n: a for n, _, a, _ in table}
    table_names = set(table_ssn)

    # The ArgTypes column is not read by any generator, so a count that
    # differs from Arity is reported as information, not as a mismatch.
    for n, _, arity, ntypes in table:
        if arity != ntypes:
            report.note("argc-types", TABLE,
                        f"{n} Arity {arity} but {ntypes} ArgTypes listed")

    # Kernel lists: each must match the table on names, SSNs and argc.
    for path, text in kernel_texts.items():
        entries, unparsed = parse_kernel(text)
        if unparsed or not entries:
            report.fail("parse", path,
                        f"{len(entries)} entries parsed, {unparsed} lines unparsed")
            continue
        dup_check(report, path, [(n, s) for n, s, _ in entries])
        got = {n: s for n, s, _ in entries}
        compare_names(report, path, table_names, set(got))
        for n, s, argc in entries:
            if n in table_ssn and table_ssn[n] != s:
                report.fail("ssn-mismatch", path,
                            f"{n} is 0x{s:04x}, table says 0x{table_ssn[n]:04x}")
            if n in table_arity and table_arity[n] != argc:
                report.fail("argc-mismatch", path,
                            f"{n} argc {argc}, table Arity {table_arity[n]}")

    # Generated stubs: SSN per name, and the Zw alias for each Nt stub.
    stubs, labels = parse_stubs(stub_text)
    if len(labels) != len(stubs) or not stubs:
        report.fail("parse", STUBS,
                    f"{len(stubs)} stubs parsed from {len(labels)} labels")
    else:
        dup_check(report, STUBS, [(n, s) for n, s, *_ in stubs])
        got = {n: s for n, s, *_ in stubs}
        compare_names(report, STUBS, table_names, set(got))
        for n, s, glob, set_name, set_target in stubs:
            if n in table_ssn and table_ssn[n] != s:
                report.fail("ssn-mismatch", STUBS,
                            f"{n} is 0x{s:04x}, table says 0x{table_ssn[n]:04x}")
            want = zw_name(n)
            if glob != set_name or set_name != want or set_target != n:
                report.fail("alias", STUBS,
                            f"{n}: alias {set_name} -> {set_target}, "
                            f"expected {want} -> {n}")

    # ntdll.def: names only (the file carries no SSNs).
    def_names, aliases = parse_def(def_text)
    dup_check_names(report, NTDLL_DEF, def_names)
    nt_def = {n for n in def_names
              if n.startswith("Nt") and n not in NON_SYSCALL_NT_EXPORTS}
    zw_def = {n for n in def_names if n.startswith("Zw")}
    compare_names(report, NTDLL_DEF, table_names, nt_def)
    compare_names(report, NTDLL_DEF,
                  {zw_name(n) for n in table_names}, zw_def)
    for alias, target in sorted(aliases.items()):
        if target is None:
            report.fail("alias", NTDLL_DEF,
                        f"{alias} has no target (expected ={'Nt' + alias[2:]})")
        elif target != "Nt" + alias[2:]:
            report.fail("alias", NTDLL_DEF,
                        f"{alias} targets {target}, expected Nt{alias[2:]}")

    # Gaps and NT_NR_SERVICES.
    max_ssn = max(table_ssn.values())
    used = len(table_ssn)
    gaps = max_ssn + 1 - used
    report.note("ssn-gap", TABLE,
                f"{gaps} unused SSNs below 0x{max_ssn:04x} "
                f"({used} services, max SSN 0x{max_ssn:04x})")
    for path, text in nr_texts.items():
        m = NR_RE.search(text)
        if not m:
            report.fail("parse", path, "NT_NR_SERVICES not defined")
        elif int(m.group(1)) != max_ssn + 1:
            report.fail("nr-services", path,
                        f"NT_NR_SERVICES is {m.group(1)}, "
                        f"expected {max_ssn + 1} (max SSN + 1)")
    return report, len(table)


def summary(report, count):
    lines = []
    for kind, path, detail in report.info:
        lines.append(f"INFO [{kind}] {path}: {detail}")
    for kind, path, detail in report.findings:
        lines.append(f"FAIL [{kind}] {path}: {detail}")
    if report.findings:
        lines.append(f"syscall tables: {len(report.findings)} mismatch(es) "
                     f"across {count} services")
    else:
        lines.append(f"syscall tables: parity holds for {count} services "
                     f"in {len(REPRESENTATIONS)} files")
    return lines


# Built-in fixture used by --selftest. Each case mutates one copy of a
# four-service table and names the finding kinds it must produce.
SELFTEST_SERVICES = [
    ("NtCreateFile", 0x0055, 11, "PHANDLE"),
    ("NtClose", 0x000f, 1, "HANDLE"),
    ("NtTestAlert", 0x01c8, 0, ""),
    ("NtRtlNtStatusToDosError", 0x01db, 2, "NTSTATUS, PULONG"),
]


def render_fixture(root, services, *, kernel_argc=None, kernel_ssn=None,
                   stub_ssn=None, drop_stub=None, def_omit=None,
                   nr=None, bad_alias=None, dup_kernel=False):
    """Write a fixture tree. Keyword arguments inject one fault each."""
    kernel_argc = kernel_argc or {}
    kernel_ssn = kernel_ssn or {}
    stub_ssn = stub_ssn or {}
    drop_stub = drop_stub or set()
    def_omit = def_omit or set()
    bad_alias = bad_alias or set()

    def put(rel, text):
        full = os.path.join(root, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)

    max_ssn = max(s for _, s, _, _ in services)
    table = ["#ifndef NT_SYSCALL", "#define NT_SYSCALL(Name, Ssn, Arity, ...)",
             "#endif"]
    for name, ssn, arity, types in services:
        tail = f", {types}" if types else ""
        table.append(f"NT_SYSCALL({name}, 0x{ssn:04x}, {arity}{tail})")
    table.append("#undef NT_SYSCALL")
    put(TABLE, "\n".join(table) + "\n")

    for rel in KERNEL_LISTS:
        lines = []
        for name, ssn, arity, _ in services:
            argc = kernel_argc.get(name, arity)
            s = kernel_ssn.get(name, ssn)
            lines.append(f"NT_SERVICE(0x{s:04x}, {name}, {argc})")
            if dup_kernel and name == services[0][0]:
                lines.append(f"NT_SERVICE(0x{s:04x}, {name}, {argc})")
        put(rel, "\n".join(lines) + "\n")

    for rel in NR_SERVICES_HEADERS:
        value = max_ssn + 1 if nr is None else nr
        put(rel, f"#define NT_NR_SERVICES {value}  /* fixture */\n")

    stubs = ["/* auto-generated syscall stubs - do not edit */",
             "    .text", "    .align 16"]
    for name, ssn, _, _ in services:
        if name in drop_stub:
            continue
        zw = "Zw" + name[2:]
        target = name if "Zw" + name[2:] not in bad_alias else "NtTestAlert"
        s = stub_ssn.get(name, ssn)
        stubs += [f"{name}:",
                  "    mov %rcx, %r10",
                  f"    mov ${s:#06x}, %eax",
                  "    syscall",
                  "    ret",
                  f"    .globl {zw}",
                  f"    .set {zw}, {target}"]
    put(STUBS, "\n".join(stubs) + "\n")

    defs = ["LIBRARY ntdll", "EXPORTS"]
    for name, _, _, _ in services:
        if name not in def_omit:
            defs.append(f"    {name}")
        zw = "Zw" + name[2:]
        if zw not in def_omit:
            target = name if zw not in bad_alias else "NtTestAlert"
            defs.append(f"    {zw}={target}")
    defs.append("    NtCurrentTeb")
    put(NTDLL_DEF, "\n".join(defs) + "\n")


def expect_kinds(report, kinds):
    got = sorted({k for k, _, _ in report.findings})
    return got == sorted(kinds)


def selftest():
    cases = [
        ("clean", {}, []),
        ("missing-kernel", {"kernel_only": True}, ["missing"]),
        ("argc", {"kernel_argc": {"NtClose": 2}}, ["argc-mismatch"]),
        ("ssn", {"kernel_ssn": {"NtCreateFile": 0x0056}}, ["ssn-mismatch"]),
        ("stub-ssn", {"stub_ssn": {"NtClose": 0x0010}}, ["ssn-mismatch"]),
        ("drop-stub", {"drop_stub": {"NtTestAlert"}}, ["missing"]),
        ("dup", {"dup_kernel": True}, ["duplicate"]),
        ("def-missing", {"def_omit": {"NtClose"}}, ["missing"]),
        ("alias", {"bad_alias": {"ZwClose"}}, ["alias"]),
        ("nr", {"nr": 0x1dd}, ["nr-services"]),
    ]
    for label, opts, want in cases:
        opts = dict(opts)
        kernel_only = opts.pop("kernel_only", False)
        with tempfile.TemporaryDirectory() as tmp:
            render_fixture(tmp, SELFTEST_SERVICES, **opts)
            if kernel_only:
                # Drop one name from the first kernel list only.
                path = os.path.join(tmp, KERNEL_LISTS[0])
                with open(path, encoding="utf-8") as fh:
                    kept = [ln for ln in fh if "NtClose" not in ln]
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.writelines(kept)
            report, _ = check_tree(tmp)
            if not expect_kinds(report, want):
                got = sorted({k for k, _, _ in report.findings})
                print(f"selftest FAIL: {label}: expected {want}, got {got}",
                      file=sys.stderr)
                return 1
    print(f"selftest ok: {len(cases)} fixture cases give the expected findings")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check that every copy of the NT syscall table agrees.")
    parser.add_argument("--root", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".."),
        help="repository root (default: parent of tools/)")
    parser.add_argument("--quiet", action="store_true",
                        help="print only the summary line")
    parser.add_argument("--selftest", action="store_true",
                        help="check the built-in fixture and exit")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"check_syscall_tables: error: not a directory: {root}",
              file=sys.stderr)
        return 2
    report, count = check_tree(root)
    lines = summary(report, count)
    if args.quiet:
        lines = lines[-1:]
    print("\n".join(lines))
    return 1 if report.findings else 0


if __name__ == "__main__":
    sys.exit(main())
