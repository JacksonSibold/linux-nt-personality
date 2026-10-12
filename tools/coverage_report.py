#!/usr/bin/env python3
"""
coverage_report.py - line coverage for the Python tooling (standard library only).

Runs the unit tests in-process under sys.settrace and reports, per file, how
many executable statement lines ran. Measured files:

  build/*.py
  tools/*.py
  src/kernel/nt/gen_ntstatus_table.py

Executable lines come from the AST: every statement line, except docstrings.
Code that runs only in a subprocess is not seen by the tracer, so the numbers
are a lower bound for the scripts that the tests drive through a child process.

Usage:
  python3 tools/coverage_report.py            run tests/unit and print the report
  python3 tools/coverage_report.py --selftest  check the measurement on a fixture

Exit status: 0 report printed and all unit tests passed, 1 a unit test failed,
2 usage error.
"""
import argparse
import ast
import contextlib
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TEST_DIR = REPO / "tests" / "unit"


def target_files(repo=REPO):
    """The files the report covers, as absolute paths in a stable order."""
    files = sorted((repo / "build").glob("*.py"))
    files += sorted((repo / "tools").glob("*.py"))
    files.append(repo / "src" / "kernel" / "nt" / "gen_ntstatus_table.py")
    return files


def executable_lines(path):
    """Line numbers of statements in the file, docstrings excluded."""
    tree = ast.parse(Path(path).read_bytes(), filename=str(path))
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            first = node.body[0] if node.body else None
            if (isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant)
                    and isinstance(first.value.value, str)):
                docstrings.add(first)
    return {node.lineno for node in ast.walk(tree)
            if isinstance(node, ast.stmt) and node not in docstrings}


def _key(path):
    return os.path.normcase(os.path.realpath(str(path)))


class LineTracer:
    """Records the line events of frames whose code lives in a target file."""

    def __init__(self, paths):
        self.hits = {_key(p): set() for p in paths}
        self._seen = {}  # co_filename -> normalized key, or None for other files

    def global_trace(self, frame, event, arg):
        filename = frame.f_code.co_filename
        if filename not in self._seen:
            key = _key(filename)
            self._seen[filename] = key if key in self.hits else None
        if self._seen[filename] is None:
            return None
        return self._local_trace

    def _local_trace(self, frame, event, arg):
        if event == "line":
            self.hits[self._seen[frame.f_code.co_filename]].add(frame.f_lineno)
        return self._local_trace


def measure(run, paths):
    """Call run() with tracing on. Returns (run's result, {key: set of hit lines})."""
    tracer = LineTracer(paths)
    sys.settrace(tracer.global_trace)
    try:
        result = run()
    finally:
        sys.settrace(None)
    return result, tracer.hits


def summarize(paths, hits):
    """Rows of (display name, executable, covered) for each path, in order."""
    rows = []
    for path in paths:
        key = _key(path)
        if not Path(path).is_file():
            rows.append((str(path), 0, 0))
            continue
        exe = executable_lines(path)
        covered = len(exe & hits.get(key, set()))
        rows.append((str(path), len(exe), covered))
    return rows


def display_name(path, repo=REPO):
    try:
        return Path(path).relative_to(repo).as_posix()
    except ValueError:
        return str(path)


def render(rows, repo=REPO):
    def pct(exe, cov):
        return f"{100.0 * cov / exe:6.1f}%" if exe else "   n/a"

    width = max([len("file")] + [len(display_name(p, repo)) for p, _, _ in rows])
    out = [f"{'file'.ljust(width)}  {'lines':>6}  {'covered':>7}  {'pct':>7}"]
    total_exe = total_cov = 0
    for path, exe, cov in rows:
        total_exe += exe
        total_cov += cov
        name = display_name(path, repo)
        out.append(f"{name.ljust(width)}  {exe:>6}  {cov:>7}  {pct(exe, cov)}")
    out.append(f"{'TOTAL'.ljust(width)}  {total_exe:>6}  {total_cov:>7}  "
               f"{pct(total_exe, total_cov)}")
    return "\n".join(out) + "\n"


def selftest():
    """Measure a fixture module with one branch never taken. Returns an exit status."""
    fails = []

    def check(cond, label):
        if not cond:
            fails.append(label)

    source = (
        '"""Fixture module."""\n'           # 1: docstring, not executable
        "def f(x):\n"                       # 2: runs at import
        "    if x:\n"                       # 3: runs
        "        return 1\n"                # 4: runs
        "    return 2\n"                    # 5: never runs
        "def g():\n"                        # 6: runs at import
        "    return 3\n"                    # 7: never runs
    )
    with tempfile.TemporaryDirectory() as tmp:
        fixture = Path(tmp) / "cov_fixture_mod.py"
        fixture.write_text(source, encoding="utf-8")

        check(executable_lines(fixture) == {2, 3, 4, 5, 6, 7},
              f"executable lines {sorted(executable_lines(fixture))}")

        def run():
            spec = importlib.util.spec_from_file_location("cov_fixture_mod", fixture)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod.f(1)

        value, hits = measure(run, [fixture])
        check(value == 1, f"fixture call returned {value}")
        check(hits[_key(fixture)] >= {2, 3, 4}, f"hits {sorted(hits[_key(fixture)])}")
        check(not hits[_key(fixture)] & {5, 7}, "untaken lines were marked hit")

        rows = summarize([fixture, Path(tmp) / "missing.py"], hits)
        check(rows[0][1:] == (6, 4), f"fixture row {rows[0]}")
        check(rows[1][1:] == (0, 0), f"missing row {rows[1]}")
        text = render(rows, repo=Path(tmp))
        check("cov_fixture_mod.py" in text and "TOTAL" in text, "rendered table")
        check("66.7%" in text, "fixture percentage rendered")

    # The tracer must stop when the measured call returns.
    check(sys.gettrace() is None, "tracer left installed")

    if fails:
        for f in fails:
            print(f"selftest FAIL: {f}", file=sys.stderr)
        return 1
    print("selftest: coverage measurement checks passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Line coverage for build/, tools/ and the kernel table generator.")
    parser.add_argument("--selftest", action="store_true",
                        help="check the measurement on a built-in fixture and exit")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if not TEST_DIR.is_dir():
        print(f"coverage_report: error: no test directory at {TEST_DIR}", file=sys.stderr)
        return 2

    paths = target_files()
    loader = unittest.TestLoader()
    runner = unittest.TextTestRunner(stream=sys.stderr, verbosity=1)
    with contextlib.redirect_stdout(sys.stderr):
        result, hits = measure(
            lambda: runner.run(loader.discover(str(TEST_DIR), top_level_dir=str(TEST_DIR))),
            paths)
    sys.stdout.write(render(summarize(paths, hits)))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
