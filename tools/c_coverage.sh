#!/bin/sh
# tools/c_coverage.sh - gcov line coverage for the C host test suites.
#
# Builds and runs three suites with COVERAGE=1 (gcov instrumentation, -O0,
# objects in tests/kernel-host/build-cov, tests/host/out-cov and
# tests/ntdll-host/out-cov), runs gcov over the counters, and prints:
#
#   1. per-file line coverage for src/kernel/nt/*.c, src/loader/*.c and
#      src/ntdll/*.c. Test drivers (*_test.c, *tests.c, repro_mem_vad.c) are
#      skipped. A file no suite compiles is printed as "not built".
#   2. the 15 uncovered functions (0 lines executed) with the most executable
#      lines, as a size measure.
#
# Usage:
#   sh tools/c_coverage.sh             run every suite, then report
#   sh tools/c_coverage.sh --no-run    report from the existing counters only
#
# A failing test does not stop the report: the counters up to the failure are
# still written. Suite results are printed first; the exit status is 1 if any
# suite or test failed. kernel-host "check" takes minutes (sync_test is slow),
# so --no-run is useful while iterating on the report.
#
# Toolchain: MSYS2 UCRT64 gcc with gcov (C:/Users/Jackson/repos/TOOLCHAIN.md).
# Installs nothing; exits 2 if gcov is missing.

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 2
cd "$ROOT" || exit 2

if [ -d /c/msys64/ucrt64/bin ]; then
  PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
  export PATH
fi
if ! command -v gcov >/dev/null 2>&1; then
  echo "c_coverage: gcov not found on PATH (MSYS2 UCRT64 gcc ships it); nothing installed" >&2
  exit 2
fi

NO_RUN=0
case "${1:-}" in
  "") ;;
  --no-run) NO_RUN=1 ;;
  *) echo "usage: sh tools/c_coverage.sh [--no-run]" >&2; exit 2 ;;
esac

status=0
KDIR=tests/kernel-host
HDIR=tests/host
NDIR=tests/ntdll-host

if [ "$NO_RUN" -eq 0 ]; then
  echo "== kernel-host (make check)"
  if make -C "$KDIR" COVERAGE=1 check; then
    echo "kernel-host: pass"
  else
    echo "kernel-host: FAIL"; status=1
  fi

  echo "== host (make test)"
  if make -C "$HDIR" COVERAGE=1 test; then
    echo "host: pass"
  else
    echo "host: FAIL"; status=1
  fi

  echo "== ntdll-host (build, then each test binary)"
  if make -C "$NDIR" COVERAGE=1 build; then
    for bin in $(make -s -C "$NDIR" COVERAGE=1 print-run-bins); do
      (cd "$NDIR" && "./$bin" >/dev/null 2>&1); rc=$?
      if [ "$rc" -eq 0 ]; then
        echo "  $bin: pass"
      else
        echo "  $bin: FAIL (rc=$rc)"; status=1
      fi
    done
  else
    echo "ntdll-host: build FAILED"; status=1
  fi
fi

