#!/usr/bin/env python3
"""
service_inventory.py - classify the handler of every NT system service.

The service table is src/kernel/nt/nt_service_list.h, one line per service:

    NT_SERVICE(0x0046, NtYieldExecution, 0)

For each service the handler definitions in src/kernel/nt/*.c are looked up
as `NTSTATUS nt_svc_<Name>(...) { ... }`. Test sources (*_test.c) and
prototypes (no body) are ignored. Each service gets one class:

  IMPLEMENTED           a strong definition whose body is not a refusal
  STUB-NOT-IMPLEMENTED  a strong definition whose body is only a bare
                        `return STATUS_NOT_IMPLEMENTED;` or
                        `return STATUS_NOT_SUPPORTED;` (void casts of unused
                        arguments are allowed)
  REFUSES               a strong definition whose body refuses after argument
                        validation. The body is zero or more statements
                        `if (...) return V;` with V one of STATUS_INVALID_PARAMETER,
                        STATUS_INVALID_HANDLE, STATUS_INVALID_ADDRESS, followed
                        by exactly one `return R;` with R one of
                        STATUS_NOT_IMPLEMENTED, STATUS_NOT_SUPPORTED,
                        STATUS_INVALID_DEVICE_REQUEST, STATUS_ACCESS_DENIED, or
                        STATUS_INVALID_INFO_CLASS. The last status is accepted
                        only in that shape: it is then a refusal of every class
                        (the class argument is never tested; the validation
                        returns refuse earlier, and nothing can succeed). Any
                        other statement (a switch on the class, a conditional
                        refusal, a STATUS_SUCCESS path, a loop) makes the body
                        IMPLEMENTED. A bare return of the two stub statuses is
                        STUB, not REFUSES; every other refusal body is REFUSES.
  WEAK-FALLBACK         only weak definitions exist, so the table entry is the weak
                        fallback. A definition is weak when it carries the weak
                        attribute, or when a weak declaration of the same name comes
                        before it in the file (a prototype with the attribute, or the
                        NT_SERVICE macro that declares the table weak, then included).
  UNREACHABLE-WRAPPER-MISSING
                        no nt_svc_<Name> definition at all, but a function
                        defined in src/kernel/nt/*.c (not an nt_svc_ wrapper)
                        matches the service name. Names match after lower-casing,
                        removing underscores, removing a leading "nt" and a
                        trailing "internal" or "impl". The first match in file
                        order is reported as the handler file.
  MISSING               no definition at all; the table entry is NULL
  A (userspace) / B (kernel)
                        hybrid: the service has a strong definition in the
                        userspace branch and one in the kernel branch of a
                        conditional on __KERNEL__. A and B are the classes of
                        the two branches, classified by the rules above.

Linker rules apply: a strong definition overrides a weak one, so a service
with both is classified by its strong definition. Comments and string literals
are removed before bodies are read.

Preprocessor conditionals: only conditionals on __KERNEL__ are evaluated.
`#ifdef __KERNEL__` and `#if defined(__KERNEL__)` put the kernel branch first;
`#ifndef __KERNEL__` and `#if !defined(__KERNEL__)` put the userspace branch
first; the other branch of each is the opposite side. An `#elif` makes its
branch unlabelled. Every other conditional is transparent: its branches are
read as one. A service that has strong definitions under both labelled
branches is reported as a hybrid; otherwise the first strong definition in
file order decides its class.

Each service is placed in an area chosen from its name (first matching
pattern wins). The area is only for grouping in the report.

Usage:
  python3 tools/service_inventory.py                    summary on stdout
  python3 tools/service_inventory.py --markdown         Markdown report on stdout
  python3 tools/service_inventory.py --markdown --out docs/status/SERVICES.md
  python3 tools/service_inventory.py --json             per-service records (JSON)
  python3 tools/service_inventory.py --root PATH        scan another tree
  python3 tools/service_inventory.py --selftest         built-in fixtures

Exit status: 0 success, 1 a source file cannot be read or the selftest fails,
2 usage error. Standard library only; nothing in the tree is modified unless
--out is given.
"""
import argparse
import json
import os
import re
import sys
import tempfile

