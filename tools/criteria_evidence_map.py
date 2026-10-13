#!/usr/bin/env python3
"""Map Definition-of-Done criteria to candidate repository evidence.

Reads tests/conformance/criteria.json, the roadmap milestone table
(docs/status/CRITERIA-ROADMAP.md), the registry (tests/harness/registry.yaml)
and the reviewed assessments (tools/criteria_evidence_assessments.yaml).

Candidate discovery (no test is run):
  tests/host/*.c, tests/kernel-host/*.c, tests/ntdll-host/*.c   C test sources
  tests/unit/*.py                                               unittest modules
  tools/*.py that expose a --check, --policy or --criterion mode  check scripts
  docs/policy/*.md and docs/status/CRITERIA.md                  machine-checkable docs

A candidate scores when it names the criterion ID (strong) or shares keywords
with the candidate's test or function names and leading text (weak).

Assessments are written by a reviewer, not computed. Each verdict is one of:
  FULL     the cited tests, as written, demonstrate everything the criterion text requires
  PARTIAL  they demonstrate some of it; the entry must say what is missing
  NONE     no candidate demonstrates the criterion

The tool enforces the rules that can be checked mechanically: a FULL or PARTIAL
verdict must cite at least one candidate that was discovered in this repository,
and a FULL verdict must not be on a criterion whose roadmap needs include GOLDEN-DIFF,
NT-KERNEL, HW-BOOT, VM-HARNESS or ANTI-CHEAT-DRM. It never writes results and never
marks a criterion PASS; the harness result column is recorded from dodrun output.

Usage:
  criteria_evidence_map.py                 candidate list for every criterion, as text
  criteria_evidence_map.py --markdown      assessed criteria as a Markdown table
  criteria_evidence_map.py --selftest      built-in checks on synthetic data and repository references

Standard library plus PyYAML.
"""
import argparse
import json
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - reported by main()
    yaml = None

ROOT = Path(__file__).resolve().parents[1]
CRITERIA = ROOT / "tests/conformance/criteria.json"
ROADMAP = ROOT / "docs/status/CRITERIA-ROADMAP.md"
REGISTRY = ROOT / "tests/harness/registry.yaml"
ASSESSMENTS = ROOT / "tools/criteria_evidence_assessments.yaml"

ID_RE = re.compile(r"\bDOD-[A-Z0-9]+-[A-Z]\d{3}\b")
ROADMAP_ROW_RE = re.compile(
    r"^\|\s*(\d+)\s*\|\s*`(DOD-[A-Z0-9-]+)`\s*\|\s*(P\d)\s*\|\s*(\w+)\s*\|\s*(M-[A-Z])\s*\|\s*([^|]+?)\s*\|")
C_FUNC_RE = re.compile(r"^(?:static\s+)?(?:void|int|NTSTATUS|ULONG|DWORD|BOOL|size_t)\s+(\w+)\s*\(", re.M)
PY_TEST_RE = re.compile(r"^\s*def\s+(test_\w+)\s*\(", re.M)
PY_FUNC_RE = re.compile(r"^def\s+(\w+)\s*\(", re.M)
STOP = {
    "with", "that", "from", "this", "each", "every", "must", "none", "only", "under",
    "into", "they", "their", "than", "then", "when", "where", "which", "more", "less",
    "same", "over", "after", "before", "against", "across", "using", "used", "uses",
    "should", "would", "could", "will", "shall", "have", "been", "being", "were", "does",
    "the", "and", "for", "are", "not", "all", "any", "one", "two", "per", "run", "runs",
    "test", "tests", "pass", "passes", "criterion", "verification", "threshold", "dod",
    "exact", "exactly", "zero", "100%", "from", "such", "also", "about", "like",
}
# Roadmap needs that make a criterion unsuitable for a host-only FULL verdict.
NON_HOST_NEEDS = {"GOLDEN-DIFF", "NT-KERNEL", "HW-BOOT", "VM-HARNESS", "ANTI-CHEAT-DRM"}
VERDICTS = {"FULL", "PARTIAL", "NONE"}


def keywords(text):
    """Lowercased content words of length >= 4, stopwords removed."""
    words = re.findall(r"[a-z][a-z0-9_]{3,}", text.lower())
    return {w for w in words if w not in STOP}


def parse_roadmap(text):
    """Return {criterion id: {milestone, needs, tier, position}} from the roadmap table."""
    out = {}
    for line in text.splitlines():
        m = ROADMAP_ROW_RE.match(line)
        if not m:
            continue
        pos, cid, tier, _area, milestone, needs = m.groups()
        out[cid] = {
            "position": int(pos),
            "tier": tier,
            "milestone": milestone,
            "needs": [n.strip() for n in needs.split(",") if n.strip()],
        }
    return out


