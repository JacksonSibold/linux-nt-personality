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
# Negation for the driver, evidence and pass rules. Narrower than QUALIFIER: a
# disclaimer must say the thing is absent, not merely that a waiver exists.
NEGATION = re.compile(
    r"\b(not|never|no|none|nothing|cannot|can't|does not|do not|out[- ]of[- ]scope|"
    r"excluded|absent|missing|refused|refuses)\b",
    re.IGNORECASE,
)
BOUNDARY_STATEMENT = re.compile(r"No third-party NT `?\.sys`? executes", re.IGNORECASE)
WAIVER_REF = re.compile(r"§\s*11|section 11", re.IGNORECASE)
STATEMENT_SITES = [BOUNDARY_DOC, Path("README.md"), CRITERIA_DOC]
# A hyphen before the word makes it part of an identifier (SECOND-PASS), not a result.
PASS_CLAIM = re.compile(r"(?<![\w-])(pass|passes|passing|passed|certified|green)(?!\w)", re.IGNORECASE)
PASS_COUNT = re.compile(r"criteria\s+passing", re.IGNORECASE)
TABLE_ID = re.compile(r"^\|\s*`?(DOD-[A-Z]+-[AB]\d{3})`?\s*\|")
MARKDOWN_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
BUNDLE_PATH = re.compile(r"[\w./-]*(?:bundle|\.evidence\.json)[\w./-]*", re.IGNORECASE)
MATRIX_COLUMNS = ("ID", "Tier", "Verdict", "Evidence", "Owner")

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
    """Root Markdown files plus docs/status and docs/policy Markdown, sorted."""
    files = sorted(root.glob("*.md"))
    files += sorted((root / "docs" / "status").glob("*.md"))
    files += sorted((root / "docs" / "policy").glob("*.md"))
    return [p for p in files if p.is_file()]


def sentences(text):
    """Yield (line_number, sentence) for every sentence in the text."""
    for lineno, line in enumerate(text.splitlines(), start=1):
        for piece in SENTENCE_SPLIT.split(line.strip()):
            piece = piece.strip()
            if piece:
                yield lineno, piece


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
    flat = " ".join(text.split())  # a declaration may wrap across lines
    if not re.search(r"windows[- ]update[- ]signed[^.]*out[- ]of[- ]scope|out[- ]of[- ]scope[^.]*windows[- ]update[- ]signed",
                     flat, re.IGNORECASE):
        violations.append(f"{POLICY_DOC}: Windows-update-signed kernel is not declared out of scope")
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


def normalize(text):
    """Lower case, no markdown emphasis or code marks, single spaces."""
    return " ".join(text.replace("*", "").replace("`", "").split()).lower()


def criterion_texts(root):
    """Map each criterion ID to its normalized criterion text from criteria.json."""
    try:
        data = json.loads(read(root, CRITERIA_JSON))
    except ValueError as exc:
        raise LintError(f"cannot load criteria text: {exc}")
    return {c["id"]: normalize(c.get("criterion", "")) for c in data}


def is_restatement(line, texts):
    """True when a table row restates its criterion's own text. That is the
    requirement, not a claim that the requirement is met."""
    m = TABLE_ID.match(line)
    if not m or not texts.get(m.group(1)):
        return False
    return texts[m.group(1)] in normalize(line)


def claim_sentences(root, texts):
    """Yield (rel, lineno, sentence) for the claim surface, minus criterion restatements."""
    for path in claim_surface(root):
        rel = path.relative_to(root).as_posix()
        lines = path.read_text(encoding="utf-8").splitlines()
        for lineno, line in enumerate(lines, start=1):
            if is_restatement(line, texts):
                continue
            for piece in SENTENCE_SPLIT.split(line.strip()):
                piece = piece.strip()
                if piece:
                    yield rel, lineno, piece


