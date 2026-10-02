#!/usr/bin/env python3
"""
merge_dod.py - deterministically reconcile the two independent Definition of
Done specifications (Agent A, Agent B) into one authoritative criteria set.

Merge rules (from the project spec):
  1. Preserve every unique criterion; drop nothing.
  2. On dependency-class disagreement take the WORST class.
  3. On tier disagreement take the HIGHER priority.
  4. Keep the STRONGER verification: when both agents specify a criterion for
     the same slot, BOTH verification methods must pass (conjunction is
     strictly stronger than either alone).

Output: tests/conformance/criteria.json  (drives the automated harness)
"""

import json
import os
import re
import sys
from collections import OrderedDict

PROJ = "/path/to/ntpers-research"
DOCS = os.path.join(PROJ, "docs", "phase0")
OUT = os.path.join(PROJ, "tests", "conformance", "criteria.json")

# Agent A and Agent B used slightly different domain codes for the same domain.
DOMAIN_ALIAS = {
    "SOCK": "NET",
    "DOT": "DOTNET",
    "VM": "BOOTVM",
    "BOOT-VM": "BOOTVM",
    "BOOTVM": "BOOTVM",
    "PHM": "BOOTPH",
    "BOOT-PH": "BOOTPH",
    "BOOTPH": "BOOTPH",
    "LNX": "LINUXR",
    "LINUXR": "LINUXR",
}

DEP_ORDER = ["SELF", "EXT-DOC", "EXT-RE", "EXT-GATED"]  # worst = highest index
TIER_ORDER = ["P3", "P2", "P1", "P0"]                   # higher = highest index

ROW_RE = re.compile(r"^\|\s*(?:\*\*)?(DOD-[A-Z]+-\d+)")
SPLIT_RE = re.compile(r"(?<!\\)\|")


def split_row(line):
    parts = SPLIT_RE.split(line.strip())
    if parts and parts[0].strip() == "":
        parts = parts[1:]
    if parts and parts[-1].strip() == "":
        parts = parts[:-1]
    return [p.strip() for p in parts]


def find_dep(cells):
    for c in cells:
        t = c.replace("*", "").replace("`", "").strip().upper()
        if t in DEP_ORDER:
            return t
    # sometimes embedded, e.g. "EXT-GATED (MS key)"
    for c in cells:
        u = c.upper()
        for d in ("EXT-GATED", "EXT-RE", "EXT-DOC", "SELF"):
            if re.search(r"\b" + re.escape(d) + r"\b", u):
                return d
    return None


def find_tier(cells):
    for c in cells:
        m = re.search(r"\b(P[0-3])\b", c)
        if m:
            return m.group(1)
    return None


def canon_domain(cid):
    d = cid.split("-")[1]
    return DOMAIN_ALIAS.get(d, d)


TIER_CELL_RE = re.compile(r"^\**\s*(P[0-3])\s*\**$")


def parse(path, source):
    """Positional parse. Both agents emit 6-column criteria tables:
         A: | ID | Tier   | Criterion | Verification | Threshold | Dep |
         B: | ID | Domain | Criterion | Verification | Threshold | Dep |
       Distinguished by whether column 1 is a bare tier token.
       Falls back to longest-cell heuristics for any off-shape row.
    """
    out = []
    skipped = 0
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.startswith("|"):
                continue
            m = ROW_RE.match(line)
            if not m:
                continue
            cells = split_row(line)
            cid = m.group(1)
            dom = canon_domain(cid)

            if len(cells) >= 6:
                crit, ver, thr = cells[2], cells[3], cells[4]
                dep = find_dep([cells[5]]) or find_dep(cells)
                tm = TIER_CELL_RE.match(cells[1])
                tier = tm.group(1) if tm else find_tier([cells[2]])
            else:
                # Off-shape row (e.g. the EXT-GATED registry table). Keep it,
                # but derive fields defensively.
                skipped += 1
                body = cells[1:]
                cand = sorted(body, key=len, reverse=True)
                crit = cand[0] if cand else ""
                ver = cand[1] if len(cand) > 1 else ""
                thr = cand[2] if len(cand) > 2 else ""
                dep = find_dep(cells)
                tier = find_tier(cells)
                # A row in the gated registry is gated by construction.
                if dep is None and "gated" in line.lower():
                    dep = "EXT-GATED"

            out.append({
                "raw_id": cid,
                "domain": dom,
                "tier": tier,
                "dependency_class": dep,
                "criterion": crit,
                "verification": ver,
                "threshold": thr,
                "source": source,
                "offshape": len(cells) < 6,
            })
    # A criterion can appear twice in one document: once in its domain table
    # and again in the EXT-GATED registry. Collapse by raw_id, preferring the
    # full 6-column row but propagating a gated classification from either.
    by_id = OrderedDict()
    for r in out:
        prev = by_id.get(r["raw_id"])
        if prev is None:
            by_id[r["raw_id"]] = r
            continue
        keep, other = (prev, r) if not prev["offshape"] else (r, prev)
        if other["dependency_class"] == "EXT-GATED":
            keep["dependency_class"] = "EXT-GATED"
            keep["gated_note"] = other["criterion"]
        by_id[r["raw_id"]] = keep

    deduped = list(by_id.values())
    print(f"  {source}: {len(out)} rows -> {len(deduped)} unique "
          f"({skipped} off-shape, {len(out) - len(deduped)} collapsed)")
    return deduped


