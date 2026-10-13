#!/usr/bin/env python3
"""Status report for the Definition-of-Done conformance criteria.

Reads tests/conformance/criteria.json (the machine-readable criteria set named
in docs/phase0/DEFINITION-OF-DONE.md) and emits one row per criterion:
id, area, description, status.

Per-criterion status comes only from a recorded value. The merge script writes
the placeholder "not_implemented" into every entry; that is not a verification
result, so such rows report "unknown". The aggregate pass count in README.md
(for example "3 of 423") is reported separately, because the README does not
say which criteria passed.

Usage:
  criteria_report.py                    plain-text table on stdout
  criteria_report.py --json             JSON document on stdout
  criteria_report.py --markdown         Markdown document on stdout
  criteria_report.py --markdown --out docs/status/CRITERIA.md
  criteria_report.py --selftest         built-in checks on synthetic data

Standard library only.
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CRITERIA_PATH = REPO / "tests" / "conformance" / "criteria.json"
README_PATH = REPO / "README.md"

REQUIRED_FIELDS = ("id", "domain", "tier", "dependency_class", "criterion", "status")
PLACEHOLDER_STATUSES = {"", "not_implemented"}
README_RE = re.compile(r"\*\*(\d+) of (\d+)\*\*\s+full conformance criteria passing")
ASSESSMENTS_PATH = REPO / "tools" / "criteria_evidence_assessments.yaml"
REGISTRY_PATH = REPO / "tests" / "harness" / "registry.yaml"
ASSESSMENT_RE = re.compile(r"^(DOD-[A-Z]+-[AB]\d{3}):[ \t]*\n[ \t]+verdict:[ \t]*(\w+)", re.MULTILINE)
REGISTRY_RE = re.compile(r"^  (DOD-[A-Z]+-[AB]\d{3}):[ \t]*$", re.MULTILINE)
# Reviewed NONE/PARTIAL/FULL verdicts (criteria_evidence_map.py) in matrix wording.
VERDICT_WORDS = {"FULL": "implemented", "PARTIAL": "partial", "NONE": "not demonstrated"}
BUNDLE_DIR = REPO / "docs" / "status" / "bundles"
BUNDLE_GLOB = "dodrun-*.json"
STATUS_DIR = REPO / "docs" / "status"
ASSESSMENT_LINK = "CRITERIA-EVIDENCE.md"
REGISTRY_LINK = "../../tests/harness/registry.yaml"
BOUNDARY_LINK = "../policy/CLAIM-BOUNDARY.md"
DRV_STATEMENT = "No third-party NT `.sys` executes in the Personality."


def load_assessments(path=ASSESSMENTS_PATH):
    """Map criterion ID to its matrix verdict wording. Empty when the file is absent."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError:
        return {}
    return {cid: VERDICT_WORDS.get(v, "not demonstrated")
            for cid, v in ASSESSMENT_RE.findall(text)}


def load_registered(path=REGISTRY_PATH):
    """Criterion IDs that have a registered command (uncommented entries under tests:)."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError:
        return set()
    return set(REGISTRY_RE.findall(text))


def load_dodrun_results(paths):
    """Map criterion ID to {'state', 'file'} from dodrun results documents.

    tests/harness/dodrun.py writes one results document per run: a JSON object whose
    "results" list holds one {"id", "state", ...} record per criterion. Files are read
    in the order given, so a later file replaces an earlier record for the same ID.
    Raises ValueError when a file is unreadable or does not have that shape.
    """
    found = {}
    for path in paths:
        path = Path(path)
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ValueError(f"cannot read dodrun results {path}: {exc}")
        results = doc.get("results") if isinstance(doc, dict) else None
        if not isinstance(results, list):
            raise ValueError(f"{path} has no 'results' list")
        for rec in results:
            if not (isinstance(rec, dict) and isinstance(rec.get("id"), str)
                    and isinstance(rec.get("state"), str)):
                raise ValueError(f"{path} has a result without a string 'id' and 'state'")
            found[rec["id"]] = {"state": rec["state"], "file": path}
    return found


def find_bundles(directory=BUNDLE_DIR):
    """The committed dodrun results documents, oldest first. Empty when none exist."""
    return sorted(Path(directory).glob(BUNDLE_GLOB))


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


def read_readme_count(path=README_PATH):
    """Return {'passing', 'total', 'source'} from the README note, or None."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError:
        return None
    m = README_RE.search(text)
    if not m:
        return None
    return {"passing": int(m.group(1)), "total": int(m.group(2)), "source": "README.md"}


