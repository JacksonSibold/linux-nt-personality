#!/usr/bin/env python3
"""Audit OS-identity literals against tests/conformance/os-identity-policy.json.

Checks two things:

1. Every declared site (file, function, field) assigns the declared value to
   that field inside the named function body.
2. No identity field is assigned a literal anywhere under the scan roots
   outside a declared site. This is the "no per-call ad-hoc values" rule.

Exit codes: 0 all clean, 1 violations found, 2 the policy or a declared file
cannot be read.

Function bodies are located by brace matching and do not skip string or
character literals. The sources audited here do not put braces inside them.
"""
import argparse
import json
import pathlib
import re
import sys

LITERAL = r"(0x[0-9a-fA-F]+|\d+)"


def load_policy(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def parse_literal(text):
    return int(text, 16) if text.lower().startswith("0x") else int(text, 10)


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


def check_sites(root, policy):
    results = []
    fields = policy["fields"]
    for site in policy["sites"]:
        declared = fields[site["field"]]["value"]
        path = root / site["file"]
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            results.append(("missing-file", site, None, declared, str(exc)))
            continue
        span = function_span(lines, site["function"])
        if span is None:
            results.append(("missing-function", site, None, declared, "function not found"))
            continue
        assign = re.compile(r"(?:->|\.)\s*" + re.escape(site["field"]) + r"\s*=\s*" + LITERAL + r"\s*;")
        found = None
        for line in lines[span[0]:span[1] + 1]:
            m = assign.search(line)
            if m:
                found = parse_literal(m.group(1))
                break
        if found is None:
            results.append(("missing-assignment", site, None, declared, "no literal assignment"))
        elif found != declared:
            results.append(("mismatch", site, found, declared, ""))
        else:
            results.append(("ok", site, found, declared, ""))
    return results


def scan_undeclared(root, policy):
    fields = policy["fields"]
    field_alt = "|".join(re.escape(f) for f in fields)
    assign = re.compile(r"(?:->|\.)\s*(" + field_alt + r")\s*=\s*" + LITERAL + r"\s*;")
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
                findings.append((rel, idx + 1, m.group(1), parse_literal(m.group(2))))
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
    except (OSError, ValueError) as exc:
        print(f"cannot read policy {policy_path}: {exc}", file=sys.stderr)
        return 2

    violations = 0
    print(f"policy: {policy_path} (reference build {policy.get('reference_build')})")
    for state, site, found, declared, note in check_sites(root, policy):
        label = f"{site['file']}:{site['function']}.{site['field']}"
        if state == "ok":
            print(f"  ok        {label} = {found}")
        else:
            violations += 1
            shown = "-" if found is None else found
            print(f"  {state:<10} {label} found={shown} declared={declared} {note}".rstrip())
    for rel, line, field, value in scan_undeclared(root, policy):
        violations += 1
        print(f"  undeclared {rel}:{line} {field} = {value}")
    print(f"violations: {violations}")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