def worst_dep(a, b):
    vals = [x for x in (a, b) if x in DEP_ORDER]
    if not vals:
        return "EXT-DOC"
    return max(vals, key=DEP_ORDER.index)


def higher_tier(a, b):
    vals = [x for x in (a, b) if x in TIER_ORDER]
    if not vals:
        return "P1"
    return max(vals, key=TIER_ORDER.index)


def main():
    pa = os.path.join(DOCS, "dod-agent-a.md")
    pb = os.path.join(DOCS, "dod-agent-b.md")
    A = parse(pa, "A")
    B = parse(pb, "B")
    print(f"parsed: A={len(A)} rows, B={len(B)} rows")

    # Group per canonical domain, preserving document order, then zip the two
    # agents' criteria for the same domain slot-by-slot.
    def bucket(rows):
        d = OrderedDict()
        for r in rows:
            d.setdefault(r["domain"], []).append(r)
        return d

    ba, bb = bucket(A), bucket(B)
    domains = list(OrderedDict.fromkeys(list(ba.keys()) + list(bb.keys())))

    # UNION-PRESERVING MERGE.
    #
    # Positional zipping of A's Nth criterion with B's Nth criterion in a
    # domain produces false fusions (A's "PlayReady playback" is not B's
    # "Steam client login"), which would corrupt both the criterion text and
    # the EXT-GATED register. Semantic dedup is not mechanically decidable
    # here, so we preserve every criterion from both documents as a distinct,
    # independently-testable entry. Over-counting a genuine duplicate is safe;
    # fusing two distinct criteria is not. Entries are grouped by domain so a
    # later authoring pass can collapse true duplicates with human judgement.
    merged = []
    stats = {"a": 0, "b": 0}

    for dom in domains:
        for src, rows in (("A", ba.get(dom, [])), ("B", bb.get(dom, []))):
            for i, r in enumerate(rows):
                stats[src.lower()] += 1
                merged.append(OrderedDict([
                    ("id", f"DOD-{dom}-{src}{i + 1:03d}"),
                    ("domain", dom),
                    ("tier", higher_tier(r["tier"], None)),
                    ("dependency_class", worst_dep(r["dependency_class"], None)),
                    ("criterion", r["criterion"]),
                    ("verification", r["verification"]),
                    ("threshold", r["threshold"]),
                    ("source", [src]),
                    ("dup_group", dom),
                    ("origin_ids", [r["raw_id"]]),
                    ("gated_note", r.get("gated_note", "")),
                    ("status", "not_implemented"),
                ]))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(merged, fh, indent=2, ensure_ascii=False)

    # summary
    from collections import Counter
    tc = Counter(m["tier"] for m in merged)
    dc = Counter(m["dependency_class"] for m in merged)
    dom_c = Counter(m["domain"] for m in merged)

    print(f"\nWROTE {OUT}")
    print(f"TOTAL CRITERIA: {len(merged)}")
    print(f"  merge stats: {stats}")
    print(f"  tiers: {dict(sorted(tc.items()))}")
    print(f"  dep classes: {dict(sorted(dc.items()))}")
    print(f"  domains ({len(dom_c)}): {dict(sorted(dom_c.items()))}")
    print(f"  duplicate-review groups (by domain): {len(dom_c)}")
    print("\n=== EXT-GATED REGISTER ===")
    for m in merged:
        if m["dependency_class"] == "EXT-GATED":
            print(f"  {m['id']} [{m['tier']}] {m['criterion'][:150]}...")


if __name__ == "__main__":
    sys.exit(main())