SERVICE_LIST = "src/kernel/nt/nt_service_list.h"
HANDLER_DIR = "src/kernel/nt"

IMPLEMENTED = "IMPLEMENTED"
STUB = "STUB-NOT-IMPLEMENTED"
REFUSES = "REFUSES"
WEAK = "WEAK-FALLBACK"
UNREACHABLE = "UNREACHABLE-WRAPPER-MISSING"
MISSING = "MISSING"
STATUSES = (IMPLEMENTED, STUB, REFUSES, WEAK, UNREACHABLE, MISSING)
BODY_CLASSES = (IMPLEMENTED, STUB, REFUSES)

USERSPACE = "userspace"
KERNEL = "kernel"
HYBRID_SEP = " / "

STUB_STATUSES = ("STATUS_NOT_IMPLEMENTED", "STATUS_NOT_SUPPORTED")
REFUSAL_STATUSES = STUB_STATUSES + ("STATUS_INVALID_DEVICE_REQUEST", "STATUS_ACCESS_DENIED")
# Final-return status that refuses every class. It counts as a refusal only in
# the shape refusal_class() accepts (no statement but validation returns before
# it), so a body that dispatches on the class is never REFUSES because of it.
CLASS_REFUSAL_STATUS = "STATUS_INVALID_INFO_CLASS"
VALIDATION_STATUSES = ("STATUS_INVALID_PARAMETER", "STATUS_INVALID_HANDLE", "STATUS_INVALID_ADDRESS")

SERVICE_RE = re.compile(
    r"^\s*NT_SERVICE\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*(\w+)\s*,\s*(\d+)\s*\)\s*$",
    re.M)
DEF_RE = re.compile(
    r"(?P<attr>__attribute__\s*\(\(\s*weak\s*\)\)\s*)?"
    r"\bNTSTATUS\s+nt_svc_(?P<name>\w+)\s*\(", re.S)
FUNC_HEAD_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
WEAK_ATTR_RE = re.compile(r"__attribute__\s*\(\(\s*weak\s*\)\)")
# A prototype (no body) of nt_svc_<name> carrying the weak attribute, before or after
# the name. Group 1 or group 2 is the service name.
WEAK_PROTO_RE = re.compile(
    r"__attribute__\s*\(\(\s*weak\s*\)\)[^;{}]*?\bnt_svc_(\w+)\s*\([^;{}]*\)\s*;"
    r"|\bnt_svc_(\w+)\s*\([^;{}]*\)\s*__attribute__\s*\(\(\s*weak\s*\)\)\s*;")
DEFINE_SERVICE_RE = re.compile(r"^\s*#\s*define\s+NT_SERVICE\b(.*)$")
UNDEF_SERVICE_RE = re.compile(r"^\s*#\s*undef\s+NT_SERVICE\b")
INCLUDE_LIST_RE = re.compile(r'^\s*#\s*include\s*"' + re.escape(os.path.basename(SERVICE_LIST)) + '"')
DIRECTIVE_LINE_RE = re.compile(r"(?m)^[ \t]*#.*$")
DIRECTIVE_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")
KERNEL_IF_RE = re.compile(r"(!?)\s*(defined\s*\(\s*__KERNEL__\s*\)|defined\s+__KERNEL__|__KERNEL__)")
VOID_CAST_RE = re.compile(r"\(\s*void\s*\)[^;]*;")
RETURN_RE = re.compile(r"^return\s+(\w+)$", re.S)
VALIDATION_RE = re.compile(r"^if\s*\(.*\)\s*return\s+(\w+)$", re.S)
C_KEYWORDS = frozenset({
    "if", "for", "while", "switch", "return", "sizeof", "else", "do", "defined",
    "typeof", "__typeof__", "_Alignof", "__attribute__", "_Static_assert",
})
IMPL_SUFFIXES = ("internal", "impl")

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


def hybrid_status(user_class, kernel_class):
    """Status text for a service defined in both branches of a __KERNEL__ conditional."""
    return f"{user_class} ({USERSPACE}) / {kernel_class} ({KERNEL})"


def is_hybrid(status):
    return HYBRID_SEP in status


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


