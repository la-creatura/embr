#!/usr/bin/env bash
# runs every tools/bench/*.embr under the tree-walker and the VM and prints a
# comparison table.
#
#   tools/bench/run.sh                 # 3 runs each, best-of (default)
#   RUNS=10 tools/bench/run.sh         # more runs
#   EMBR=path/to/embr tools/bench/run.sh
#   tools/bench/run.sh fib loop        # only these benchmarks
#
# needs an embr built with both backends (linux-vm-debug or, better for real
# numbers, a release build configured with -DEMBR_WITH_VM=ON, a Debug build
# measures -O0 code). `embr --version` is printed first so the table says what
# it measured. uses hyperfine when installed, otherwise a plain timer (best of RUNS).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
embr="${EMBR:-$root/bin/linux/embr}"
runs="${RUNS:-3}"

[[ -x "$embr" ]] || { echo "embr not found at $embr (set EMBR=...)" >&2; exit 1; }
"$embr" --version
if ! echo 'print(1)' > /tmp/.embr_bench_probe.embr || ! "$embr" --vm /tmp/.embr_bench_probe.embr >/dev/null 2>&1; then
    echo "this embr has no VM backend; rebuild with EMBR_WITH_VM=ON" >&2; exit 1
fi
rm -f /tmp/.embr_bench_probe.embr

if (( $# )); then names=("$@"); else names=(); for f in "$here"/*.embr; do names+=("$(basename "$f" .embr)"); done; fi

# best-of-N wall time in seconds for one command
best() {
    local b="" t s e
    for ((k = 0; k < runs; k++)); do
        s=$(date +%s.%N); "$@" >/dev/null; e=$(date +%s.%N)
        t=$(echo "$e - $s" | bc -l)
        if [[ -z "$b" ]] || (( $(echo "$t < $b" | bc -l) )); then b=$t; fi
    done
    echo "$b"
}

printf '\n%-10s %12s %12s %10s\n' benchmark tree-walker vm "vm/tree"
printf '%-10s %12s %12s %10s\n' ---------- ------------ ------------ ----------
for n in "${names[@]}"; do
    f="$here/$n.embr"; [[ -f "$f" ]] || { echo "no such benchmark: $n" >&2; continue; }
    # correctness guard: both backends must print the same thing, or the timing is meaningless
    a="$("$embr" "$f" 2>&1 | grep -v '^\[runtime\]')"; b="$("$embr" --vm "$f" 2>&1 | grep -v '^\[runtime\]')"
    [[ "$a" == "$b" ]] || { echo "OUTPUT MISMATCH in $n: tree='$a' vm='$b'" >&2; exit 1; }
    tt=$(best "$embr" "$f"); tv=$(best "$embr" --vm "$f")
    printf '%-10s %11.3fs %11.3fs %9.2fx\n' "$n" "$tt" "$tv" "$(echo "$tv / $tt" | bc -l)"
done
echo