def status_of(entry, results=None):
    """The dodrun state of the criterion when a result file holds one, else the recorded status."""
    if results and entry["id"] in results:
        return results[entry["id"]]["state"].lower()
    raw = (entry.get("status") or "").strip()
    return "unknown" if raw in PLACEHOLDER_STATUSES else raw


def verdict_of(cid, dependency, assessed, results=None):
    """Matrix verdict: the dodrun state when a result file holds one, else a reviewed assessment,
    else gated or not assessed. A recorded run is newer evidence than a review."""
    if results and cid in results:
        return f"dodrun {results[cid]['state'].lower()}"
    if cid in assessed:
        return assessed[cid]
    if dependency == "EXT-GATED":
        return "gated"
    return "not assessed"


def evidence_of(cid, assessed, registered, results=None):
    """Local link to the record that says what is known about the row. A dodrun result links
    its results document (the link text names the path from the repository root, which is
    how the docs lint finds a bundle); it is unsigned, so the row says so."""
    if results and cid in results:
        target = Path(results[cid]["file"])
        from_repo = Path(os.path.relpath(target, REPO)).as_posix()
        from_status = Path(os.path.relpath(target, STATUS_DIR)).as_posix()
        return f"[{from_repo}]({from_status}); unsigned"
    if cid in assessed:
        return f"[assessment]({ASSESSMENT_LINK}); no bundle"
    if cid in registered:
        return f"[registry]({REGISTRY_LINK}); no bundle"
    return f"none; [registry]({REGISTRY_LINK}) has no entry"


def owner_of(dependency):
    """The project owns every row except the EXT-GATED ones, whose artifacts a third party controls."""
    if dependency == "EXT-GATED":
        return "project owner (third party, DEFINITION-OF-DONE section 3.1)"
    return "project owner"


def build_rows(criteria, assessed=None, registered=None, results=None):
    assessed = assessed or {}
    registered = registered or set()
    results = results or {}
    rows = []
    for c in criteria:
        dep = c["dependency_class"]
        rows.append(OrderedDict([
            ("id", c["id"]),
            ("area", c["domain"]),
            ("description", " ".join(str(c["criterion"]).split())),
            ("tier", c["tier"]),
            ("dependency_class", dep),
            ("status", status_of(c, results)),
            ("verdict", verdict_of(c["id"], dep, assessed, results)),
            ("evidence", evidence_of(c["id"], assessed, registered, results)),
            ("owner", owner_of(dep)),
        ]))
    return rows


def summarize(rows, readme):
    return OrderedDict([
        ("total", len(rows)),
        ("status_counts", dict(sorted(Counter(r["status"] for r in rows).items()))),
        ("tier_counts", dict(sorted(Counter(r["tier"] for r in rows).items()))),
        ("dependency_counts", dict(sorted(Counter(r["dependency_class"] for r in rows).items()))),
        ("readme_passing", readme),
    ])


def render_json(rows, summary):
    doc = OrderedDict([("summary", summary), ("criteria", rows)])
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def md_cell(text):
    return str(text).replace("|", "\\|")


def render_markdown(rows, summary):
    readme = summary["readme_passing"]
    lines = [
        "# Conformance criteria status",
        "",
        "Generated by `tools/criteria_report.py` from `tests/conformance/criteria.json`.",
        "Do not edit by hand; regenerate with the script.",
        "",
    ]
    lines += [f"Claim boundary: {DRV_STATEMENT} See [CLAIM-BOUNDARY.md]({BOUNDARY_LINK}).", ""]
    if readme:
        lines.append(
            f"README.md records **{readme['passing']} of {readme['total']}** criteria passing, "
            "but no signed bundle is linked, so the count is unverified. It does not name them, "
            "so per-row status stays `unknown` until a result is recorded."
        )
    else:
        lines.append("README.md records no pass count.")
    lines += ["", f"Total criteria: {summary['total']}", "", "## Summary", "",
              "| Status | Count |", "|---|---|"]
    lines += [f"| {md_cell(k)} | {v} |" for k, v in summary["status_counts"].items()]
    lines += ["", "| Tier | Count |", "|---|---|"]
    lines += [f"| {md_cell(k)} | {v} |" for k, v in summary["tier_counts"].items()]
    lines += ["", "## Criteria", "",
              "| ID | Area | Description | Tier | Dependency | Status | Verdict | Evidence | Owner |",
              "|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append("| " + " | ".join([
            md_cell(r["id"]), md_cell(r["area"]), md_cell(r["description"]),
            md_cell(r["tier"]), md_cell(r["dependency_class"]), md_cell(r["status"]),
            md_cell(r["verdict"]), r["evidence"], md_cell(r["owner"]),
        ]) + " |")
    return "\n".join(lines) + "\n"