def refusal_class(body):
    """STUB, REFUSES, or None (not a refusal) for one function body."""
    rest = VOID_CAST_RE.sub(" ", body).replace("{", " ").replace("}", " ")
    stmts = [s.strip() for s in rest.split(";") if s.strip()]
    if not stmts:
        return None
    last = RETURN_RE.match(stmts[-1])
    if not last or last.group(1) not in REFUSAL_STATUSES + (CLASS_REFUSAL_STATUS,):
        return None
    for stmt in stmts[:-1]:
        check = VALIDATION_RE.match(stmt)
        if not check or check.group(1) not in VALIDATION_STATUSES:
            return None
    if len(stmts) == 1 and last.group(1) in STUB_STATUSES:
        return STUB
    return REFUSES


def body_is_stub(body):
    """True when the body is a bare stub return (see refusal_class)."""
    return refusal_class(body) == STUB


def body_class(body):
    return refusal_class(body) or IMPLEMENTED


def kernel_branch_labels(kind, expr):
    """(then, else) branch labels for a conditional on __KERNEL__, else (None, None)."""
    expr = expr.strip()
    if kind == "ifdef" and expr == "__KERNEL__":
        return KERNEL, USERSPACE
    if kind == "ifndef" and expr == "__KERNEL__":
        return USERSPACE, KERNEL
    if kind == "if":
        m = KERNEL_IF_RE.fullmatch(expr)
        if m:
            return (USERSPACE, KERNEL) if m.group(1) else (KERNEL, USERSPACE)
    return None, None


def branch_labels(text):
    """Branch label in force on each line of comment-free text (None when unlabelled).

    Directive lines themselves are unlabelled. Only __KERNEL__ conditionals give a
    label; an enclosing label applies inside nested conditionals that carry none.
    """
    labels = []
    stack = []  # entries: [then_label, else_label, label_in_current_branch]
    for line in text.split("\n"):
        m = DIRECTIVE_RE.match(line)
        if m:
            kind, expr = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                then_label, else_label = kernel_branch_labels(kind, expr)
                stack.append([then_label, else_label, then_label])
            elif stack:
                top = stack[-1]
                if kind == "elif":
                    top[1] = None
                    top[2] = None
                elif kind == "else":
                    top[2] = top[1]
                elif kind == "endif":
                    stack.pop()
            labels.append(None)
            continue
        label = None
        for entry in reversed(stack):
            if entry[2] is not None:
                label = entry[2]
                break
        labels.append(label)
    return labels


def weak_declarations(src_text, service_names=()):
    """name -> line index of the first weak declaration of nt_svc_<name> in one file.

    A declaration with the weak attribute gives the symbol weak binding for the rest
    of the translation unit, so a definition after it is weak too (a strong
    definition elsewhere still wins at link time). Two forms appear in the NT
    sources: a prototype carrying the attribute, and the NT_SERVICE macro that
    declares every table entry weak and is then expanded by including
    nt_service_list.h. Directives are read from the original text, because
    stripping blanks the quoted include name.
    """
    first = {}
    for m in WEAK_PROTO_RE.finditer(strip_comments_and_strings(src_text)):
        name = m.group(1) or m.group(2)
        first.setdefault(name, strip_comments_and_strings(src_text).count("\n", 0, m.start()))
    macro_weak = False
    for i, line in enumerate(src_text.split("\n")):
        define = DEFINE_SERVICE_RE.match(line)
        if define:
            macro_weak = "nt_svc_" in define.group(1) and bool(WEAK_ATTR_RE.search(define.group(1)))
        elif UNDEF_SERVICE_RE.match(line):
            macro_weak = False
        elif macro_weak and INCLUDE_LIST_RE.match(line):
            for name in service_names:
                first.setdefault(name, i)
    return first


def scan_definitions_with_branch(src_text, service_names=()):
    """Yield (name, is_weak, body, label) for each nt_svc_ definition in one file.

    service_names are the table entries, used for the macro form of a weak declaration
    (see weak_declarations).
    """
    text = strip_comments_and_strings(src_text)
    labels = branch_labels(text)
    weak_from = weak_declarations(src_text, service_names)
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
        line = text.count("\n", 0, m.start())
        name = m.group("name")
        is_weak = bool(m.group("attr")) or (name in weak_from and weak_from[name] < line)
        yield name, is_weak, body, labels[line]


