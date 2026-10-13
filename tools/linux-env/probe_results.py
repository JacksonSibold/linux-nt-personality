#!/usr/bin/env python3
"""
probe_results.py - turn the serial log of a probes boot into per-probe results and diff them with the
host-captured Windows goldens, using the repo's own comparator (tests/differential/dualrun.py).

Subcommands:
  split   SERIAL_LOG OUT_DIR
          Walks the serial log in order. Each probe is framed by the kmsg markers written by
          tools/linux-env/probes-initramfs.sh (NTPERS_PROBE_BEGIN / _RC / _END). Probe stdout reaches the
          console as "nt console[-11]: <chunk>" (kernel printk, see tools/linux-env/ntpers_vm_stdout.c),
          so the chunks are de-prefixed and joined. The first JSON object in the frame is the probe's
          output. Writes OUT_DIR/<probe>.raw (de-prefixed text), OUT_DIR/<probe>.json (when a JSON object
          parses) and OUT_DIR/status.json.

  compare OUT_DIR GOLDEN_DIR
          For each probe with a JSON result and a golden (<probe>.json or <probe>.json.gz), calls
          dualrun.normalize_json / diff_jsons / evaluate_probe and writes OUT_DIR/compare.json.

Exit status is 0 on success; the verdicts are in compare.json, not in the exit status.
"""
import gzip
import importlib.util
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

MARKER_RE = re.compile(r"NTPERS_PROBE_(BEGIN|RC|END) (\S+)(?: (-?\d+))?")
ALL_DONE_RE = re.compile(r"NTPERS_ALL_DONE")
# printk line with a timestamp, console payload prefix from ntio_console_write (nt_io.c pr_info)
CONSOLE_RE = re.compile(r"^(?:\[\s*\d+\.\d+\]\s*)?nt console\[-?\d+\]: ?(.*)$")
TS_RE = re.compile(r"^\[\s*\d+\.\d+\]")
DROP_RE = re.compile(r"messages? dropped")