def discover(root):
    """Return a list of candidate dicts: path, kind, names, text."""
    found = []

    def add(path, kind, names, text):
        found.append({"path": path.relative_to(root).as_posix(), "kind": kind,
                      "names": sorted(set(names)), "text": text})

    for sub in ("tests/host", "tests/kernel-host", "tests/ntdll-host"):
        for p in sorted((root / sub).glob("*.c")):
            text = p.read_text(encoding="utf-8", errors="replace")
            add(p, "host-c", C_FUNC_RE.findall(text), text)
    for p in sorted((root / "tests/unit").glob("test_*.py")):
        text = p.read_text(encoding="utf-8", errors="replace")
        add(p, "unit-py", PY_TEST_RE.findall(text), text)
    for p in sorted((root / "tools").glob("*.py")):
        text = p.read_text(encoding="utf-8", errors="replace")
        if p.name != Path(__file__).name and re.search(r"--check|--policy|--criterion", text):
            add(p, "tool-check", PY_FUNC_RE.findall(text), text)
    for p in sorted((root / "docs/policy").glob("*.md")):
        text = p.read_text(encoding="utf-8", errors="replace")
        add(p, "doc", re.findall(r"^#+\s+(.+)$", text, re.M), text)
    # The capability matrix lists every criterion ID, so an ID mention there is not evidence.
    matrix = root / "docs/status/CRITERIA.md"
    if matrix.is_file():
        text = matrix.read_text(encoding="utf-8", errors="replace")
        add(matrix, "doc-matrix", [], text)
    return found


def candidates_for(cid, criterion_text, pool, top=3):
    """Score every pooled candidate for one criterion and return the best few."""
    kw = keywords(criterion_text)
    scored = []
    for cand in pool:
        mentions = cand["kind"] != "doc-matrix" and re.search(r"\b" + re.escape(cid) + r"\b", cand["text"])
        strong = 10 if mentions else 0
        names_text = " ".join(cand["names"])
        head = "\n".join(cand["text"].splitlines()[:40])
        cand_kw = keywords(names_text + " " + head)
        shared = sorted(kw & cand_kw)
        score = strong + len(shared)
        if score > 0:
            scored.append({"path": cand["path"], "kind": cand["kind"], "score": score,
                           "id_mention": bool(strong), "shared": shared[:8]})
    scored.sort(key=lambda c: (-c["score"], c["path"]))
    return scored[:top]


def load_yaml(path):
    if yaml is None:
        raise RuntimeError("PyYAML is required (python -m pip install pyyaml)")
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def validate(assessments, known_paths, roadmap):
    """Return a list of rule violations in the assessments."""
    errors = []
    for cid, a in assessments.items():
        verdict = a.get("verdict")
        if verdict not in VERDICTS:
            errors.append(f"{cid}: verdict {verdict!r} is not one of {sorted(VERDICTS)}")
            continue
        cites = a.get("candidates") or []
        if verdict in ("FULL", "PARTIAL") and not cites:
            errors.append(f"{cid}: {verdict} needs at least one cited candidate")
        for path in cites:
            if path not in known_paths:
                errors.append(f"{cid}: cited candidate {path} was not discovered in this repository")
        if verdict == "PARTIAL" and not a.get("missing"):
            errors.append(f"{cid}: PARTIAL must say what is missing")
        if verdict == "FULL":
            needs = set(roadmap.get(cid, {}).get("needs", []))
            if needs & NON_HOST_NEEDS:
                errors.append(f"{cid}: FULL not allowed, roadmap needs {sorted(needs & NON_HOST_NEEDS)}")
    return errors


def render_markdown(assessments, criteria, roadmap):
    lines = ["| ID | Tier | Verdict | Candidates | Missing / reasoning | Harness result |",
             "|---|---|---|---|---|---|"]
    by_id = {c["id"]: c for c in criteria}
    for cid in sorted(assessments):
        a = assessments[cid]
        tier = by_id.get(cid, {}).get("tier", "?")
        cites = ", ".join(f"`{p}`" for p in a.get("candidates") or []) or "none"
        note = a.get("reasoning", "").replace("|", "/").replace("\n", " ")
        missing = a.get("missing")
        if missing:
            note = f"Missing: {missing.replace('|', '/')}. {note}"
        lines.append(f"| `{cid}` | {tier} | {a['verdict']} | {cites} | {note} | {a.get('harness_result', '')} |")
    return "\n".join(lines)


