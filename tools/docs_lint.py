#!/usr/bin/env python3
"""docs_lint.py - documentation policy lint for the Definition-of-Done criteria.

Implements the check named by the criteria verification field
`dod docs-lint --policy` (DOD-SEC-A008) and the claim-boundary and
capability-matrix checks (DOD-DRV-B003, DOD-EVI-B006). Standard library only.

A violation is a sentence on the claim surface that affirms a mechanism the policy
puts out of scope, or a required statement or cross-reference that is missing.
The lint never edits the documents it reads.

Usage:
  docs_lint.py --policy --criterion DOD-SEC-A008
  docs_lint.py --policy --criterion DOD-DRV-B003
  docs_lint.py --policy --criterion DOD-EVI-B006
  docs_lint.py --policy --all
  docs_lint.py --policy --criterion DOD-SEC-A008 --root PATH   (synthetic trees)

Exit status: 0 zero violations, 1 one or more violations, 2 usage or unreadable input.
"""
import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

POLICY_DOC = Path("docs/policy/INTEGRITY-POLICY.md")
BOUNDARY_DOC = Path("docs/policy/CLAIM-BOUNDARY.md")
CRITERIA_DOC = Path("docs/status/CRITERIA.md")
CRITERIA_JSON = Path("tests/conformance/criteria.json")
RESTATEMENT_DOCS = {"CRITERIA.md", "CRITERIA-ROADMAP.md", "CRITERIA-PROGRESS.md"}

DOD_ID_RE = re.compile(r"\bDOD-[A-Z]+-[AB]\d{3}\b")
SENTENCE_SPLIT = re.compile(r"(?<=[.!?;])\s+")

# A sentence is a claim when it has a topic term and a claim verb, and no
# qualifier. Qualifiers are the words that make a sentence a disclaimer.
CLAIM_VERB = re.compile(
    r"\b(supports?|supported|enables?|enabled|implements?|implemented|provides?|"
    r"provided|compatible|passes|passing|active|running|satisfies|guarantees?|"
    r"certified|achieves?|runs?|loads?|executes?|works?)\b",
    re.IGNORECASE,
)
QUALIFIER = re.compile(
    r"\b(not|never|no|none|nothing|without|out[- ]of[- ]scope|declared|cannot|"
    r"can't|does not|do not|historical|not reproduced|unknown|gated|excluded|"
    r"absent|missing|instead of|rather than|waiver|waived|refused|refuses)\b",
    re.IGNORECASE,
)

SEC_TOPIC = re.compile(
    r"patchguard|\bkpp\b|secure kernel|\bhvci\b|virtualization[- ]based security|"
    r"\bvbs\b|windows[- ]update[- ]signed|microsoft[- ]signed",
    re.IGNORECASE,
)
DRV_TOPIC = re.compile(
    r"(third[- ]party|vendor|production|whql)[^.;]{0,60}(\.sys|whql|signed)|"
    r"\bwhql\b",
    re.IGNORECASE,
)
POLICY_HEADINGS = ["## Scope", "## Claims", "## Out-of-scope mechanisms",
                   "## Rules for public text", "## Cross-references"]


class LintError(Exception):
    """Unreadable or missing input: exit status 2, not a policy violation."""


def read(root, rel):
    path = root / rel
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise LintError(f"cannot read {rel}: {exc}")


def claim_surface(root):
    """Root Markdown files plus docs/status and docs/policy Markdown, sorted.

    The criteria restatements quote requirements such as "load a third-party
    driver", so they are not claims and are left out. The capability-matrix
    rule reads them directly.
    """
    files = sorted(root.glob("*.md"))
    files += sorted((root / "docs" / "status").glob("*.md"))
    files += sorted((root / "docs" / "policy").glob("*.md"))
    return [p for p in files if p.is_file() and p.name not in RESTATEMENT_DOCS]


UNIT_START = ("|", "#", "- ", "* ")
LIST_ITEM = re.compile(r"\d+\. ")


def sentences(text):
    """Yield (line_number, sentence) for every sentence in the text.

    Wrapped prose lines are joined into one paragraph before splitting, so a
    sentence broken across lines is still one sentence. Table rows, headings and
    list items are single units.
    """
    buf = []
    start = 1

    def flush():
        if not buf:
            return
        for piece in SENTENCE_SPLIT.split(" ".join(buf)):
            piece = piece.strip()
            if piece:
                yield start, piece
        buf.clear()

    for lineno, line in enumerate(text.splitlines(), start=1):
        s = line.strip()
        if not s:
            yield from flush()
            continue
        if s.startswith(UNIT_START) or LIST_ITEM.match(s):
            yield from flush()
            for piece in SENTENCE_SPLIT.split(s):
                piece = piece.strip()
                if piece:
                    yield lineno, piece
            continue
        if not buf:
            start = lineno
        buf.append(s)
    yield from flush()


def is_claim(sentence, topic):
    return bool(topic.search(sentence) and CLAIM_VERB.search(sentence)
                and not QUALIFIER.search(sentence))


def load_criteria_ids(root):
    try:
        data = json.loads(read(root, CRITERIA_JSON))
    except (ValueError, LintError) as exc:
        raise LintError(f"cannot load criteria ids: {exc}")
    return {c["id"] for c in data}


