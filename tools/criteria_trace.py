#!/usr/bin/env python3
"""Traceability matrix from the Definition-of-Done criteria to repository evidence.

Reads tests/conformance/criteria.json (423 criteria, see
docs/phase0/DEFINITION-OF-DONE.md) and scans the files under tests/ for the
criterion IDs they name. Each criterion gets one class, the first that applies:

  HAS-HARNESS-ENTRY  tests/harness/registry.yaml registers a runnable command
                     for the ID (not "unimplemented", not skipped)
  HAS-HOST-TEST      a code file under tests/ (outside tests/unit) names the ID
  REFERENCED-IN-DOCS named elsewhere under tests/: documents, data, comments,
                     unimplemented registry entries, tests/unit self-tests
  NO-EVIDENCE        no file under tests/ names the ID

This is traceability, not results. A class is evidence that something names
the criterion; it is never a pass. The result column stays "unknown" unless a
repository record states a status (criteria.json currently has none).

Detection is textual. A probe that exercises a criterion without naming its ID
is not seen. "DOD-PE-A010/B010" counts both IDs. The registry is read as text,
so PyYAML is not needed.

Usage:
  criteria_trace.py                     one-line-per-class summary on stdout
  criteria_trace.py --json              JSON document on stdout
  criteria_trace.py --markdown          Markdown document on stdout
  criteria_trace.py --markdown --out docs/status/TRACEABILITY.md
  criteria_trace.py --selftest          built-in checks on synthetic data

Exit status: 0 success, 1 a file cannot be read or the selftest fails.
Standard library only.
"""
import argparse
import json
import re
import subprocess
import sys
from collections import Counter, OrderedDict
from pathlib import Path, PurePosixPath

REPO = Path(__file__).resolve().parents[1]
CRITERIA_PATH = REPO / "tests" / "conformance" / "criteria.json"
TESTS_DIR = "tests"
REGISTRY_REL = "tests/harness/registry.yaml"
# Files that describe the criteria rather than exercise them. They name every
# ID, so counting them would mark every criterion as referenced.
SPEC_FILES = frozenset({"tests/conformance/criteria.json", "tests/conformance/thresholds.json"})
UNIT_PREFIX = "tests/unit/"
CODE_SUFFIXES = frozenset({".c", ".h", ".py", ".sh", ".ps1", ".js", ".ts", ".cs", ".mk"})
CODE_NAMES = frozenset({"Makefile", "makefile"})
MAX_BYTES = 2 * 1024 * 1024
INACTIVE_COMMANDS = frozenset({"", "null", "~", "unimplemented"})
REQUIRED_FIELDS = ("id", "domain", "tier", "dependency_class", "criterion", "status")
PLACEHOLDER_STATUSES = frozenset({"", "not_implemented"})

HOST = "HAS-HOST-TEST"
HARNESS = "HAS-HARNESS-ENTRY"
DOCS = "REFERENCED-IN-DOCS"
NONE = "NO-EVIDENCE"
CLASSES = (HOST, HARNESS, DOCS, NONE)
TOP_N = 20

# DOD-<DOMAIN>-<A|B><NNN>, optionally followed by "/<B|A><NNN>" shorthand.
ID_RE = re.compile(r"DOD-([A-Z]+)-([AB]\d{3})(?!\d)((?:\s*/\s*[AB]\d{3}(?!\d))*)")
SUFFIX_RE = re.compile(r"[AB]\d{3}")
ENTRY_RE = re.compile(r"^  (DOD-[A-Z]+-[AB]\d{3}):\s*(?:#.*)?$")


def extract_ids(text):
    """Set of criterion IDs named in text, shorthand ranges expanded."""
    ids = set()
    for m in ID_RE.finditer(text):
        domain = m.group(1)
        ids.add(f"DOD-{domain}-{m.group(2)}")
        for token in SUFFIX_RE.findall(m.group(3)):
            ids.add(f"DOD-{domain}-{token}")
    return ids