def render_text(rows, summary):
    headers = ["ID", "AREA", "TIER", "DEP", "STATUS", "DESCRIPTION"]
    table = [[r["id"], r["area"], r["tier"], r["dependency_class"], r["status"],
              r["description"]] for r in rows]
    widths = [max(len(h), *(len(row[i]) for row in table)) if table else len(h)
              for i, h in enumerate(headers[:-1])]
    out = []
    out.append("  ".join(h.ljust(w) for h, w in zip(headers[:-1], widths)) + "  " + headers[-1])
    for row in table:
        out.append("  ".join(v.ljust(w) for v, w in zip(row[:-1], widths)) + "  " + row[-1])
    out.append("")
    out.append(f"total: {summary['total']}  status: {summary['status_counts']}")
    readme = summary["readme_passing"]
    if readme:
        out.append(f"README records {readme['passing']} of {readme['total']} passing (ids not recorded)")
    return "\n".join(out) + "\n"


def selftest():
    """Run checks on synthetic data. Returns the number of failed checks."""
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    sample = [
        {"id": "DOD-AA-A001", "domain": "AA", "tier": "P0", "dependency_class": "SELF",
         "criterion": "Plain | pipe\n and newline", "status": "not_implemented"},
        {"id": "DOD-AA-A002", "domain": "AA", "tier": "P1", "dependency_class": "EXT-RE",
         "criterion": "Recorded pass", "status": "pass"},
        {"id": "DOD-BB-A001", "domain": "BB", "tier": "P3", "dependency_class": "EXT-GATED",
         "criterion": "No status field value", "status": ""},
    ]
    check("status placeholder maps to unknown", status_of(sample[0]) == "unknown")
    check("recorded status is kept", status_of(sample[1]) == "pass")
    check("empty status maps to unknown", status_of(sample[2]) == "unknown")

    rows = build_rows(sample)
    check("description collapses whitespace", rows[0]["description"] == "Plain | pipe and newline")
    readme = {"passing": 3, "total": 423, "source": "README.md"}
    summary = summarize(rows, readme)
    check("total counted", summary["total"] == 3)
    check("status counts", summary["status_counts"] == {"pass": 1, "unknown": 2})
    check("tier counts", summary["tier_counts"] == {"P0": 1, "P1": 1, "P3": 1})

    md = render_markdown(rows, summary)
    check("markdown escapes pipe", "Plain \\| pipe and newline" in md)
    check("markdown has criteria header", "| ID | Area | Description | Tier | Dependency | Status | Verdict | Evidence | Owner |" in md)
    check("markdown reports readme count", "**3 of 423**" in md)

    doc = json.loads(render_json(rows, summary))
    check("json round-trips", doc["criteria"][1]["status"] == "pass")
    check("json summary total", doc["summary"]["total"] == 3)

    txt = render_text(rows, summary)
    check("text lists every id", all(r["id"] in txt for r in rows))

    m = README_RE.search("only **3 of 423** full conformance criteria passing.")
    check("readme regex parses", m is not None and m.groups() == ("3", "423"))
    check("readme regex ignores other text",
          README_RE.search("**2 of 5** other things passing") is None)

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
    parser.add_argument("--results", action="append", type=Path, metavar="PATH",
                        help="dodrun results JSON to read statuses from (repeatable; "
                             "default: docs/status/bundles/dodrun-*.json)")
    parser.add_argument("--selftest", action="store_true", help="run built-in checks and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return 1 if selftest() else 0

    try:
        criteria = load_criteria(CRITERIA_PATH)
    except (OSError, ValueError) as exc:  # bad JSON, wrong shape, duplicate id, unreadable
        print(f"criteria_report: error: cannot load {CRITERIA_PATH}: {exc}", file=sys.stderr)
        return 1
    try:
        results = load_dodrun_results(args.results or find_bundles())
    except ValueError as exc:
        print(f"criteria_report: error: {exc}", file=sys.stderr)
        return 1
    rows = build_rows(criteria, load_assessments(ASSESSMENTS_PATH), load_registered(REGISTRY_PATH),
                      results)
    summary = summarize(rows, read_readme_count(README_PATH))

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
            print(f"criteria_report: error: cannot write {args.out}: {exc.strerror}",
                  file=sys.stderr)
            return 1
    else:
        # Bytes, not text: the Windows console codepage cannot encode every
        # criterion character, and the report must not depend on it.
        sys.stdout.buffer.write(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