def check_policy_doc(root, violations):
    try:
        text = read(root, POLICY_DOC)
    except LintError:
        violations.append(f"{POLICY_DOC}: policy document is missing")
        return
    for heading in POLICY_HEADINGS:
        if heading not in text:
            violations.append(f"{POLICY_DOC}: missing section '{heading}'")
    if not re.search(r"patchguard.*out[- ]of[- ]scope|out[- ]of[- ]scope.*patchguard",
                     text, re.IGNORECASE):
        violations.append(f"{POLICY_DOC}: PatchGuard is not declared out of scope")
    if not re.search(r"secure kernel.*out[- ]of[- ]scope|out[- ]of[- ]scope.*secure kernel",
                     text, re.IGNORECASE):
        violations.append(f"{POLICY_DOC}: Secure Kernel is not declared out of scope")
    known = load_criteria_ids(root)
    for cid in sorted(set(DOD_ID_RE.findall(text))):
        if cid not in known:
            violations.append(f"{POLICY_DOC}: cross-reference {cid} is not in criteria.json")


def check_claims(root, topic, violations):
    for path in claim_surface(root):
        rel = path.relative_to(root).as_posix()
        for lineno, sentence in sentences(path.read_text(encoding="utf-8")):
            if is_claim(sentence, topic):
                violations.append(f"{rel}:{lineno}: affirms an out-of-scope mechanism: {sentence[:120]}")


def lint_sec_a008(root):
    violations = []
    check_policy_doc(root, violations)
    check_claims(root, SEC_TOPIC, violations)
    return violations


def lint_drv_b003(root):
    violations = []
    try:
        boundary = read(root, BOUNDARY_DOC)
    except LintError:
        return [f"{BOUNDARY_DOC}: claim-boundary document is missing"]
    if not re.search(r"no third-party NT .?\.sys.? executes", boundary, re.IGNORECASE):
        violations.append(f"{BOUNDARY_DOC}: required statement 'No third-party NT .sys executes' is missing")
    for path in claim_surface(root):
        rel = path.relative_to(root).as_posix()
        for lineno, sentence in sentences(path.read_text(encoding="utf-8")):
            low = sentence.lower()
            if "hardware support" in low and not ("api subset" in low and "device class" in low):
                violations.append(f"{rel}:{lineno}: 'hardware support' claim does not name an API subset and device class: {sentence[:120]}")
            if is_claim(sentence, DRV_TOPIC):
                violations.append(f"{rel}:{lineno}: affirms a third-party or gated driver claim: {sentence[:120]}")
    return violations


CELL_SPLIT = re.compile(r"(?<!\\)\|")


def table_cells(line):
    """Split a Markdown table row on unescaped pipes; the outer pipes give empty ends."""
    parts = [c.strip() for c in CELL_SPLIT.split(line.strip())]
    return parts[1:-1]


def lint_evi_b006(root):
    violations = []
    try:
        table = read(root, CRITERIA_DOC).splitlines()
    except LintError:
        return [f"{CRITERIA_DOC}: capability matrix is missing"]
    rows = [ln for ln in table if ln.startswith("| DOD-")]
    if not rows:
        violations.append(f"{CRITERIA_DOC}: capability matrix has no criterion rows")
    for ln in rows:
        cells = table_cells(ln)
        if len(cells) < 8:
            violations.append(f"{CRITERIA_DOC}: row lacks ID, tier, verdict, evidence or owner: {ln[:100]}")
            continue
        cid, _area, _desc, tier, dep, status, evidence, owner = cells[:8]
        if not tier:
            violations.append(f"{CRITERIA_DOC}: {cid} has no tier")
        if status.lower() == "pass" and evidence in ("", "none recorded"):
            violations.append(f"{CRITERIA_DOC}: {cid} claims pass without an evidence link")
        if dep == "EXT-GATED" and not owner:
            violations.append(f"{CRITERIA_DOC}: gated {cid} has no owner name")
    # A pass-count claim is unlinked unless its paragraph also says it is historical
    # or not reproduced. The qualifier may be in the next sentence of the paragraph.
    pass_count = re.compile(r"criteria passing", re.IGNORECASE)
    for path in claim_surface(root):
        rel = path.relative_to(root).as_posix()
        for block in re.split(r"\n\s*\n", path.read_text(encoding="utf-8")):
            flat = " ".join(block.split())
            if pass_count.search(flat) and not QUALIFIER.search(flat):
                violations.append(f"{rel}: unqualified pass-count claim without a linked bundle: {flat[:120]}")
    return violations


RULES = {
    "DOD-SEC-A008": lint_sec_a008,
    "DOD-DRV-B003": lint_drv_b003,
    "DOD-EVI-B006": lint_evi_b006,
}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--policy", action="store_true", help="run the policy checks (required)")
    p.add_argument("--criterion", help="criterion ID whose rule set to run")
    p.add_argument("--all", action="store_true", help="run every rule set")
    p.add_argument("--root", type=Path, default=REPO, help="tree to lint (default: this repository)")
    args = p.parse_args(argv)
    if not args.policy:
        p.error("--policy is required")
    if bool(args.criterion) == bool(args.all):
        p.error("give exactly one of --criterion or --all")
    selected = sorted(RULES) if args.all else [args.criterion]
    if args.criterion and args.criterion not in RULES:
        print(f"docs_lint: no policy rule set for {args.criterion}", file=sys.stderr)
        return 2
    total = 0
    try:
        for cid in selected:
            found = RULES[cid](args.root)
            total += len(found)
            for v in found:
                print(f"VIOLATION [{cid}] {v}")
            print(f"{cid}: {len(found)} violation(s)")
    except LintError as exc:
        print(f"docs_lint: {exc}", file=sys.stderr)
        return 2
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