def parse_registry(text):
    """{id: {"command": str or None, "skip": bool}} for each top-level registry key."""
    entries = {}
    current = None
    for line in text.splitlines():
        m = ENTRY_RE.match(line)
        if m:
            current = m.group(1)
            entries[current] = {"command": None, "skip": False}
            continue
        if current is None or not line.startswith("    ") or line.strip().startswith("#"):
            if line.strip() and not line.startswith("    "):
                current = None
            continue
        key, _, value = line.strip().partition(":")
        value = value.split(" #")[0].strip()
        if key == "command":
            entries[current]["command"] = value
        elif key == "skip":
            entries[current]["skip"] = value.lower() == "true"
    return entries


def is_active(entry):
    """True when the registry entry names a command that may run."""
    if entry["skip"] or entry["command"] is None:
        return False
    return entry["command"].strip().strip("\"'").lower() not in INACTIVE_COMMANDS


def is_code(rel):
    path = PurePosixPath(rel)
    return path.suffix in CODE_SUFFIXES or path.name in CODE_NAMES


def status_of(entry):
    raw = (entry.get("status") or "").strip()
    return "unknown" if raw in PLACEHOLDER_STATUSES else raw


def load_criteria(path=CRITERIA_PATH):
    """Load and minimally validate the criteria list. Raises ValueError."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ValueError("criteria file must hold a JSON list")
    seen = set()
    for i, entry in enumerate(data):
        missing = [k for k in REQUIRED_FIELDS if k not in entry]
        if missing:
            raise ValueError(f"entry {i} ({entry.get('id', '?')}) lacks {missing}")
        if entry["id"] in seen:
            raise ValueError(f"duplicate id {entry['id']}")
        seen.add(entry["id"])
    return data


def build_matrix(criteria, registry_text, sources):
    """Classify every criterion. Returns (rows, summary).

    sources is an iterable of (repo-relative path, text) for the files under
    tests/ other than the registry and the spec files. Pure: no I/O.
    """
    evidence = {c["id"]: {"host": set(), "docs": set(), "harness": set()} for c in criteria}
    entries = parse_registry(registry_text)
    for cid in extract_ids(registry_text):
        if cid in entries and is_active(entries[cid]):
            continue
        if cid in evidence:
            evidence[cid]["docs"].add(REGISTRY_REL)
    for cid, entry in entries.items():
        if cid in evidence and is_active(entry):
            evidence[cid]["harness"].add(REGISTRY_REL)

    for rel, text in sources:
        if rel in SPEC_FILES or rel == REGISTRY_REL:
            continue
        kind = "host" if is_code(rel) and not rel.startswith(UNIT_PREFIX) else "docs"
        for cid in extract_ids(text):
            if cid in evidence:
                evidence[cid][kind].add(rel)

    rows = []
    for c in criteria:
        cid = c["id"]
        ev = evidence[cid]
        if ev["harness"]:
            cls = HARNESS
        elif ev["host"]:
            cls = HOST
        elif ev["docs"]:
            cls = DOCS
        else:
            cls = NONE
        if cid in entries:
            registry_state = "active" if is_active(entries[cid]) else "placeholder"
        else:
            registry_state = "absent"
        rows.append(OrderedDict([
            ("id", cid),
            ("area", c["domain"]),
            ("description", " ".join(str(c["criterion"]).split())),
            ("tier", c["tier"]),
            ("dependency_class", c["dependency_class"]),
            ("classification", cls),
            ("result", status_of(c)),
            ("registry", registry_state),
            ("host_tests", sorted(ev["host"])),
            ("harness_entries", sorted(ev["harness"])),
            ("doc_refs", sorted(ev["docs"])),
        ]))
    return rows, summarize(rows)


def coverage_rank(rows, n=TOP_N):
    """Criteria with the most executable sources, then most doc sources, then ID."""
    scored = [r for r in rows if r["classification"] != NONE]
    scored.sort(key=lambda r: (-(len(r["host_tests"]) + len(r["harness_entries"])),
                               -len(r["doc_refs"]), r["id"]))
    return [OrderedDict([
        ("id", r["id"]), ("area", r["area"]), ("tier", r["tier"]),
        ("classification", r["classification"]),
        ("host_tests", r["host_tests"]), ("harness_entries", r["harness_entries"]),
        ("doc_refs", r["doc_refs"]),
    ]) for r in scored[:n]]


def _class_counts(rows):
    counts = Counter(r["classification"] for r in rows)
    return OrderedDict((cls, counts.get(cls, 0)) for cls in CLASSES)


def summarize(rows):
    by_tier = OrderedDict()
    for tier in sorted({r["tier"] for r in rows}):
        by_tier[tier] = _class_counts([r for r in rows if r["tier"] == tier])
    by_area = OrderedDict()
    for area in sorted({r["area"] for r in rows}):
        by_area[area] = _class_counts([r for r in rows if r["area"] == area])
    return OrderedDict([
        ("total", len(rows)),
        ("classification_counts", _class_counts(rows)),
        ("result_counts", dict(sorted(Counter(r["result"] for r in rows).items()))),
        ("by_tier", by_tier),
        ("by_area", by_area),
        ("top_coverage", coverage_rank(rows)),
    ])


def list_test_files(root=REPO):
    """Repo-relative paths under tests/: tracked files from git, else a walk."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", TESTS_DIR],
            capture_output=True, check=True,
        ).stdout
        return sorted(n for n in out.decode("utf-8", "replace").split("\0") if n)
    except (OSError, subprocess.CalledProcessError):
        base = Path(root) / TESTS_DIR
        if not base.is_dir():
            return []
        return sorted(p.relative_to(root).as_posix() for p in base.rglob("*") if p.is_file())