def scan_definitions(src_text):
    """Yield (name, is_weak, body) for each nt_svc_ definition in one file."""
    for name, is_weak, body, _label in scan_definitions_with_branch(src_text):
        yield name, is_weak, body


def scan_functions(src_text):
    """Yield the name of each function defined in one file, excluding nt_svc_ wrappers.

    Preprocessor lines are skipped, so a function-like macro body is not read as
    a function. Keywords followed by a parenthesis are not function names.
    """
    text = DIRECTIVE_LINE_RE.sub(lambda m: " " * len(m.group(0)),
                                 strip_comments_and_strings(src_text))
    for m in FUNC_HEAD_RE.finditer(text):
        name = m.group(1)
        if name in C_KEYWORDS or name.startswith("nt_svc_"):
            continue
        close = match_close(text, m.end() - 1, "(", ")")
        if close < 0:
            continue
        after = close + 1
        while after < len(text) and text[after] in " \t\r\n":
            after += 1
        if after < len(text) and text[after] == "{":
            yield name


def norm_name(name):
    """Name key for matching a service to a function: lower case, no underscores,
    no leading "nt", no trailing "internal" or "impl"."""
    s = name.lower().replace("_", "")
    if s.startswith("nt"):
        s = s[2:]
    for suffix in IMPL_SUFFIXES:
        if s.endswith(suffix) and len(s) > len(suffix):
            s = s[:-len(suffix)]
            break
    return s


def scan_tree(root):
    """Return (handlers, impls) for the handler directory under root.

    handlers: service name -> {"strong": (file, class) or None,
                               "weak": file or None,
                               "labelled": {label: (file, class)}}
              "strong" is the first strong definition in file order; "labelled"
              holds the first strong definition under each __KERNEL__ branch;
              "plain" is the first strong definition outside any such branch.
    impls:    normalised name -> [(file, function name)] in file order, for
              functions that are not nt_svc_ wrappers.
    """
    handlers = {}
    impls = {}
    hdir = os.path.join(root, HANDLER_DIR)
    if not os.path.isdir(hdir):
        raise OSError(f"no handler directory: {hdir}")
    names = service_names_of(root)
    for fname in sorted(os.listdir(hdir)):
        if not fname.endswith(".c") or fname.endswith("_test.c"):
            continue
        path = os.path.join(hdir, fname)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        rel = f"{HANDLER_DIR}/{fname}"
        for name, is_weak, body, label in scan_definitions_with_branch(text, names):
            entry = handlers.setdefault(name, {"strong": None, "weak": None,
                                               "labelled": {}, "plain": None})
            if is_weak:
                entry["weak"] = entry["weak"] or rel
                continue
            cls = body_class(body)
            if entry["strong"] is None:
                entry["strong"] = (rel, cls)
            if label is None:
                entry["plain"] = entry["plain"] or (rel, cls)
            elif label not in entry["labelled"]:
                entry["labelled"][label] = (rel, cls)
        for func in scan_functions(text):
            impls.setdefault(norm_name(func), []).append((rel, func))
    return handlers, impls


def collect_handlers(root):
    """Map service name -> handler record (see scan_tree)."""
    return scan_tree(root)[0]


def service_names_of(root):
    """Table entry names in order, or [] when the service list is absent."""
    try:
        return [svc["name"] for svc in parse_services(root)]
    except OSError:
        return []


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


def classify(services, handlers, impls=None):
    impls = impls or {}
    rows = []
    for svc in services:
        h = handlers.get(svc["name"]) or {"strong": None, "weak": None, "labelled": {}, "plain": None}
        lab = h["labelled"]
        key = norm_name(svc["name"])
        unreachable_file = impls[key][0][0] if key in impls else None
        if USERSPACE in lab and KERNEL in lab:
            user_file, user_cls = lab[USERSPACE]
            kernel_file, kernel_cls = lab[KERNEL]
            status = hybrid_status(user_cls, kernel_cls)
            file = ", ".join(dict.fromkeys([user_file, kernel_file]))
        elif h["strong"] is not None:
            file, status = h["strong"]
        elif h["weak"] is not None:
            file, status = h["weak"], WEAK
        elif key in impls:
            file, _func = impls[key][0]
            status = UNREACHABLE
        else:
            file, status = "", MISSING
        kernel_file, kernel_cls = branch_class(h, unreachable_file, KERNEL)
        user_file, user_cls = branch_class(h, unreachable_file, USERSPACE)
        rows.append({**svc, "status": status, "file": file, "area": area_of(svc["name"]),
                     "kernel_class": kernel_cls, "kernel_file": kernel_file,
                     "userspace_class": user_cls})
    return rows