# gcov over every counter file. -f prints per-function summaries; gcov prints
# each source's functions before that source's "File" summary, so awk attaches
# the functions to the file that follows. -n writes no .gcov files.
RAW="${TEMP:-/tmp}/c_coverage_raw_$$.txt"
: > "$RAW"; part_n=0
for gcda in "$KDIR/build-cov"/*.gcda "$HDIR/out-cov"/*.gcda "$NDIR/out-cov"/*.gcda; do
  [ -f "$gcda" ] || continue
  case "$gcda" in
    "$HDIR"/*) suite=host ;;
    "$NDIR"/*) suite=ntdll-host ;;
    *)         suite=kernel-host ;;
  esac
  part_n=$((part_n + 1)); part="$RAW.part$part_n"
  gcov -n -f -o "$(dirname "$gcda")" "$gcda" 2>/dev/null | tr -d '' | awk -v suite="$suite" '
    # "Function '"'"'name'"'"'" is 10 characters before the name, "File '"'"'" is 6.
    function name(s, pre) { s = substr(s, pre + 1); return substr(s, 1, length(s) - 1) }
    function norm(s) { sub(/^.*src\//, "src/", s); return s }
    /^Function \x27/ { mode = "fn"; fname = name($0, 10); next }
    /^File \x27/     { mode = "file"; file = norm(name($0, 6)); got = 0; next }
    /^Lines executed:/ {
      split($0, a, /[:% ]+/)              # "Lines" "executed" pct "of" total
      pct = a[3] + 0; total = a[5] + 0
      if (mode == "fn") {
        nf++; fn_name[nf] = fname; fn_pct[nf] = pct; fn_total[nf] = total
      } else if (mode == "file" && !got) {
        got = 1
        printf "F\t%s\t%d\t%d\t%.2f\t%s\n", file, int(pct * total / 100 + 0.5), total, pct, suite
        for (i = 1; i <= nf; i++)
          printf "U\t%s\t%s\t%.2f\t%d\t%s\n", file, fn_name[i], fn_pct[i], fn_total[i], suite
        nf = 0
      }
    }
  ' > "$part"
done
cat "$RAW".part* >> "$RAW" 2>/dev/null
rm -f "$RAW".part*

# A file can appear in more than one counter file; keep its best count.
awk -F '\t' 'BEGIN { OFS = "\t" }
  $1 == "F" { if (!($2 in best) || $3 > best[$2]) { best[$2] = $3; tot[$2] = $4; pc[$2] = $5; who[$2] = $6 } }
  END { for (f in best) print "F", f, best[f], tot[f], pc[f], who[f] }
' "$RAW" > "$RAW.files"
awk -F '\t' 'BEGIN { OFS = "\t" } $1 == "U" { print }' "$RAW" | sort -u > "$RAW.fns"

echo
echo "Per-file line coverage (src/kernel/nt, src/loader, src/ntdll; test drivers excluded):"
awk -F '\t' '
  $1 == "F" { ex[$2] = $3; tot[$2] = $4; sfx[$2] = $6 }
  END {
    print "| File | Suite | Lines executed | Lines | Line coverage |"
    print "|---|---|---:|---:|---:|"
    n = split("src/kernel/nt src/loader src/ntdll", groups, " ")
    for (g = 1; g <= n; g++) {
      cmd = "ls " groups[g] "/*.c 2>/dev/null"
      while ((cmd | getline f) > 0) {
        if (f ~ /_test\.c$/ || f ~ /tests\.c$/ || f ~ /repro_mem_vad\.c$/) continue
        if (f in tot && tot[f] > 0)
          printf "| `%s` | %s | %d | %d | %.2f%% |\n", f, sfx[f], ex[f], tot[f], ex[f] * 100 / tot[f]
        else
          printf "| `%s` | not built | - | - | - |\n", f
      }
      close(cmd)
    }
  }
' "$RAW.files"

echo
echo "Uncovered functions (0 lines executed), largest first by executable lines:"
awk -F '\t' 'BEGIN { OFS = "\t" }
  $1 == "U" && $4 + 0 == 0 && $5 + 0 > 0 && $2 ~ /^src\/(kernel\/nt|loader|ntdll)\// && $2 !~ /(_test\.c|tests\.c|repro_mem_vad\.c)$/ {
    print $5, $2, $3
  }' "$RAW.fns" | sort -t "$(printf '\t')" -k1,1nr | head -15 | awk -F '\t' '
  BEGIN { print "| # | Function | File | Executable lines |"; print "|---:|---|---|---:|" }
  { printf "| %d | `%s` | `%s` | %d |\n", NR, $3, $2, $1 }
'

rm -f "$RAW" "$RAW.files" "$RAW.fns"
exit $status