def read_source(path):
    """Text of a file, or None when it is binary, too large or unreadable."""
    try:
        if path.stat().st_size > MAX_BYTES:
            return None
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", "replace")


def collect(root=REPO):
    """(criteria, registry_text, sources) read from a repository tree."""
    root = Path(root)
    criteria = load_criteria(root / "tests" / "conformance" / "criteria.json")
    registry = root / REGISTRY_REL
    registry_text = registry.read_text(encoding="utf-8")
    sources = []
    for rel in list_test_files(root):
        if rel in SPEC_FILES or rel == REGISTRY_REL:
            continue
        text = read_source(root / rel)
        if text is not None:
            sources.append((rel, text))
    return criteria, registry_text, sources


def md_cell(text):
    return str(text).replace("|", "\\|")


def _sources_cell(paths, limit=2):
    if not paths:
        return "-"
    shown = ", ".join(f"`{p}`" for p in paths[:limit])
    extra = len(paths) - limit
    return shown + (f" (+{extra})" if extra > 0 else "")


def render_json(rows, summary):
    doc = OrderedDict([("summary", summary), ("criteria", rows)])
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def _table(header, body_rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    lines += ["| " + " | ".join(md_cell(c) for c in row) + " |" for row in body_rows]
    return lines


def render_markdown(rows, summary):
    counts = summary["classification_counts"]
    lines = [
        "# Criteria traceability matrix",
        "",
        "Generated by `tools/criteria_trace.py` from `tests/conformance/criteria.json`, "
        "`tests/harness/registry.yaml` and the files under `tests/`. Do not edit by hand; "
        "regenerate with the script.",
        "",
        "This is a traceability matrix, not a results report. A named reference or a "
        "registry entry is not a pass. Every result stays `unknown` until a repository "
        "record states one.",
        "",
        "Classes, first match wins:",
        "",
        f"- `{HARNESS}`: `tests/harness/registry.yaml` registers a runnable command for the ID.",
        f"- `{HOST}`: a code file under `tests/` (outside `tests/unit`) names the ID.",
        f"- `{DOCS}`: named elsewhere under `tests/` (documents, data, comments, "
        "unimplemented registry entries, `tests/unit` self-tests).",
        f"- `{NONE}`: no file under `tests/` names the ID.",
        "",
        "Detection is textual, so a probe that covers a criterion without naming its ID "
        "is not counted. `criteria.json` and `thresholds.json` are excluded because they "
        "describe the criteria rather than exercise them.",
        "",
        "## Summary",
        "",
        f"Total criteria: {summary['total']}",
        "",
    ]
    lines += _table(["Class", "Count"], [[k, v] for k, v in counts.items()])
    lines += ["", "Results: " + ", ".join(f"{k} {v}" for k, v in summary["result_counts"].items()),
              "", "## By tier", ""]
    lines += _table(["Tier"] + list(CLASSES) + ["Total"], [
        [tier] + list(c.values()) + [sum(c.values())] for tier, c in summary["by_tier"].items()
    ])
    lines += ["", "## By area", ""]
    lines += _table(["Area"] + list(CLASSES) + ["Total"], [
        [area] + list(c.values()) + [sum(c.values())] for area, c in summary["by_area"].items()
    ])
    lines += ["", f"## Most existing coverage (top {TOP_N})", "",
              "Ranked by distinct host-test and harness sources, then by doc sources, then ID.",
              ""]
    top = summary["top_coverage"]
    if top:
        lines += _table(["#", "ID", "Area", "Tier", "Class", "Host/harness", "Docs", "Files"], [
            [i, r["id"], r["area"], r["tier"], r["classification"],
             len(r["host_tests"]) + len(r["harness_entries"]), len(r["doc_refs"]),
             _sources_cell(r["host_tests"] + r["harness_entries"] + r["doc_refs"])]
            for i, r in enumerate(top, 1)
        ])
    else:
        lines.append("No criterion has any evidence.")
    lines += ["", "## NO-EVIDENCE criteria by area", ""]
    missing = [r for r in rows if r["classification"] == NONE]
    if not missing:
        lines.append("Every criterion has at least one reference.")
    for area in sorted({r["area"] for r in missing}):
        group = [r for r in missing if r["area"] == area]
        lines += [f"### {area} ({len(group)})", ""]
        lines += _table(["ID", "Tier", "Dependency", "Description"], [
            [r["id"], r["tier"], r["dependency_class"], r["description"]] for r in group
        ])
        lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def render_text(rows, summary):
    out = [f"criteria: {summary['total']}"]
    out += [f"{cls}: {n}" for cls, n in summary["classification_counts"].items()]
    out.append("results: " + ", ".join(f"{k} {v}" for k, v in summary["result_counts"].items()))
    return "\n".join(out) + "\n"


def selftest():
    """Run checks on synthetic data. Returns the number of failed checks."""
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    check("plain id", extract_ids("see DOD-PE-A010 here") == {"DOD-PE-A010"})
    check("shorthand range", extract_ids("DOD-PE-A010/B010 need") == {"DOD-PE-A010", "DOD-PE-B010"})
    check("no partial number", extract_ids("DOD-PE-A0101") == set())

    registry = (
        "tests:\n"
        "  # DOD-NT-B001 is intentionally UNIMPLEMENTED.\n"
        "  DOD-AA-A002:\n"
        "    command: [\"python3\", \"tests/kernel/x.py\"]\n"
        "    timeout: 60\n"
        "  DOD-AA-B001:\n"
        "    command: unimplemented\n"
    )
    entries = parse_registry(registry)
    check("active entry parsed", is_active(entries["DOD-AA-A002"]))
    check("unimplemented entry inactive", not is_active(entries["DOD-AA-B001"]))
    check("entry skip flag", not is_active({"command": "x", "skip": True}))

    criteria = [
        {"id": "DOD-AA-A001", "domain": "AA", "tier": "P0", "dependency_class": "SELF",
         "criterion": "Host | test", "status": "not_implemented"},
        {"id": "DOD-AA-A002", "domain": "AA", "tier": "P0", "dependency_class": "SELF",
         "criterion": "Harness", "status": "not_implemented"},
        {"id": "DOD-AA-A003", "domain": "AA", "tier": "P1", "dependency_class": "EXT-DOC",
         "criterion": "Docs only", "status": "not_implemented"},
        {"id": "DOD-AA-A004", "domain": "AA", "tier": "P1", "dependency_class": "EXT-RE",
         "criterion": "Nothing | here", "status": "not_implemented"},
        {"id": "DOD-AA-B001", "domain": "AA", "tier": "P2", "dependency_class": "SELF",
         "criterion": "Placeholder", "status": "not_implemented"},
        {"id": "DOD-BB-A001", "domain": "BB", "tier": "P3", "dependency_class": "EXT-GATED",
         "criterion": "Shorthand", "status": "pass"},
    ]
    sources = [
        ("tests/host/a.c", "/* DOD-AA-A001 */"),
        ("tests/harness/README.md", "Docs mention DOD-AA-A003."),
        ("tests/unit/test_x.py", "# DOD-AA-A003 fixture"),
        ("tests/conformance/criteria.json", "DOD-AA-A004 DOD-AA-B001 DOD-BB-A001"),
        ("tests/host/b.c", "DOD-BB-A001/B001"),
    ]
    rows, summary = build_matrix(criteria, registry, sources)
    by_id = {r["id"]: r for r in rows}
    check("host test class", by_id["DOD-AA-A001"]["classification"] == HOST)
    check("harness class", by_id["DOD-AA-A002"]["classification"] == HARNESS)
    check("docs class from tests/unit and docs", by_id["DOD-AA-A003"]["classification"] == DOCS)
    check("spec file ignored", by_id["DOD-AA-A004"]["classification"] == NONE)
    check("unimplemented entry is docs", by_id["DOD-AA-B001"]["classification"] == DOCS
          and by_id["DOD-AA-B001"]["registry"] == "placeholder")
    check("result stays unknown", by_id["DOD-AA-A001"]["result"] == "unknown")
    check("recorded pass is shown", by_id["DOD-BB-A001"]["result"] == "pass")
    check("host ref in code file", by_id["DOD-BB-A001"]["host_tests"] == ["tests/host/b.c"])
    check("counts", summary["classification_counts"] == {HOST: 2, HARNESS: 1, DOCS: 2, NONE: 1})
    check("tier table", summary["by_tier"]["P0"] == {HOST: 1, HARNESS: 1, DOCS: 0, NONE: 0})
    check("top coverage excludes none", all(r["classification"] != NONE
                                            for r in summary["top_coverage"]))

    md = render_markdown(rows, summary)
    check("markdown escapes pipe", "Nothing \\| here" in md)
    check("markdown lists no-evidence", "### AA (1)" in md and "DOD-AA-A004" in md)
    check("markdown states not a pass", "not a results report" in md)
    check("markdown has top table", "| # | ID |" in md)
    doc = json.loads(render_json(rows, summary))
    check("json round-trips", doc["criteria"][0]["id"] == "DOD-AA-A001")
    check("json summary total", doc["summary"]["total"] == 6)
    check("text summary", "criteria: 6" in render_text(rows, summary))

    if fails:
        for f in fails:
            print("FAIL:", f)
        print(f"selftest: {len(fails)} failed")
    else:
        print("selftest: all checks passed")
    return len(fails)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    fmt = parser.add_mutually_exclusive_group()
    fmt.add_argument("--json", action="store_true", help="emit JSON")
    fmt.add_argument("--markdown", action="store_true", help="emit Markdown")
    parser.add_argument("--out", help="write output to this file instead of stdout")
    parser.add_argument("--selftest", action="store_true", help="run built-in checks and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return 1 if selftest() else 0

    try:
        criteria, registry_text, sources = collect(REPO)
    except (OSError, ValueError) as exc:
        print(f"criteria_trace: error: cannot read repository inputs: {exc}", file=sys.stderr)
        return 1
    rows, summary = build_matrix(criteria, registry_text, sources)

    if args.json:
        text = render_json(rows, summary)
    elif args.markdown:
        text = render_markdown(rows, summary)
    else:
        text = render_text(rows, summary)

    if args.out:
        try:
            Path(args.out).write_text(text, encoding="utf-8", newline="\n")
        except OSError as exc:
            print(f"criteria_trace: error: cannot write {args.out}: {exc.strerror}",
                  file=sys.stderr)
            return 1
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