def branch_class(h, unreachable_file, branch):
    """(file, class) that one build branch sees for a service.

    branch is KERNEL or USERSPACE. A definition under a __KERNEL__ label belongs
    to that branch only. An unlabelled strong definition is compiled into both
    builds. Without one, the branch falls back to the weak definition (WEAK),
    then to an unreferenced implementation (UNREACHABLE), then to nothing (MISSING).
    """
    lab = h["labelled"]
    if branch in lab:
        return lab[branch]
    if h.get("plain") is not None:
        return h["plain"]
    if h["weak"] is not None:
        return h["weak"], WEAK
    if unreachable_file is not None:
        return unreachable_file, UNREACHABLE
    return "", MISSING


def ordered_statuses(counts):
    """Base statuses first, then any hybrid statuses in sorted order."""
    extra = sorted(s for s in counts if s not in STATUSES)
    return list(STATUSES) + extra


def summarize(rows):
    counts = {s: 0 for s in STATUSES}
    for r in rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    return counts


def area_order(rows):
    seen = []
    for area, _ in AREA_PATTERNS:
        seen.append(area)
    seen.append(OTHER_AREA)
    return [a for a in seen if any(r["area"] == a for r in rows)]


def render_json(rows):
    """One record per table entry, in table order. kernel_class is the class the
    kernel build (__KERNEL__ branch) sees; userspace_class the userspace one."""
    records = []
    for r in rows:
        records.append({
            "ssn": f"0x{r['ssn']:04x}",
            "name": r["name"],
            "argc": r["argc"],
            "area": r["area"],
            "status": r["status"],
            "kernel_class": r["kernel_class"],
            "kernel_file": r["kernel_file"],
            "userspace_class": r["userspace_class"],
        })
    return json.dumps({"services": records}, indent=2) + "\n"


def render_text(rows):
    counts = summarize(rows)
    lines = [f"services: {len(rows)}"]
    for s in ordered_statuses(counts):
        lines.append(f"  {s}: {counts[s]}")
    return "\n".join(lines) + "\n"


RULES_MD = (
    "Each service in `src/kernel/nt/nt_service_list.h` is classified from the",
    "`NTSTATUS nt_svc_<Name>(...)` definitions in `src/kernel/nt/*.c`. Test sources",
    "(`*_test.c`) and prototypes are ignored. Comments and string literals are",
    "removed first. Linker rules apply: a strong definition overrides a weak one.",
    "",
    "### Classes",
    "",
    "- `IMPLEMENTED`: a strong definition whose body is not a refusal.",
    "- `STUB-NOT-IMPLEMENTED`: a strong definition whose body is only",
    "  `return STATUS_NOT_IMPLEMENTED;` or `return STATUS_NOT_SUPPORTED;`. Void casts",
    "  of unused arguments are allowed.",
    "- `REFUSES`: a strong definition whose body refuses after argument validation.",
    "  Zero or more `if (...) return V;` statements with `V` one of",
    "  `STATUS_INVALID_PARAMETER`, `STATUS_INVALID_HANDLE`, `STATUS_INVALID_ADDRESS`,",
    "  then exactly one `return R;` with `R` one of `STATUS_NOT_IMPLEMENTED`,",
    "  `STATUS_NOT_SUPPORTED`, `STATUS_INVALID_DEVICE_REQUEST`, `STATUS_ACCESS_DENIED`.",
    "  Any other statement makes the body `IMPLEMENTED`. A bare stub return is `STUB`.",
    "- `WEAK-FALLBACK`: only weak definitions exist. A definition is weak when it",
    "  carries `__attribute__((weak))`, or when a weak declaration of the same name",
    "  comes before it in the file (a prototype with the attribute, or the `NT_SERVICE`",
    "  macro that declares the table weak, followed by `#include` of the list).",
    "- `UNREACHABLE-WRAPPER-MISSING`: no `nt_svc_<Name>` definition, but a function",
    "  in `src/kernel/nt/*.c` matches the name (lower case, no underscores, leading",
    "  `nt` and trailing `internal` or `impl` removed). The implementation exists",
    "  but nothing in the table reaches it.",
    "- `MISSING`: no definition; the table entry is NULL.",
    "- `X (userspace) / Y (kernel)`: a hybrid. The service has a strong definition in",
    "  each branch of a conditional on `__KERNEL__`. `X` and `Y` are the classes of",
    "  the two branches.",
    "",
    "Preprocessor: only conditionals on `__KERNEL__` are evaluated (`#ifdef`,",
    "`#ifndef`, `#if defined(...)`, `#if !defined(...)`, with `#else`). An `#elif`",
    "makes its branch unlabelled. Other conditionals are read as one. Without a",
    "hybrid, the first strong definition in file order decides the class.",
)


