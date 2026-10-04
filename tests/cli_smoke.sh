#!/usr/bin/env bash
# runs the real `embr` binary on malformed and edge-case scripts and checks it never dies from a signal or
# an uncaught C++ exception (exit >= 128 / "terminate called") and reports a readable error instead.
# the unit tests call the library directly, so a bug in the CLI layer (src/cli.cpp) -- like lexer errors
# escaping main() and aborting the tree-walker -- is invisible to them.
#
#   cli_smoke.sh <embr binary> [--vm]
set -u
embr="$1"; shift
flags=("$@")   # (empty array under set -u needs the ${flags[@]+...} form, bash 3.2 on macos)
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

check() {   # name, source, expected substring on stderr+stdout (or "" for "anything, but no crash")
    local name="$1" src="$2" want="$3"
    printf '%s\n' "$src" > "$work/t.embr"
    local out rc
    out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/t.embr" 2>&1)"; rc=$?
    if (( rc >= 128 )) || [[ "$out" == *"terminate called"* ]]; then
        echo "FAIL [$name]: crashed (exit $rc): ${out:0:200}"; fail=1; return
    fi
    if [[ -n "$want" && "$out" != *"$want"* ]]; then
        echo "FAIL [$name]: expected output containing '$want', got: ${out:0:200}"; fail=1; return
    fi
}

check "unknown escape"        'print("\q")'                  "unknown escape"
check "unterminated string"   'print("abc'                   "unterminated string"
check "multiple decimal pts"  'x = 1.2.3'                    "multiple decimal points"
check "trailing decimal pt"   'x = 5.'                       "trailing decimal point"
check "bad \x escape"         'print("\x4")'                 "hex digits"
check "bad \u escape"         'print("\uD800")'              "Unicode scalar"
check "number out of range"   'print(1e999)'                 "out of range"
check "int literal overflow"  'print(99999999999999999999999)' ""
check "deep nesting"          "x = $(printf '(%.0s' $(seq 1 20000))1$(printf ')%.0s' $(seq 1 20000))" "nesting too deep"
check "long operator chain"   "x = 1$(printf '+1%.0s' $(seq 1 20000))" "too deeply nested"
check "bare return at EOF"    'return'                       "unexpected token"
check "top-level return"      'print("a")
return 1
print("b")'                                                  ""
check "stray break"           'break'                        ""
check "runtime error + trace" 'fn f()
  error("boom")
end
f()'                                                          "stack trace"
check "undefined variable"    'print(nope_not_defined)'      "undefined variable"
check "empty file"            ''                             ""
check "only comments"         '# hi
#[ block ]#'                                                 ""

# ---- `embr check`: parse + compile without running (must not execute anything, must report every error)
printf 'print("SHOULD NOT RUN")\nx = 1\n' > "$work/good.embr"
out="$("$embr" check "$work/good.embr" 2>&1)"; rc=$?
[[ $rc -eq 0 && -z "$out" ]] || { echo "FAIL [check good file]: rc=$rc out=$out"; fail=1; }
printf 'fn f(a,\n  return a\nend\nprint("x")\nbreak\n' > "$work/bad.embr"
out="$("$embr" check "$work/bad.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 ]] || { echo "FAIL [check bad file]: expected exit 1, got $rc"; fail=1; }
[[ "$out" == *"bad.embr:2:"* && "$out" == *"bad.embr:5:"* ]] || { echo "FAIL [check bad file]: expected two located errors, got: $out"; fail=1; }
printf 'print("a\\q")\n' > "$work/lex.embr"
out="$("$embr" check "$work/lex.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"lex.embr:1:"*"unknown escape"* ]] || { echo "FAIL [check lexer error]: rc=$rc out=$out"; fail=1; }
out="$("$embr" check "$work/good.embr" "$work/bad.embr" "$work/none.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"none.embr: error: cannot open"* ]] || { echo "FAIL [check several files]: rc=$rc out=$out"; fail=1; }
"$embr" check >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [check with no file]: expected exit 2"; fail=1; }

# ---- `embr test`: pass/fail accounting, exit status, filter, discovery, setup/teardown
mkdir -p "$work/tests"
cat > "$work/tests/test_a.embr" <<'EMBR'
import "testing.embr"
fn test_ok() assert_eq(1, 1) end
fn test_bad() assert_eq(1, 2) end
fn helper_not_a_test() error("must not run") end
EMBR
cat > "$work/tests/b_test.embr" <<'EMBR'
import "testing.embr"
fn test_fine() assert_true(1) end
EMBR
printf 'x = = 1\n' > "$work/tests/test_syntax.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} test "$work/tests" 2>&1)"; rc=$?
[[ $rc -eq 1 ]] || { echo "FAIL [embr test with failures]: expected exit 1, got $rc"; fail=1; }
[[ "$out" == *"PASS test_ok"* && "$out" == *"FAIL test_bad"* && "$out" == *"PASS test_fine"* ]] || { echo "FAIL [embr test results]: $out"; fail=1; }
[[ "$out" == *"2 passed"*"3 failed"* || "$out" == *"2 passed, 2 failed"* ]] || { echo "FAIL [embr test summary]: ${out: -120}"; fail=1; }
[[ "$out" != *"must not run"* ]] || { echo "FAIL [embr test ran a non-test function]"; fail=1; }
[[ "$out" == *"while loading"* ]] || { echo "FAIL [embr test syntax error file]: $out"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} test --filter=fine "$work/tests/b_test.embr" 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == *"1 passed, 0 failed"* ]] || { echo "FAIL [embr test --filter / single file]: rc=$rc $out"; fail=1; }
"$embr" ${flags[@]+"${flags[@]}"} test "$work/nothing_here" >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [embr test missing path]: expected exit 2"; fail=1; }
mkdir -p "$work/empty"; "$embr" ${flags[@]+"${flags[@]}"} test "$work/empty" >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [embr test no test files]: expected exit 2"; fail=1; }

# piped REPL: bare expressions echo their value, statements and blocks run quietly, no prompt noise
out="$(printf 'x = 5\nx * 2\nprint("hi")\nif x > 1\nprint("blk")\nend\n' | "$embr" ${flags[@]+"${flags[@]}"} 2>&1)"
[[ "$out" == $'10\nhi\nblk' ]] || { echo "FAIL [piped repl echo]: $out"; fail=1; }

# a missing script file and --version must also behave
"$embr" ${flags[@]+"${flags[@]}"} "$work/does_not_exist.embr" >/dev/null 2>&1; (( $? >= 128 )) && { echo "FAIL: missing file crashed"; fail=1; }
"$embr" --version | grep -q "backends:" || { echo "FAIL: --version"; fail=1; }

(( fail )) && exit 1
echo "cli smoke OK"