def print_candidates(criteria, pool, roadmap):
    for c in criteria:
        cid = c["id"]
        hits = candidates_for(cid, c.get("criterion", "") + " " + c.get("threshold", ""), pool)
        if not hits:
            continue
        needs = roadmap.get(cid, {}).get("needs", ["-"])
        print(f"{cid} [{c.get('dependency_class')}] needs={','.join(needs)}")
        for h in hits:
            print(f"    {h['score']:>3}  {h['path']}  ({h['kind']}) shared={','.join(h['shared']) or '-'}"
                  + ("  id-mention" if h["id_mention"] else ""))


def selftest():
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    check("keywords drop stopwords and short words",
          keywords("the test passes with 100 cases") == {"cases"})
    row = "| 28 | `DOD-SYNC-A001` | P0 | SYNC | M-C | WIN32-SHIM, NT-KERNEL | `WI-SYNC-KERNEL` | XL | why |"
    parsed = parse_roadmap(row)
    check("roadmap row parses id, milestone, needs",
          parsed.get("DOD-SYNC-A001", {}).get("needs") == ["WIN32-SHIM", "NT-KERNEL"]
          and parsed["DOD-SYNC-A001"]["milestone"] == "M-C")
    synthetic = [
        {"path": "tests/host/x.c", "kind": "host-c", "names": ["protect_maps_each_base_value"],
         "text": "/* DOD-SYNC-A001 */\nstatic void protect_maps_each_base_value(void) {}"},
        {"path": "tests/host/y.c", "kind": "host-c", "names": ["unrelated"], "text": "int x;"},
    ]
    hits = candidates_for("DOD-SYNC-A001", "protect maps each base value", synthetic)
    check("id mention outranks keyword-only match",
          hits and hits[0]["path"] == "tests/host/x.c" and hits[0]["id_mention"])
    check("unrelated candidate is not returned",
          all(h["path"] != "tests/host/y.c" for h in hits))
    known = {"tests/host/x.c"}
    rules = {"DOD-A-B001": {"needs": ["WIN32-SHIM"]}, "DOD-B-A001": {"needs": ["GOLDEN-DIFF"]}}
    check("bad verdict rejected",
          validate({"DOD-A-B001": {"verdict": "PASS"}}, known, rules))
    check("FULL with no citation rejected",
          validate({"DOD-A-B001": {"verdict": "FULL", "candidates": []}}, known, rules))
    check("citation to undiscovered path rejected",
          validate({"DOD-A-B001": {"verdict": "FULL", "candidates": ["tests/host/zz.c"]}}, known, rules))
    check("PARTIAL without missing text rejected",
          validate({"DOD-A-B001": {"verdict": "PARTIAL", "candidates": ["tests/host/x.c"]}}, known, rules))
    check("FULL on golden-needing criterion rejected",
          validate({"DOD-B-A001": {"verdict": "FULL", "candidates": ["tests/host/x.c"]}}, known, rules))
    check("valid FULL on host criterion accepted",
          not validate({"DOD-A-B001": {"verdict": "FULL", "candidates": ["tests/host/x.c"]}}, known, rules))

    # Repository references: every discovered candidate must be readable, and the
    # shipped assessments must satisfy every rule.
    pool = discover(ROOT)
    check("repository yields host, unit, tool and doc candidates",
          {c["kind"] for c in pool} >= {"host-c", "unit-py", "tool-check", "doc"})
    roadmap = parse_roadmap(ROADMAP.read_text(encoding="utf-8"))
    check("roadmap lists the 103 M-C criteria",
          sum(1 for v in roadmap.values() if v["milestone"] == "M-C") == 103)
    if ASSESSMENTS.exists() and yaml is not None:
        assessments = load_yaml(ASSESSMENTS)
        errs = validate(assessments, {c["path"] for c in pool}, roadmap)
        check("shipped assessments satisfy the rules: " + "; ".join(errs), not errs)
    for f in failures:
        print("SELFTEST FAIL:", f, file=sys.stderr)
    if not failures:
        print("selftest: all checks passed")
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--markdown", action="store_true", help="print the assessed criteria as a Markdown table")
    p.add_argument("--selftest", action="store_true", help="run built-in checks")
    args = p.parse_args(argv)
    if args.selftest:
        return selftest()
    if yaml is None:
        print("PyYAML is required", file=sys.stderr)
        return 2
    criteria = json.loads(CRITERIA.read_text(encoding="utf-8"))
    roadmap = parse_roadmap(ROADMAP.read_text(encoding="utf-8"))
    pool = discover(ROOT)
    if args.markdown:
        assessments = load_yaml(ASSESSMENTS)
        errs = validate(assessments, {c["path"] for c in pool}, roadmap)
        if errs:
            print("\n".join(errs), file=sys.stderr)
            return 1
        print(render_markdown(assessments, criteria, roadmap))
        return 0
    print_candidates(criteria, pool, roadmap)
    return 0


if __name__ == "__main__":
    sys.exit(main())