def render_markdown(rows):
    counts = summarize(rows)
    out = []
    out.append("# NT service handler inventory")
    out.append("")
    out.append("<!-- Generated by tools/service_inventory.py --markdown. Do not edit by hand. -->")
    out.append("")
    out.extend(RULES_MD)
    out.append("")
    out.append("## Counts")
    out.append("")
    out.append("| Status | Services |")
    out.append("|---|---:|")
    out.append(f"| total | {len(rows)} |")
    for s in ordered_statuses(counts):
        out.append(f"| {s} | {counts[s]} |")
    out.append("")
    out.append("## By area")
    out.append("")
    out.append(f"| Area | Total | IMPLEMENTED | {STUB} | {REFUSES} | {WEAK} | {UNREACHABLE} | {MISSING} | hybrid |")
    out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for area in area_order(rows):
        sub = [r for r in rows if r["area"] == area]
        c = summarize(sub)
        hybrids = sum(1 for r in sub if is_hybrid(r["status"]))
        out.append(f"| {area} | {len(sub)} | {c[IMPLEMENTED]} | {c[STUB]} | {c[REFUSES]} | "
                   f"{c[WEAK]} | {c[UNREACHABLE]} | {c[MISSING]} | {hybrids} |")
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
    handlers, impls = scan_tree(root)
    return classify(services, handlers, impls)


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
NT_SERVICE(0x0010, NtCreatePort, 2)
NT_SERVICE(0x0011, NtDeniedOnly, 1)
NT_SERVICE(0x0012, NtValidatedRefusal, 2)
NT_SERVICE(0x0013, NtQueryDirectoryObject, 3)
NT_SERVICE(0x0014, NtRemovedWrapper, 1)
NT_SERVICE(0x0015, NtGuardedRefusal, 1)
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

# Userspace branch implements, kernel branch refuses: a hybrid.
FIXTURE_BRANCH = """\
#ifdef __KERNEL__
NTSTATUS nt_svc_NtCreatePort(PHANDLE a, ULONG b)
{
    (void)a; (void)b;
    return STATUS_NOT_IMPLEMENTED;
}
#else
NTSTATUS nt_svc_NtCreatePort(PHANDLE a, ULONG b)
{
    if (!a) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}
#endif
"""

# Refusals: a bare non-stub refusal, and a validated refusal.
FIXTURE_REFUSE = """\
NTSTATUS nt_svc_NtDeniedOnly(PHANDLE a) { (void)a; return STATUS_ACCESS_DENIED; }
NTSTATUS nt_svc_NtValidatedRefusal(PHANDLE a, ULONG b)
{
    if (!a) return STATUS_INVALID_HANDLE;
    (void)b;
    return STATUS_INVALID_DEVICE_REQUEST;
}
NTSTATUS nt_svc_NtGuardedRefusal(ULONG b)
{
    if (b) return STATUS_NOT_IMPLEMENTED;
    return STATUS_NOT_SUPPORTED;
}
"""

