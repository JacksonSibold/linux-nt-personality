#!/usr/bin/env python3
"""Audit OS-identity values against tests/conformance/os-identity-policy.json.

Checks four things:

1. Every declared site (file, function, field) assigns the declared value to
   that field inside the named function body. The value is a literal, or an
   identity macro defined in the policy's identity_header.
2. The identity header defines each field's symbol with the declared value.
   The header is the one shared definition of the identity (ADR 0075).
3. No identity field is assigned a literal anywhere under the scan roots
   outside a declared site.
4. No identity field is assigned an identity macro outside a declared site
   unless the header defines that macro with the declared value. Using the
   correct macro is the sanctioned route; a wrong or undefined one is flagged.

Exit codes: 0 all clean, 1 violations found, 2 the policy, the identity header
or a declared file cannot be read.

Function bodies are located by brace matching and do not skip string or
character literals. The sources audited here do not put braces inside them.
"""
import argparse
import json
import pathlib
import re
import sys

LITERAL = r"(0x[0-9a-fA-F]+|\d+)"
# A value is a literal or an identifier (an identity macro).
VALUE = r"(0x[0-9a-fA-F]+|\d+|[A-Za-z_]\w*)"
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(0x[0-9a-fA-F]+|\d+)\b", re.M)


def load_policy(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def parse_literal(text):
    return int(text, 16) if text.lower().startswith("0x") else int(text, 10)


def load_symbols(root, policy):
    """Map each identity macro in policy['identity_header'] to its integer value."""
    header = policy.get("identity_header")
    if not header:
        return {}
    text = (root / header).read_text(encoding="utf-8", errors="replace")
    return {m.group(1): parse_literal(m.group(2)) for m in DEFINE.finditer(text)}


def resolve(token, symbols):
    """Integer value of a literal or identity macro token, or None if neither."""
    if re.fullmatch(LITERAL, token):
        return parse_literal(token)
    return symbols.get(token)


def function_span(lines, name):
    """Return (start, end) line indexes of the body of function `name`, or None."""
    define = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for i, line in enumerate(lines):
        if define.search(line) and not line.rstrip().endswith(";"):
            # The definition's opening brace may be on this line or a later one.
            depth = 0
            opened = False
            for j in range(i, len(lines)):
                for ch in lines[j]:
                    if ch == "{":
                        depth += 1
                        opened = True
                    elif ch == "}":
                        depth -= 1
                        if opened and depth == 0:
                            return (i, j)
                if not opened and lines[j].rstrip().endswith(";"):
                    break
    return None


def check_header(policy, symbols):
    """Each field that names a symbol must be defined in the header with the declared value."""
    results = []
    header = policy.get("identity_header", "(none)")
    seen = set()
    for field, spec in policy["fields"].items():
        symbol = spec.get("symbol")
        if symbol is None or (symbol, spec["value"]) in seen:
            continue
        seen.add((symbol, spec["value"]))
        label = f"{header}:{symbol}"
        if symbol not in symbols:
            results.append(("missing-symbol", label, None, spec["value"], "not defined in identity header"))
        elif symbols[symbol] != spec["value"]:
            results.append(("mismatch", label, symbols[symbol], spec["value"], "header disagrees with policy"))
        else:
            results.append(("ok", label, symbols[symbol], spec["value"], ""))
    return results


def check_sites(root, policy, symbols):
    results = []
    fields = policy["fields"]
    for site in policy["sites"]:
        declared = fields[site["field"]]["value"]
        label = f"{site['file']}:{site['function']}.{site['field']}"
        path = root / site["file"]
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            results.append(("missing-file", label, None, declared, str(exc)))
            continue
        span = function_span(lines, site["function"])
        if span is None:
            results.append(("missing-function", label, None, declared, "function not found"))
            continue
        assign = re.compile(r"(?:->|\.)\s*" + re.escape(site["field"]) + r"\s*=\s*" + VALUE + r"\s*;")
        token = None
        for line in lines[span[0]:span[1] + 1]:
            m = assign.search(line)
            if m:
                token = m.group(1)
                break
        if token is None:
            results.append(("missing-assignment", label, None, declared, "no assignment"))
            continue
        found = resolve(token, symbols)
        if found is None:
            results.append(("unknown-symbol", label, None, declared, f"{token} is not defined in the identity header"))
        elif found != declared:
            results.append(("mismatch", label, found, declared, ""))
        else:
            results.append(("ok", label, found, declared, ""))
    return results


def scan_undeclared(root, policy, symbols=None):
    """Identity assignments outside declared sites that are not allowed.

    Returns (file, line, field, token) tuples. A literal is always flagged. A
    macro is flagged only if the header does not define it or it differs from
    the declared value.
    """
    if symbols is None:
        symbols = load_symbols(root, policy)
    fields = policy["fields"]
    field_alt = "|".join(re.escape(f) for f in fields)
    assign = re.compile(r"(?:->|\.)\s*(" + field_alt + r")\s*=\s*" + VALUE + r"\s*;")
    declared_spans = {}
    for site in policy["sites"]:
        path = root / site["file"]
        if not path.is_file():
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        span = function_span(lines, site["function"])
        if span:
            declared_spans.setdefault(site["file"], []).append(span)
    findings = []
    for scan_root in policy.get("scan_roots", ["src"]):
        base = root / scan_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".c", ".h") or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            spans = declared_spans.get(rel, [])
            for idx, line in enumerate(lines):
                m = assign.search(line)
                if not m:
                    continue
                if any(s <= idx <= e for s, e in spans):
                    continue
                field, token = m.group(1), m.group(2)
                if re.fullmatch(LITERAL, token):
                    flagged = True
                else:
                    value = symbols.get(token)
                    flagged = value is None or value != fields[field]["value"]
                if flagged:
                    findings.append((rel, idx + 1, field, token))
    return findings


def main(argv=None):
    here = pathlib.Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(here), help="repository root to audit")
    ap.add_argument("--policy", default=None, help="policy file (default: tests/conformance/os-identity-policy.json under root)")
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root).resolve()
    policy_path = pathlib.Path(args.policy) if args.policy else root / "tests" / "conformance" / "os-identity-policy.json"
    try:
        policy = load_policy(policy_path)
        symbols = load_symbols(root, policy)
    except (OSError, ValueError) as exc:
        print(f"cannot read policy {policy_path}: {exc}", file=sys.stderr)
        return 2

    violations = 0
    print(f"policy: {policy_path} (reference build {policy.get('reference_build')})")
    results = check_header(policy, symbols) + check_sites(root, policy, symbols)
    for state, label, found, declared, note in results:
        if state == "ok":
            print(f"  ok        {label} = {found}")
        else:
            violations += 1
            shown = "-" if found is None else found
            print(f"  {state:<10} {label} found={shown} declared={declared} {note}".rstrip())
    for rel, line, field, token in scan_undeclared(root, policy, symbols):
        violations += 1
        print(f"  undeclared {rel}:{line} {field} = {token}")
    print(f"violations: {violations}")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
