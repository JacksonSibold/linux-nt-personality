#!/usr/bin/env python3
"""Validate criteria.json and print the authoritative DoD summary."""
import json
import os
from collections import Counter
from pathlib import Path

# Repository root: NTPERS_ROOT if set, otherwise the parent of build/.
ROOT = Path(os.environ.get("NTPERS_ROOT")
            or Path(__file__).resolve().parents[1])
P = ROOT / "tests" / "conformance" / "criteria.json"

d = json.load(open(P, encoding="utf-8"))
print("valid JSON. entries:", len(d))

req = {"id", "domain", "tier", "dependency_class", "criterion",
       "verification", "threshold", "source", "status"}
bad = [x["id"] for x in d if not req.issubset(x)]
print("entries missing required fields:", len(bad))

ids = [x["id"] for x in d]
print("duplicate ids:", len(ids) - len(set(ids)))

empty_crit = [x["id"] for x in d if not x["criterion"].strip()]
print("empty criterion text:", len(empty_crit), empty_crit[:5])

print("\ntiers:", dict(sorted(Counter(x["tier"] for x in d).items())))
print("dep:  ", dict(sorted(Counter(x["dependency_class"] for x in d).items())))
print("source:", dict(sorted(Counter(x["source"][0] for x in d).items())))

print("\n=== EXT-GATED (%d) ===" % sum(
    1 for x in d if x["dependency_class"] == "EXT-GATED"))
for x in d:
    if x["dependency_class"] == "EXT-GATED":
        print(" %-20s [%s] %s" % (x["id"], x["tier"], x["criterion"][:110]))

print("\n=== P0 BY DOMAIN ===")
print(dict(sorted(Counter(
    x["domain"] for x in d if x["tier"] == "P0").items())))