def split(serial_log: Path, out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    text = serial_log.read_text(encoding="utf-8", errors="replace")
    probes = {}
    order = []
    cur = None
    in_payload = False
    dropped = 0
    all_done = False
    for line in text.splitlines():
        line = line.rstrip("\r")
        if DROP_RE.search(line):
            dropped += 1
        m = MARKER_RE.search(line)
        if m:
            kind, name, val = m.group(1), m.group(2), m.group(3)
            if kind == "BEGIN":
                cur = name
                in_payload = False
                if name not in probes:
                    probes[name] = {"ran": True, "rc": None, "end": False, "chunks": []}
                    order.append(name)
            elif kind == "RC" and name in probes:
                probes[name]["rc"] = int(val)
            elif kind == "END" and name in probes:
                probes[name]["end"] = True
                cur = None
                in_payload = False
            continue
        if ALL_DONE_RE.search(line):
            all_done = True
            continue
        if cur is None:
            continue
        cm = CONSOLE_RE.match(line)
        if cm:
            probes[cur]["chunks"].append(cm.group(1))
        elif not TS_RE.match(line):
            # Continuation line of a console chunk (printk prints it without the prefix), or one of
            # the blank lines printk adds after each chunk. Kept, so that no payload is lost.
            probes[cur]["chunks"].append(line)
        # Timestamped kernel lines inside the frame are not probe output and are skipped.

    dec = json.JSONDecoder()
    status = {"probes": {}, "printk_drop_lines": dropped, "all_done_seen": all_done}
    for name in order:
        p = probes[name]
        # Join with "": printk's line breaks are not part of the probe's bytes. A record cut inside a
        # JSON string (the shim splits where no newline is near) must rejoin without a newline.
        raw = "".join(p["chunks"])
        (out_dir / f"{name}.raw").write_text(raw, encoding="utf-8")
        parsed = None
        idx = raw.find("{")
        if idx >= 0:
            try:
                parsed, _ = dec.raw_decode(raw[idx:])
            except ValueError:
                parsed = None
        if parsed is not None:
            (out_dir / f"{name}.json").write_text(json.dumps(parsed, indent=1), encoding="utf-8")
        status["probes"][name] = {
            "ran": p["ran"],
            "rc": p["rc"],
            "end_marker": p["end"],
            "payload_bytes": len(raw),
            "json_ok": parsed is not None,
            "json_probe_field": parsed.get("probe") if isinstance(parsed, dict) else None,
        }
    (out_dir / "status.json").write_text(json.dumps(status, indent=1), encoding="utf-8")
    return status


def _load_dualrun():
    spec = importlib.util.spec_from_file_location("dualrun", ROOT / "tests" / "differential" / "dualrun.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _leaves(obj) -> int:
    if isinstance(obj, dict):
        return sum(_leaves(v) for v in obj.values())
    if isinstance(obj, list):
        return sum(_leaves(v) for v in obj)
    return 1


def _golden(golden_dir: Path, probe: str):
    gz = golden_dir / f"{probe}.json.gz"
    if gz.exists():
        return json.loads(gzip.open(gz, "rt", encoding="utf-8").read())
    plain = golden_dir / f"{probe}.json"
    if plain.exists():
        return json.loads(plain.read_text(encoding="utf-8"))
    return None


def compare(out_dir: Path, golden_dir: Path) -> dict:
    dr = _load_dualrun()
    status = json.loads((out_dir / "status.json").read_text(encoding="utf-8"))
    results = {}
    for probe, st in status["probes"].items():
        entry = {"ran": st["ran"], "rc": st["rc"], "end_marker": st["end_marker"],
                 "json_ok": st["json_ok"]}
        vm = None
        if st["json_ok"]:
            vm = json.loads((out_dir / f"{probe}.json").read_text(encoding="utf-8"))
        win = _golden(golden_dir, probe)
        entry["golden_found"] = win is not None
        if win is None:
            entry["verdict"] = "NO_GOLDEN"
            results[probe] = entry
            continue
        divs = dr.diff_jsons(win, vm, probe)
        ev = dr.evaluate_probe(probe, win, vm, divs)
        mismatched = [d for d in divs if d["severity"] != "S4"]
        total = _leaves(dr.normalize_json(win, probe))
        if vm is None:
            # No VM JSON: nothing matched. The comparator's single S1 "vm_execution" entry is
            # counted as one mismatch on top of the golden leaves, so report the golden size instead.
            matched = 0
            mismatch_count = total
        else:
            matched = max(0, total - len(mismatched))
            mismatch_count = len(mismatched)
        entry.update({
            "verdict": ev["verdict"],
            "reason": ev["reason"],
            "golden_leaves": total,
            "mismatched_fields": mismatch_count,
            "matched_fields": matched,
            "s1": ev["s1"], "s2": ev["s2"], "s3": ev["s3"],
            "first_mismatches": [
                {"field": d["field"], "severity": d["severity"], "note": d["note"],
                 "windows": str(d["windows"])[:160], "vm": str(d["vm"])[:160]}
                for d in mismatched[:5]
            ],
        })
        results[probe] = entry
    summary = {"golden_dir": str(golden_dir), "probes": results,
               "printk_drop_lines": status["printk_drop_lines"],
               "all_done_seen": status["all_done_seen"]}
    (out_dir / "compare.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    return summary


def main(argv):
    if len(argv) >= 4 and argv[1] == "split":
        s = split(Path(argv[2]), Path(argv[3]))
        for name, st in s["probes"].items():
            print(f"{name}: rc={st['rc']} end={st['end_marker']} json={st['json_ok']} bytes={st['payload_bytes']}")
        print(f"printk drop lines: {s['printk_drop_lines']}  all_done: {s['all_done_seen']}")
        return 0
    if len(argv) >= 4 and argv[1] == "compare":
        s = compare(Path(argv[2]), Path(argv[3]))
        for name, e in s["probes"].items():
            print(f"{name}: verdict={e.get('verdict')} mismatched={e.get('mismatched_fields')} "
                  f"matched={e.get('matched_fields')} of {e.get('golden_leaves')}")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
