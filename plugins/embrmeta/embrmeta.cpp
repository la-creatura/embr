// embrmeta.cpp
// metaprogramming for embr: create, update and delete global variables and functions by a name computed at runtime
//
// usage
//   import "embrmeta"
//   meta_set("greet", fn(name) return "hi " + name end)
//   print(greet("world"))          # "hi world"
//   print(meta_has("greet"))       # 1
//   meta_undef("greet")
//   print(meta_has("greet"))       # 0
//
// why it's a plugin: `name = value` always targets a name written in the source, there is no syntax for "assign to
// the variable this string names". meta_set/meta_undef are that missing piece, on top of Interpreter::defineGlobal/
// undefineGlobal/hasGlobal (core/registry.h). "global" here is the same native scope bind() writes to, so a
// binding made this way acts like a real native function: visible from every module, not filtered by import
//
// main use: modules/ffi.embr parses a C declaration at runtime and installs a callable under the C function's own
// name (like `InitWindow`), which the module's source can't spell as an identifier
//
// raw bytecode injection (VmRunner::inject) is deliberately not exposed here. it works on VM internals with no
// script-level meaning, and global-scope changes don't need it (they work the same on both backends)

#include <embr/embr.h>

using namespace embr;

static Param pStr(std::string n) { return Param::req(std::move(n), TS::Str); }
static Param pAny(std::string n) { return Param::req(std::move(n), TS::Any); }

EMBR_PLUGIN {

    // meta_set(name: str, value: any) -> 0
    // creates or overwrites a global binding named `name`. works the same for a variable or a callable
    // (a Value is a Value), so there is no separate "define a function" call
    interp->bindSig("meta_set", {pStr("name"), pAny("value")},
    [interp](const std::vector<Value>& args) -> Value {
        interp->defineGlobal(args[0].asString(), args[1]);
        return Value(0.0);
    });

    // meta_get(name: str) -> any
    // reads a global binding by name: the one meta_set() or a plugin's bind() installed, even if a module scope
    // shadows that name right now (a plain variable reference would see the shadow). raises if it isn't defined,
    // so a typo fails loudly like a literal identifier would
    interp->bindSig("meta_get", {pStr("name")},
    [interp](const std::vector<Value>& args) -> Value {
        const std::string& name = args[0].asString();
        if (!interp->hasGlobal(name))
            raiseError("meta_get", "undefined global: " + name);
        return interp->getGlobal(name);
    });

    // meta_has(name: str) -> num
    interp->bindSig("meta_has", {pStr("name")},
    [interp](const std::vector<Value>& args) -> Value {
        return Value(interp->hasGlobal(args[0].asString()) ? 1.0 : 0.0);
    });

    // meta_undef(name: str) -> num
    // removes a global binding; returns 1 if something was actually
    // removed, 0 if it wasn't defined (a no-op, not an error, matches
    // fs_remove()'s "removing something already gone is fine" convention)
    interp->bindSig("meta_undef", {pStr("name")},
    [interp](const std::vector<Value>& args) -> Value {
        return Value(interp->undefineGlobal(args[0].asString()) ? 1.0 : 0.0);
    });
}
