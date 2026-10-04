# embr

embr (Embeddable/Extensible Meta-Binding Runtime) & (EMBR Makes Binding Reliable)
was created solely because i wanted a single header scripting language and the lua vm pissed me off. with this, you can include one file and be able to use it for configs, scripting and dynamic plugin loading, or download a release (a tarball on the github releases page, unpack it anywhere, `bin/embr` finds its plugins by itself) and use the cli interpreter to run scripts.

![license](https://img.shields.io/badge/license-GPL%203.0-blue.svg) ![version](https://img.shields.io/badge/version-1-green.svg)

## table of contents

- [features](#features)
- [using as lib](#lib)
- [building from source](#building)
- [release (cli interpreter) usage](#cli)
- [documentation](#documentation)
- [contributing](#contributing)
- [faq](#faq)
- [license](#license)

## features

- minimalistic syntax with 18 reserved keywords
- dynamically typed variables
- optional function arg and return typechecking
- python-esque module system (`import` works for native plugins and for `.embr` files)
- C ABI plugin support, and a good pile of plugins already: strings, json, csv, regex, files, http(s), threads, coroutines, ...
- built-in FFI plugin for calling C libraries (via libffi)
- first class functions and closures with full upvalue capture
- number, int, string, array, map, callable, pointer data types (no bool or nil type on purpose, see the [type system](https://github.com/la-creatura/embr-lang/wiki/type-system) page)
- two backends: a tree-walker (simple, the default) and a bytecode vm (faster)
- `--sandbox` mode to run a script that shouldn't be touching your files, processes or network
- `embr test`, `embr check`, `embr fmt` (formatter), `embr lsp` (editor errors) and a small package manager (`embr pkg`)

## lib

the headers under `include/embr/` are glued into one file at build time, `embr.h`. it ends up at `build/<preset>/generated/embr/embr.h`, copy that into your project and `#include "embr.h"` is all you need to access everything. run embr code with
```cpp
embr::Interpreter interp;  // interpreter context with all the variables and stuff
embr::runSource("print(\"hewwo wowwd!\")", interp);
```
`embr::runSource` uses the tree-walker. if your `embr.h` was built with the vm, `embr::vm::runSource(src, interp)` runs the same thing on the vm instead

plugins (`import "json"` and friends) are separate `.so` files, they aren't inside `embr.h`. to use them from your own program, keep them next to your executable or point `EMBR_PATH` at them

#### sophisticated route

under the hood, `runSource()` is just a convenience wrapper for this
```cpp
inline void runSource(const std::string& src, Interpreter& interp,
                      const std::string& filename = "<input>") {
    Lexer lex(src);
    auto tokens = lex.tokenize();
    SourceMap sm = lex.sourceMap();
    Parser parser(std::move(tokens), sm);
    auto prog = parser.parse();

    // transfer ownership before running
    const std::vector<StmtPtr>* stable = interp.storeProgram(std::move(prog));

    Runner runner(interp);
    try {
        runner.run(*stable, sm, filename);
    } catch (const EmbrError& e) {
        std::cerr << e.what() << formatTrace(e);
    }
}
```
you can store the program, inspect the AST, or call `runner.invoke()` directly. the [architecture](https://github.com/la-creatura/embr-lang/wiki/architecture) page explains how the pieces connect

## building
```bash
git clone --recurse-submodules https://github.com/la-cretura/embr.git
cd embr

# installing dev libffi package on linux
sudo apt install libffi-dev      # debian/ubuntu
sudo dnf install libffi-devel    # fedora

# optional: OpenSSL dev package, needed for https:// in the http plugin
# (without it the plugin still builds and plain http:// works; https:// raises a clear error,
#  and http_tls_available() returns 0). cmake prints a status line when it isn't found.
sudo apt install libssl-dev      # debian/ubuntu
sudo dnf install openssl-devel   # fedora

# building for linux
cmake --preset linux-release
cmake --build --preset linux-release

# the release preset only has the tree-walker. for the vm too (--vm), use the dual-backend presets
cmake --preset linux-vm-debug        # debug, for working on embr
cmake --build --preset linux-vm-debug
cmake --preset linux-vm-release      # optimized, what the release tarball is built from
cmake --build --preset linux-vm-release

# install it (bin/embr, lib/embr/plugins, lib/embr/modules, include/embr/embr.h) or make the tarball
cmake --install build/linux-vm-release --prefix ~/.local
cmake --build build/linux-vm-release --target package    # embr-<version>-linux-<arch>.tar.gz in the build dir

# building for windows (libffi bundled in source)
cmake --preset windows-release
cmake --build --preset windows-release
```
#### macos
**untested** - i dont have a mac. the build files are there (`macos-*` presets, `bin/macos/`, brew keg-only libffi/openssl are looked up for you, an `environ`/executable-path fix for macOS) and ci has an experimental macos job, but nobody has run it yet ¯\\_(ツ)_/¯
```bash
brew install libffi pkg-config openssl@3
cmake --preset macos-release
cmake --build --preset macos-release
```
if it fails, the error will probably be one line in a plugin, and a pr or issue with that line would be very welcome. known gaps: the ffi tests open `libSystem.B.dylib`, `os_exec` is posix-only (fine on mac), https certificate lookup uses openssl's default paths rather than the keychain.

#### the shared bin/ folder
every preset writes `embr`, the plugins and the test binaries into the same `bin/<platform>/` folder. building one preset after another overwrites the first one's output, and neither build notices. a typical symptom: you built `linux-debug` last, and `--vm` says "not configured with EMBR_WITH_VM".

- `./embr --version` shows which build directory made the binary and which backends it has. check it first when a run looks like the wrong build.
- the ctest test `bin_dir_stamp` fails with the fix when `bin/` came from a different build directory.
- to fix it, delete the old output and rebuild the preset you want:
```bash
rm -f bin/linux/embr bin/linux/tests/test_* bin/linux/plugins/*.so
cmake --build --preset linux-vm-debug
```
- for a throwaway config (say, vm only), build in a directory outside the repo so it doesn't touch `bin/`:
```bash
cmake -S . -B /tmp/vmonly -DEMBR_WITH_TREE_WALKER=OFF -DEMBR_WITH_VM=ON && cmake --build /tmp/vmonly
```

#### tests
```bash
ctest --test-dir build/linux-vm-debug --output-on-failure
```
- on a dual-backend build every suite runs twice, `<name>_tw` and `<name>_vm`. each test runs in its own folder, `bin/<platform>/test-work/<name>`, with `EMBR_PATH` set, so don't assume the working directory.
- a new `tests/*_test.cpp` needs a re-configure (`cmake --preset ...`), a plain build won't find it.
- if a test crashes with no output, the output may just be unflushed. rerun with `stdbuf -oL -eL`.
- sanitizers (asan, ubsan) run in ci. fuzzing: `tools/fuzz/run.sh`. benchmarks: `tools/bench/run.sh` (needs a release build with both backends, which overwrites `bin/`).

#### debugging
- shrink the problem to a tiny script and run it with a timeout: `timeout 5 ./embr repro.embr; echo $?` (124 means it hung).
- when the tree-walker and the vm might disagree, run the same script both ways and diff: `./embr x.embr` and `./embr --vm x.embr`.
- the build is expected to be warning free (`-Werror` in ci). gcc's `-Wdangling-reference` gives false positives on helpers that return a reference, so those helpers return a pointer instead.

## cli

```bash
./embr myscript.embr
```
or
```bash
./embr
```
for repl

```bash
./embr --vm myscript.embr        # run on the bytecode vm instead of the tree-walker (needs a build with the vm, see building)
./embr myscript.embr -- a b      # arguments after -- are read in the script with os_args()
./embr --version                 # which build this is, and which backends it has
./embr check a.embr b.embr       # parse + compile only, nothing runs; errors as file:line:col: message
./embr fmt a.embr                # format in place (--check just lists files that would change, --stdout prints)
./embr lsp                       # language server on stdin/stdout, shows errors as you type in any lsp editor
./embr test tests/               # run test_*.embr files (test_* functions, assertions in testing.embr)
./embr --sandbox untrusted.embr  # imports limited to pure plugins, no file access (see the [syntax](https://github.com/la-creatura/embr-lang/wiki/syntax) page, sandbox mode)
./embr pkg install <name>        # package manager, see the pkg.embr page in the wiki
```

## documentation

all the docs live in the [wiki](https://github.com/la-creatura/embr-lang/wiki):

- [tutorial](https://github.com/la-creatura/embr-lang/wiki/tutorial) - learn embr in 15 minutes, start here if you want to write scripts
- [architecture](https://github.com/la-creatura/embr-lang/wiki/architecture) - how the pieces fit together, start here if you're new to the code
- [reference](https://github.com/la-creatura/embr-lang/wiki/home) - the language and runtime reference (grammar, types, scoping, errors, modules, backends)
- [plugin reference](https://github.com/la-creatura/embr-lang/wiki/plugin-reference) - one page per plugin/module (str, json, fs, os, path, csv, encoding, random, unicode, regex, time, table, vec, http, async, parallel, coroutine, gc, ffi, ...)
- [writing plugins](https://github.com/la-creatura/embr-lang/wiki/writing-plugins) - tutorial for writing a native plugin
- [examples/](examples/) - small runnable programs; they are run by the test suite so they stay correct

## contributing

contributions are welcome. follow these steps:

1. fork the repo
2. create your feature branch (`git checkout -b feature/pawsome-feature`)
3. commit your changes (`git commit -m 'add pawsome feature'`)
4. push to the branch (`git push origin feature/pawsome-feature`)
5. open a pull request

## faq

#### does it support XYZ

¯\\_(ツ)_/¯
only tested on linux mint 22.3 64 bit and windows 10 64 bit

#### how do i ask for a new feature

contact me on discord @alice_was_taken or like open an issue or something

#### im having XYZ issue

contact me on discord or open an issue

## license

this project is licensed under the gpl-3.0. see [LICENSE](LICENSE) for details

---

made with ❤ by [alice](https://github.com/la-creatura)
