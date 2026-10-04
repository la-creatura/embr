# embr benchmarks

```bash
# needs an embr built with BOTH backends. use a Release build for real numbers
# (a Debug build measures -O0 code):
cmake -S . -B /tmp/embr-rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/linux-gcc.cmake \
      -DEMBR_WITH_TREE_WALKER=ON -DEMBR_WITH_VM=ON
cmake --build /tmp/embr-rel --target embr embrlib_plugin -j"$(nproc)"
tools/bench/run.sh                 # best of RUNS=3, all benchmarks
tools/bench/run.sh fib closure     # only these
```

`tools/bench/check.sh` is the regression guard CI runs: it compares each vm/tree-walker ratio with
`baseline.txt` and fails if one is more than 35% worse (`TOL=0.5` to loosen, `UPDATE=1` to print a new baseline
after a real speedup).

Careful: `bin/linux/` is shared by every build directory, so the
build above overwrites whatever was there. Rebuild `linux-vm-debug` afterwards.
The runner refuses to report a timing if the two backends print different
output for a benchmark.

## Baseline (Release, gcc 13, 2026-10-03, best of 5)

| benchmark | tree-walker | vm | vm/tree |
|---|---|---|---|
| array   (200k push, 1M reads, 600k writes, 200k append) | 0.836s | 0.502s | 0.60x |
| calls   (600k small calls)          | 5.425s | 0.740s | 0.14x |
| closure (500k closure calls)        | 1.541s | 0.299s | 0.19x |
| fib     (fib(27))                   | 2.551s | 0.315s | 0.12x |
| loop    (3M iterations)             | 0.953s | 0.540s | 0.57x |
| map     (100k inserts, lookups and overwrites) | 0.238s | 0.178s | 0.75x |
| strings (100k `s = s + "ab"`)       | 0.641s | 0.024s | 0.04x |
| vec     (200k push + 600k set/get)  | 0.580s | 0.466s | 0.80x |

## What this says (hot-spot leads, not conclusions)

* The VM wins big where the tree-walker pays per-call overhead (calls, closures,
  recursion: 5-8x). straight-line loops are now about 1.8x faster than the tree-walker
  (they were level). two things did that: a variable that has a slot is read and written
  straight from the frame's slot array, and top-level variables are remembered per name
  (`CompiledChunk::GlobalCache`) instead of hashed on every access. the remembered pointer is only
  used while `Interpreter::scopeEpoch()` is unchanged, and that number changes whenever a scope is
  pushed, popped, or gains or loses a name.
* `s = s + x` is one `APPEND_VAR` op that grows the string in place, and `+` on two ints works on the
  stack without pops and pushes.
* **`arr[i]`, `m[k]`, `arr[i] = x`, `m[k] = x` and `arr = push(arr, x)` no longer copy the container** on either
  backend, when the container is a plain variable (VM: `INDEX_GET_VAR`, `INDEX_SET_VAR`, `APPEND_ARR`; tree-walker:
  `evalIndex`, `assignInPlace`, `appendInPlace`). they change the variable where it lives, so building and updating an
  n element array or map is O(n), not O(n^2). the arrays and maps in these benchmarks used to be kept at a few thousand
  elements for that reason. `append(&arr, x)` and the other in-out natives do the same for any variable or element.
  the only thing that could go wrong is the key or value code changing the variable before the op runs, so:
  * a key or value with no calls: nothing to worry about, no copy.
  * a call to a data-only builtin (`str`, `len`, `num`, ... see `isPureBuiltin` in `core/ast.h`): the same, as long as the
    name still means the builtin. if a script redefines `str`, that site copies.
  * any other call (`arr[f(i)]`): VM only. a local of the running function is still read in place, since only that
    function's own code can change it. a global (or a local a closure can see) is copied once, before the call runs.
    the tree-walker copies as it always did.
  what is still O(n): passing a whole array as a call argument (`len(a)` loads `a` first), `for x in arr`,
  and `arr = arr + [x]`. a `vec` (plugins/vec) is still the right tool for a big mutable list: see the `vec` benchmark.
