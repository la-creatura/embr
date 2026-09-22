# embr

embr (Embeddable/Extensible Meta-Binding Runtime) & (EMBR Makes Binding Reliable)
was created solely because i wanted a single header scripting language and the lua vm pissed me off. with this, you can include one file and be able to use it for configs, scripting and dynamic plugin loading, or download a release and use the cli interpreter to run scripts.

![license](https://img.shields.io/badge/license-GPL%203.0-blue.svg) ![version](https://img.shields.io/badge/version-1-green.svg)

## table of contents

- [features](#features)
- [using as lib](#lib)
- [building from source](#building)
- [release (cli interpreter) usage](#cli)
- [configuration](#configuration)
- [contributing](#contributing)
- [faq](#faq)
- [license](#license)

## features

- minimalistic syntax with 10 reserved keywords
- dynamically typed variables
- optional function arg and return typechecking
- python-esque module system
- C ABI plugin support
- built-in FFI library
- first class functions and closures with full upvalue capture
- number, string, array, map, callable, pointer data types

## lib

a simple `#include "embr.h"` in your C++ script is all you need to access everything. run embr code with
```cpp
embr::Interpreter interp;  // interpreter context with all the variables and stuff
embr::runSource("print(\"hewwo wowwd!\")", interp);
```

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
    runner.run(*stable, sm, filename);
}
```
you can store the program, inspect the AST, or call `runner.invoke()` directly

## building
```bash
git clone --recurse-submodules https://github.com/la-cretura/embr.git
cd embr

# installing dev libffi package on linux
sudo apt install libffi-dev      # debian/ubuntu
sudo dnf install libffi-devel    # fedora

# building for linux
cmake --preset linux-release
cmake --build --preset linux-release

# building for windows (libffi bundled in source)
cmake --preset windows-release
cmake --build --preset windows-release
```
#### macos
you're on your own here i dont have a mac to test it on ¯\\_(ツ)_/¯ tell me if you figure something out

## cli

```bash
./embr myscript.embr
```
or
```bash
./embr
```
for repl

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
