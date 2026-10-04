#!/usr/bin/env bash
# guards against "someone made the VM slower". runs the benchmarks (tools/bench/run.sh) and compares each
# vm/tree-walker ratio with tools/bench/baseline.txt. a ratio instead of a time, so a slow or fast machine
# doesn't matter, only the VM against the tree-walker on the same machine.
#
#   tools/bench/check.sh                 # RUNS=5, TOL=0.35
#   TOL=0.5 RUNS=9 tools/bench/check.sh fib loop
#   UPDATE=1 tools/bench/check.sh        # print a new baseline.txt from this run instead of checking
#   EMBR=path/to/embr tools/bench/check.sh
#
# needs the same build as run.sh (both backends, Release for real numbers). exit 1 on a regression.
# when GITHUB_STEP_SUMMARY is set, the table is written there too.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tol="${TOL:-0.35}"
export RUNS="${RUNS:-5}"

table="$("$here/run.sh" "$@")"
echo "$table"

# lines look like: name   1.234s   0.456s   0.37x
rows="$(echo "$table" | awk '$4 ~ /x$/ { r = $4; sub(/x$/, "", r); print $1, r }')"
[[ -n "$rows" ]] || { echo "check.sh: no benchmark results found" >&2; exit 2; }

if [[ "${UPDATE:-}" == 1 ]]; then
    echo "# vm time divided by tree-walker time, per benchmark. best of $RUNS, $("${EMBR:-$here/../../bin/linux/embr}" --version)"
    echo "# tools/bench/check.sh fails when a ratio is more than TOL (default 35%) above its line here."
    echo "$rows" | awk '{ printf "%-8s %s\n", $1, $2 }'
    exit 0
fi

fail=0
report="| benchmark | baseline | now | limit | |
|---|---|---|---|---|
"
while read -r name now; do
    base="$(awk -v n="$name" '$1 == n { print $2 }' "$here/baseline.txt")"
    if [[ -z "$base" ]]; then report+="| $name | (none) | $now | | not in baseline.txt |
"; continue; fi
    limit="$(echo "$base * (1 + $tol)" | bc -l)"
    if (( $(echo "$now > $limit" | bc -l) )); then mark="SLOWER"; fail=1; else mark="ok"; fi
    report+="$(printf '| %s | %.2fx | %.2fx | %.2fx | %s |' "$name" "$base" "$now" "$limit" "$mark")
"
done <<< "$rows"

echo; echo "vm/tree-walker ratio against tools/bench/baseline.txt (tolerance $tol)"; echo "$report"
[[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && { echo "### embr benchmarks, vm/tree-walker ratio"; echo "$report"; } >> "$GITHUB_STEP_SUMMARY"
if (( fail )); then echo "check.sh: a benchmark got slower relative to the tree-walker (see SLOWER above)" >&2; exit 1; fi
echo "check.sh: ok"