# An implementation under another name, and no nt_svc_ wrapper.
FIXTURE_IMPL_OTHER = """\
#define NT_MACRO_BODY(a) { return 0; }
static NTSTATUS nt_query_directory_object_internal(ULONG a)
{
    (void)a;
    return STATUS_SUCCESS;
}
"""

EXPECTED = {
    "NtYieldExecution": IMPLEMENTED,
    "NtQueryPerformanceCounter": STUB,
    "NtOpenEvent": WEAK,
    "NtQuerySystemInformation": REFUSES,
    "NtAccessCheck": MISSING,
    "NtAcceptConnectPort": STUB,
    "NtWeakOnly": WEAK,
    "NtTrulyAbsent": MISSING,
    "NtCreatePort": hybrid_status(IMPLEMENTED, STUB),
    "NtDeniedOnly": REFUSES,
    "NtValidatedRefusal": REFUSES,
    "NtQueryDirectoryObject": UNREACHABLE,
    "NtRemovedWrapper": MISSING,
    "NtGuardedRefusal": IMPLEMENTED,
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
    check(refusal_class(" return STATUS_ACCESS_DENIED; ") == REFUSES, "refuses: bare denied")
    check(refusal_class(" if (!a) return STATUS_INVALID_PARAMETER; return STATUS_NOT_SUPPORTED; ")
          == REFUSES, "refuses: validated stub status")
    check(refusal_class(" if (!a) return STATUS_NOT_SUPPORTED; return STATUS_NOT_SUPPORTED; ") is None,
          "refuses: conditional refusal is not a refusal body")
    check(refusal_class(" if (!a) return STATUS_INVALID_PARAMETER; x = 1; return STATUS_NOT_SUPPORTED; ")
          is None, "refuses: other statement disqualifies")
    check(refusal_class(" if (!a) return STATUS_INVALID_INFO_CLASS; return STATUS_NOT_SUPPORTED; ")
          is None, "refuses: validation status must be in the validation set")
    check(refusal_class(" return STATUS_INVALID_DEVICE_REQUEST; ") == REFUSES, "refuses: bare device")
    check(refusal_class(" return STATUS_SUCCESS; ") is None, "refuses: success")
    check(refusal_class(" return STATUS_INVALID_INFO_CLASS; ") == REFUSES,
          "refuses: bare invalid info class refuses every class")
    check(refusal_class(" switch (c) { default: return STATUS_INVALID_INFO_CLASS; } ") is None,
          "refuses: class dispatch is not a refusal")
    check(norm_name("NtQueryDirectoryObject") == norm_name("nt_query_directory_object_internal"),
          "norm: internal suffix and underscores")
    check(kernel_branch_labels("ifdef", " __KERNEL__ ") == (KERNEL, USERSPACE), "branch: ifdef")
    check(kernel_branch_labels("ifndef", "__KERNEL__") == (USERSPACE, KERNEL), "branch: ifndef")
    check(kernel_branch_labels("if", "!defined(__KERNEL__)") == (USERSPACE, KERNEL), "branch: if !defined")
    check(kernel_branch_labels("if", "defined(__linux__)") == (None, None), "branch: other macro")
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
            "branch.c": FIXTURE_BRANCH,
            "refuse.c": FIXTURE_REFUSE,
            "other.c": FIXTURE_IMPL_OTHER,
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
        check(counts[IMPLEMENTED] == 2 and counts[STUB] == 2 and counts[REFUSES] == 3
              and counts[WEAK] == 2 and counts[UNREACHABLE] == 1 and counts[MISSING] == 3
              and counts[hybrid_status(IMPLEMENTED, STUB)] == 1,
              f"counts {counts}")
        md = render_markdown(rows)
        check("## Area: system" in md and "| 0x0046 | `NtYieldExecution` |" in md,
              "markdown content")
        check("### Classes" in md and "UNREACHABLE-WRAPPER-MISSING" in md, "markdown rules")
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
    fmt.add_argument("--json", action="store_true", help="emit per-service JSON records")
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
    if args.markdown:
        text = render_markdown(rows)
    elif args.json:
        text = render_json(rows)
    else:
        text = render_text(rows)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
