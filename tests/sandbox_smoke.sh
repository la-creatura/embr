#!/usr/bin/env bash
# `--sandbox`, `--allow-plugins=` and `--no-plugins` (an import allow-list; see PluginPolicy in
# include/embr/core/registry.h) tested against the real binary, INCLUDING attempts to get around it:
# path tricks, a planted same-named library, a module that imports a denied plugin, a worker thread,
# reading a file through embrlib. a test "passes" only if the denied thing is really not reachable.
#
#   sandbox_smoke.sh <embr binary> [--vm]
set -u
embr="$1"; shift
flags=("$@")
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

run() {   # extra embr flags (space separated, may be empty), source  -> sets $out $rc
    local extra="$1" src="$2"
    printf '%s\n' "$src" > "$work/t.embr"
    # shellcheck disable=SC2086
    out="$(cd "$work" && "$embr" ${flags[@]+"${flags[@]}"} $extra "$work/t.embr" 2>&1)"; rc=$?
}
expect() {   # name, substring that must appear in the output
    [[ "$out" == *"$2"* ]] || { echo "FAIL [$1]: expected '$2' in: ${out:0:300}"; fail=1; }
}
refuse() {   # name, substring that must NOT appear
    [[ "$out" != *"$2"* ]] || { echo "FAIL [$1]: '$2' leaked: ${out:0:300}"; fail=1; }
}
nocrash() { (( rc < 128 )) || { echo "FAIL [$1]: crashed rc=$rc: ${out:0:200}"; fail=1; }; }

# ---- --sandbox: allowed plugins work, dangerous ones are refused with a catchable error
run "--sandbox" 'import "embrlib"
import "json"
import "str"
print(json_stringify([1, 2]))
print(str_upper("ok"))'
expect "sandbox allows pure plugins" "[1,2]"; expect "sandbox allows str" "OK"

for deny in os fs http ffi parallel async; do
    run "--sandbox" "try
  import \"$deny\"
  print(\"LOADED-$deny\")
catch e
  print(e[\"message\"])
end"
    expect "sandbox denies $deny" "is not allowed in this sandbox"; refuse "sandbox denies $deny" "LOADED-$deny"
done

# ---- path tricks: only bare names are accepted in a restricted sandbox
for trick in './json' '../plugins/json' '/tmp/json' 'plugins/json' 'json.so' '.'; do
    run "--sandbox" "try
  import \"$trick\"
  print(\"LOADED\")
catch e
  print(e[\"message\"])
end"
    expect "path trick $trick" "bare names"; refuse "path trick $trick" "LOADED"
done

# ---- a library planted next to the script (or in the cwd) must NOT be loaded for an allowed name
case "$(uname)" in Darwin) ext=dylib; plat=macos ;; *) ext=so; plat=linux ;; esac   # plugin extension and os_platform() here
cp "$(dirname "$embr")/plugins/json.$ext" "$work/plantedlib.$ext" 2>/dev/null
run "--allow-plugins=plantedlib" 'try
  import "plantedlib"
  print("LOADED-PLANTED")
catch e
  print("not loaded")
end'
expect "planted library" "not loaded"; refuse "planted library" "LOADED-PLANTED"
# ...but the same planted file IS found when unrestricted (proves the test itself can see it)
run "" 'import "plantedlib"
print("LOADED-PLANTED")'
expect "planted library is visible unrestricted (control)" "LOADED-PLANTED"

# ---- file access through embrlib is off in --sandbox, on with only --allow-plugins
printf 'secret' > "$work/secret.txt"
run "--sandbox" 'import "embrlib"
try
  print(load_file("'"$work"'/secret.txt"))
catch e
  print(e["message"])
end
try
  write_file("'"$work"'/pwned.txt", "x")
  print("WROTE")
catch e
  print(e["message"])
end'
expect "sandbox blocks load_file" "file access is disabled"; refuse "sandbox blocks load_file" "secret"; refuse "sandbox blocks write_file" "WROTE"
[[ ! -e "$work/pwned.txt" ]] || { echo "FAIL: write_file created a file in the sandbox"; fail=1; }
run "--allow-plugins=embrlib" 'import "embrlib"
print(load_file("'"$work"'/secret.txt"))'
expect "allow-plugins keeps embrlib file natives" "secret"

# ---- --allow-plugins is an exact list; --no-plugins allows none
run "--allow-plugins=str" 'import "str"
print(str_upper("a"))
try
  import "json"
  print("LOADED-JSON")
catch e
  print("json denied")
end'
expect "allow-plugins allows listed" "A"; expect "allow-plugins denies unlisted" "json denied"; refuse "allow-plugins" "LOADED-JSON"
run "--no-plugins" 'print("still runs")
try
  import "embrlib"
  print("LOADED")
catch e
  print("embrlib denied")
end'
expect "no-plugins: core still works" "still runs"; expect "no-plugins denies embrlib" "embrlib denied"; refuse "no-plugins" "LOADED"

# ---- a script module cannot smuggle in a denied plugin
printf 'import "os"\nmod_value = 1\n' > "$work/evilmod.embr"
run "--sandbox" 'try
  import "evilmod.embr"
  print("MODULE-LOADED")
catch e
  print(e["message"])
end'
expect "module importing os" "is not allowed"; refuse "module importing os" "MODULE-LOADED"
run "--sandbox" 'import "embrlib"
try
  load_module("evilmod")
  print("MODULE-LOADED")
catch e
  print(e["message"])
end'
expect "load_module importing os" "is not allowed"; refuse "load_module importing os" "MODULE-LOADED"

# ---- workers inherit the policy (only reachable if the user explicitly allows parallel/async)
printf 'import "os"\nfn go(x)\n  return os_getenv("HOME")\nend\n' > "$work/worker.embr"
run "--allow-plugins=embrlib,parallel,table" 'import "embrlib"
import "parallel"
inc = channel()
outc = channel()
w = worker_spawn("worker.embr", "go", inc, outc)
channel_send(inc, 1)
channel_close(inc)
r = channel_recv(outc)
print(r["ok"])
print(r["error"]["message"])
worker_join(w)'
expect "worker inherits policy" "is not allowed in this sandbox"; nocrash "worker policy"
printf 'import "os"\nfn go(x)\n  return os_getenv("HOME")\nend\n' > "$work/aworker.embr"
run "--allow-plugins=embrlib,async,table" 'import "embrlib"
import "async"
p = async_spawn("aworker.embr", "go", 1)
r = async_await(p)
print(r["ok"])
print(r["error"]["message"])'
expect "async worker inherits policy" "is not allowed in this sandbox"; nocrash "async worker policy"

# ---- default (no flag) is unchanged: everything loads
run "" 'import "os"
import "fs"
print(os_platform())'
expect "unrestricted default" "$plat"

(( fail )) && exit 1
echo "sandbox smoke OK"