def lint_drv_b003(root):
    """DOD-DRV-B003: the no-third-party-.sys statement is stated in the claim
    boundary, the public README and the capability matrix; a 'hardware support'
    claim names an API subset and a device class; an affirmed third-party or
    production-signed driver claim needs a section 11 waiver."""
    violations = []
    texts = criterion_texts(root)
    for site in STATEMENT_SITES:
        try:
            lines = read(root, site).splitlines()
        except LintError:
            violations.append(f"{site}: document is missing (required to state the claim boundary)")
            continue
        # A criterion row that quotes the requirement does not state the boundary.
        if not any(BOUNDARY_STATEMENT.search(ln) and not is_restatement(ln, texts) for ln in lines):
            violations.append(f"{site}: required statement 'No third-party NT .sys executes' is missing")
    for rel, lineno, sentence in claim_sentences(root, texts):
        low = sentence.lower()
        negated = bool(NEGATION.search(sentence))
        if "hardware support" in low and not negated and not ("api subset" in low and "device class" in low):
            violations.append(f"{rel}:{lineno}: 'hardware support' claim does not name an API subset and device class: {sentence[:120]}")
        if DRV_TOPIC.search(sentence) and CLAIM_VERB.search(sentence) and not negated:
            if not (WAIVER_REF.search(sentence) and "waiver" in low):
                violations.append(f"{rel}:{lineno}: affirms a third-party or gated driver claim without a section 11 waiver: {sentence[:120]}")
    return violations


def split_row(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def bundle_linked(root, sentence):
    """True when the sentence links a bundle file that exists in the tree.
    The lint checks that the file exists; it does not verify the signature."""
    for token in BUNDLE_PATH.findall(sentence):
        if (root / token.strip("`.,;:()")).is_file():
            return True
    return False


def lint_evi_b006(root):
    """DOD-EVI-B006: the capability matrix has ID, tier, verdict and evidence
    columns; every row shows a local evidence link that resolves; gated rows
    name an owner. A sentence that claims a criterion passes, or a pass count,
    needs a linked bundle that exists."""
    violations = []
    try:
        lines = read(root, CRITERIA_DOC).splitlines()
    except LintError:
        return [f"{CRITERIA_DOC}: capability matrix is missing"]
    header_line = next((ln for ln in lines if ln.startswith("| ID |")), None)
    if header_line is None:
        violations.append(f"{CRITERIA_DOC}: capability matrix has no header row beginning '| ID |'")
        header = []
    else:
        header = split_row(header_line)
    for col in MATRIX_COLUMNS:
        if col not in header:
            violations.append(f"{CRITERIA_DOC}: capability matrix lacks required column '{col}'")
    rows = [ln for ln in lines if TABLE_ID.match(ln)]
    if not rows:
        violations.append(f"{CRITERIA_DOC}: capability matrix has no criterion rows")
    for ln in rows:
        cells = split_row(ln)
        if not header or len(cells) != len(header):
            violations.append(f"{CRITERIA_DOC}: row does not match the matrix header: {ln[:100]}")
            continue
        row = dict(zip(header, cells))
        cid = row.get("ID", "?")
        if "Tier" in row and not row["Tier"]:
            violations.append(f"{CRITERIA_DOC}: {cid} has no tier")
        if "Verdict" in row and not row["Verdict"]:
            violations.append(f"{CRITERIA_DOC}: {cid} has no verdict")
        if "Evidence" in row:
            links = MARKDOWN_LINK.findall(row["Evidence"])
            if not links:
                violations.append(f"{CRITERIA_DOC}: {cid} shows no evidence link")
            for target in links:
                local = target.split("#")[0]
                if not local or "://" in local or not (root / CRITERIA_DOC.parent / local).exists():
                    violations.append(f"{CRITERIA_DOC}: {cid} evidence link does not resolve to a local file: {target}")
        gated = row.get("Dependency") == "EXT-GATED" or "deviation" in row.get("Verdict", "").lower()
        if gated and "Owner" in row and not row["Owner"]:
            violations.append(f"{CRITERIA_DOC}: gated or deviation row {cid} has no owner name")
    texts = criterion_texts(root)
    for rel, lineno, sentence in claim_sentences(root, texts):
        if not PASS_CLAIM.search(sentence):
            continue
        if not (DOD_ID_RE.search(sentence) or PASS_COUNT.search(sentence)):
            continue
        if NEGATION.search(sentence) or bundle_linked(root, sentence):
            continue
        violations.append(f"{rel}:{lineno}: pass claim without a linked green bundle: {sentence[:120]}")
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
