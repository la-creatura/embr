// stock library for embr
//
// can be used two ways:
//   1. linked directly: #include "embrlib.cpp" and call embr::registerStdlib(interp, runner)
//   2. dynamic plugin:  import "embrlib"  (calls embr_register via dlopen)

#include <embr/embr.h>

using namespace embr;

static Param pAny (std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pNum (std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pStr (std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pArr (std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pMap (std::string n)                      { return Param::req(std::move(n), TS::Map); }
static Param pFn  (std::string n)                      { return Param::req(std::move(n), TS::Fn ); }
//static Param pPtr (std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pOpt (std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }
//static Param pRest(std::string n, TypeSet m = TS::Any) { return Param::rest(std::move(n), m);      }

void registerEmbrLib(Interpreter& interp) {
    // input(prompt?: str) -> str
    interp.bindSig("input", {pOpt("prompt", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        if (!args.empty()) std::cout << args[0].asString() << std::flush;
        std::string line;
        std::getline(std::cin, line);
        return Value(line);
    });


    // assert(cond, msg?: str)
    interp.bindSig("assert", {pAny("cond"), pOpt("msg", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        if (!args[0].truthy()) {
            std::string msg = args.size() > 1 ? args[1].asString() : "assertion failed";
            raiseError("assert", msg);
        }
        return Value(0.0);
    });

    // error(msg: str) -> never returns normally
    // raises a script error carrying msg
    // catchable from script with try ... catch e ... end
    // the return type is never actually used
    // it exists so error() type-checks as an ordinary callable expression
    interp.bindSig("error", {pStr("msg")},
    [](const std::vector<Value>& args) -> Value {
        raiseError("error", args[0].asString());
        return Value(0.0);
    });



    // type(val) -> str
    interp.bindSig("type", {pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        return Value(args[0].typeName());
    });

    // num(val: str|num) -> num
    interp.bindSig("num", {Param::req("val", TS::Num|TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isNumeric()) return args[0];
        const auto& s = args[0].asString();
        double d;
        auto [ptr, ec] = parse_double(s.data(), s.data() + s.size(), d);
        if (ec != std::errc()) raiseError("num", "cannot convert to number: " + s);
        return Value(d);
    });

    // str(val: any) -> str
    interp.bindSig("str", {Param::req("val", TS::Any)},
    [](const std::vector<Value>& args) -> Value {
        return args[0].formatAsString();
    });


    // immmut container operations

    // len(val: str|arr|map) -> num
    interp.bindSig("len", {Param::req("val", TS::Any)},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isString()) return Value((double)args[0].asString().size());
        if (args[0].isArray())  return Value((double)args[0].asArray().size());
        return Value((double)args[0].asMap().size());
    });

    // push(arr: arr, val) -> arr
    interp.bindSig("push", {pArr("arr"), pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        auto arr = args[0].asArray();
        arr.push_back(args[1]);
        return Value(std::move(arr));
    });

    // pop(arr: arr) -> arr
    interp.bindSig("pop", {pArr("arr")},
    [](const std::vector<Value>& args) -> Value {
        auto arr = args[0].asArray();
        if (arr.empty()) raiseError("pop", "cannot pop empty array");
        arr.pop_back();
        return Value(std::move(arr));
    });

    // keys(m: map) -> arr
    interp.bindSig("keys", {pMap("m")},
    [](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        for (const auto& [k, _] : args[0].asMap()) out.push_back(Value(k));
        return Value(std::move(out));
    });

    // values(m: map) -> arr
    interp.bindSig("values", {pMap("m")},
    [](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        for (const auto& [_, v] : args[0].asMap()) out.push_back(v);
        return Value(std::move(out));
    });

    // has(container: arr|map, key) -> num
    interp.bindSig("has", {Param::req("container", TS::Arr|TS::Map), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isMap()) {
            if (!args[1].isString()) raiseError("has", "map key must be string");
            return Value(args[0].asMap().count(args[1].asString()) ? 1.0 : 0.0);
        }
        // array: check index in range
        int ii = (int)args[1].asNumber();
        return Value(ii >= 0 && ii < (int)args[0].asArray().size() ? 1.0 : 0.0);
    });

    // slice(container: arr|str, start: num, end?: num) -> arr
    interp.bindSig("slice", {Param::req("container", TS::Arr|TS::Str|TS::Map), pNum("start"), pOpt("end", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isArray()) {
            const auto& arr = args[0].asArray();
            int start = (int)args[1].asNumber();
            int end   = args.size() >= 3 ? (int)args[2].asNumber() : (int)arr.size();
            start = std::max(0,     std::min(start, (int)arr.size()));
            end   = std::max(start, std::min(end,   (int)arr.size()));
            return Value(Value::array_type(arr.begin() + start, arr.begin() + end));
        } else if (args[0].isMap()) {
            // maps have no defined order, so this slices whatever order the map currently iterates in
            // stable for a given map instance, not guaranteed across runs/rebuilds.
            const auto& m = args[0].asMap();
            std::vector<std::string> keys;
            keys.reserve(m.size());
            for (const auto& [k, v] : m) keys.push_back(k);
            int start = (int)args[1].asNumber();
            int end   = args.size() >= 3 ? (int)args[2].asNumber() : (int)keys.size();
            start = std::max(0,     std::min(start, (int)keys.size()));
            end   = std::max(start, std::min(end,   (int)keys.size()));
            Value::map_type out;
            for (int i = start; i < end; ++i) out[keys[i]] = m.at(keys[i]);
            return Value(std::move(out));
        } else {
            const auto& s = args[0].asString();
            int start = (int)args[1].asNumber();
            int end   = args.size() >= 3 ? (int)args[2].asNumber() : (int)s.size();
            start = std::max(0,     std::min(start, (int)s.size()));
            end   = std::max(start, std::min(end,   (int)s.size()));
            return Value(s.substr((size_t)start, (size_t)std::max(0, end)));
        }
    });

    // join(arr: arr, sep?: str) -> str
    interp.bindSig("join", {pArr("arr"), pOpt("sep", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        std::string sep = args.size() > 1 ? args[1].asString() : "";
        std::string out;
        const auto& arr = args[0].asArray();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) out += sep;
            out += arr[i].asString();
        }
        return Value(out);
    });



    // globals() -> map 
    interp.bindSig("globals", {},
    [&interp](const std::vector<Value>&) -> Value {
        Value::map_type m;
        for (const auto& [k, v] : interp.globals())
            m[k] = v;
        return Value(std::move(m));
    });

    // call(fn: fn, args?: arr) -> any
    interp.bindSig("call", {pFn("fn"), pOpt("args", TS::Arr)},
    [&interp](const std::vector<Value>& args) -> Value {
        std::vector<Value> fargs;
        if (args.size() >= 2) fargs = args[1].asArray();
        return invoke(interp, args[0], fargs);
    });

    // fn_name(fn: fn) -> str display name includes file:line for script fns
    interp.bindSig("fn_name", {pFn("fn")},
    [](const std::vector<Value>& args) -> Value {
        const auto& c = args[0].asCallable();
        if (c.isScript() && !c.script.sourceFile.empty() && c.script.definedAt.valid()) {
            return Value(c.script.name + "@" + c.script.sourceFile +
                         ":" + std::to_string(c.script.definedAt.startLine));
        }
        return Value(c.name());
    });

    // fn_arity(fn: fn) -> num declared param count -1 for native (open arity)
    interp.bindSig("fn_arity", {pFn("fn")},
    [](const std::vector<Value>& args) -> Value {
        const auto& c = args[0].asCallable();
        if (c.isNative() && c.sig.empty()) return Value(-1.0);
        if (c.isNative()) {
            // count non-variadic required params as the effective arity
            size_t n = 0;
            for (const auto& p : c.sig) { if (!p.optional && !p.variadic) ++n; }
            return Value((double)n);
        }
        return Value((double)c.script.params.size());
    });

    // fn_sig(fn: fn) -> str person-readable signature string
    interp.bindSig("fn_sig", {pFn("fn")},
    [](const std::vector<Value>& args) -> Value {
        const auto& c = args[0].asCallable();
        std::string s = c.name() + "(";
        const auto& params = c.isNative() ? c.sig : c.script.params;
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) s += ", ";
            const auto& p = params[i];
            if (p.variadic) s += "...";
            s += p.name;
            if (!p.type.isAny()) s += ": " + p.type.name();
            if (p.optional && !p.variadic) s += "?";
        }
        s += ")";
        if (c.isScript() && !c.script.returnType.isAny())
            s += " -> " + c.script.returnType.name();
        return Value(s);
    });

    // is_native(fn: fn) -> num
    interp.bindSig("is_native", {pFn("fn")},
    [](const std::vector<Value>& args) -> Value {
        return Value(args[0].asCallable().isNative() ? 1.0 : 0.0);
    });



    // if_do(cond, then_fn: fn, else_fn?: fn) -> any
    // inline if calls then_fn() or else_fn() based on truthiness of cond
    interp.bindSig("if_do", {pAny("cond"), pFn("then_fn"), pOpt("else_fn", TypeSet(TS::Fn))},
    [&interp](const std::vector<Value>& args) -> Value {
        if (args[0].truthy()) return invoke(interp, args[1], {});
        if (args.size() >= 3) return invoke(interp, args[2], {});
        return Value(0.0);
    });

    // while_do(cond_fn: fn, body_fn: fn) -> 0
    // calls cond_fn() before each iteration and body_fn() while truthy
    interp.bindSig("while_do", {pFn("cond_fn"), pFn("body_fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        while (invoke(interp, args[0], {}).truthy()) invoke(interp, args[1], {});
        return Value(0.0);
    });

    // do(fn: fn) -> any
    interp.bindSig("do", {pFn("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        return invoke(interp, args[0], {});
    });



    // for_each_do(container: arr|map|str, fn: fn) -> arr|map
    interp.bindSig("for_each_do", {Param::req("container", TS::Arr|TS::Map|TS::Str), pFn("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        if (TS::Arr.contains(args[0].tag())) {
            Value::array_type out;
            for (const auto& el : args[0].asArray())
                out.push_back(invoke(interp, args[1], {el}));
            return Value(std::move(out));
        } else if (TS::Map.contains(args[0].tag())) {
            Value::map_type out;
            for (const auto& [k, v] : args[0].asMap())
                out[k] = invoke(interp, args[1], {k, v});
            return Value(std::move(out));
        } else {
            Value::array_type out;
            for (const auto& el : args[0].asString())
                out.push_back(invoke(interp, args[1], {Value(std::string(1, el))}));
            return Value(std::move(out));
        }
    });

    // filter(arr: arr, pred: fn) -> arr
    interp.bindSig("filter", {pArr("arr"), pFn("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) out.push_back(el);
        return Value(std::move(out));
    });

    // reduce(arr: arr, fn: fn, init) -> any
    interp.bindSig("reduce", {pArr("arr"), pFn("fn"), pAny("init")},
    [&interp](const std::vector<Value>& args) -> Value {
        Value acc = args[2];
        for (const auto& el : args[0].asArray())
            acc = invoke(interp, args[1], {acc, el});
        return acc;
    });

    // sort(arr: arr, descending?) -> arr
    // sorts a flat numeric array via std::sort
    // (0 = ascend, 1 = descend)
    // raises an error if any element is not a number.
    interp.bindSig("sort", {pArr("arr"), pOpt("descending")},
    [](const std::vector<Value>& args) -> Value {
        auto arr = args[0].asArray();
        bool desc = args.size() >= 2 && args[1].truthy();

        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].isNumeric())
                raiseError("sort_",
                    "element at index " + std::to_string(i) +
                    " is not a number (got " + arr[i].typeName() + "). "
                    "use sort_do with a weight function for non-numeric arrays");
        }

        std::sort(arr.begin(), arr.end(),
            [desc](const Value& a, const Value& b) {
                return desc ? a.asNumber() > b.asNumber()
                            : a.asNumber() < b.asNumber();
            });

        return Value(std::move(arr));
    });

    // sort_do(arr: arr, weight: fn, descending?) -> arr
    // sorts an array of any elements by a caller-supplied weight function
    // calls weight(elem) per elem, must return num (0 = ascending, 1 = descending)
    //
    //   users = [{"name":"Alex","followers":123}, ...]
    //   sort_do(users, fn(u) return u["followers"] end, 0)
    interp.bindSig("sort_do", {pArr("arr"), pFn("weight"), pOpt("descending")},
    [&interp](const std::vector<Value>& args) -> Value {
        const auto& arr = args[0].asArray();
        bool desc = args.size() >= 3 && args[2].truthy();

        // pre-compute weights. one interpreter call per element, not per comparison
        std::vector<std::pair<double, Value>> weighted;
        weighted.reserve(arr.size());

        for (size_t i = 0; i < arr.size(); ++i) {
            Value w = invoke(interp, args[1], {arr[i]});
            if (!w.isNumeric())
                raiseError("sort_do",
                    "weight function returned " + w.typeName() +
                    " for element at index " + std::to_string(i) +
                    " weight must return a number");
            weighted.emplace_back(w.asNumber(), arr[i]);
        }

        std::sort(weighted.begin(), weighted.end(),
            [desc](const std::pair<double, Value>& a,
                   const std::pair<double, Value>& b) {
                return desc ? a.first > b.first
                            : a.first < b.first;
            });

        Value::array_type out;
        out.reserve(weighted.size());
        for (auto& [_, v] : weighted) out.push_back(std::move(v));

        return Value(std::move(out));
    });

    // load_file(path: str) -> str
    //
    // reads an arbitrary file and returns its entire contents as a string
    // the path is resolved relative to scriptDir, then cwd
    // intended as a lightweight stopgap until a proper IO plugin exists
    //
    //   contents = load_file("data/words.txt")
    //   contents = load_file("/etc/hostname")
    interp.bindSig("load_file", {pStr("path")},
    [&interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;
 
        fs::path p(args[0].asString());
 
        // resolve relative paths against scriptDir first, then cwd
        fs::path resolved;
        if (p.is_absolute()) {
            resolved = p;
        } else {
            if (!interp.scriptDir.empty()) {
                fs::path cand = (fs::path(interp.scriptDir) / p).lexically_normal();
                std::error_code ec;
                if (fs::exists(cand, ec) && !ec) resolved = cand;
            }
            if (resolved.empty()) {
                fs::path cand = (fs::current_path() / p).lexically_normal();
                std::error_code ec;
                if (fs::exists(cand, ec) && !ec) resolved = cand;
            }
            if (resolved.empty())
                resolved = p;  // let ifstream produce the OS error
        }
 
        std::ifstream f(resolved);
        if (!f)
            raiseError("load_file", "cannot open file: " + resolved.string());
 
        std::string contents((std::istreambuf_iterator<char>(f)), {});
        return Value(std::move(contents));
    });


    // write_file(path: str, contents: str) -> 0
    //
    // writes contents to path, overwriting any existing file
    // path resolution matches load_file.
    //
    //   write_file("out.txt", "hello\n")
    interp.bindSig("write_file", {pStr("path"), pStr("contents")},
    [&interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;

        fs::path p(args[0].asString());
        fs::path resolved = p.is_absolute()
            ? p
            : (!interp.scriptDir.empty() ? fs::path(interp.scriptDir) / p : fs::current_path() / p);

        std::ofstream f(resolved, std::ios::out | std::ios::trunc);
        if (!f)
            raiseError("write_file", "cannot open file for writing: " + resolved.string());

        f << args[1].asString();
        return Value(0.0);
    });

    // append_file(path: str, contents: str) -> 0
    //
    // appends contents to path, creating it if it doesn't exist.
    // path resolution matches write_file.
    //
    //   append_file("log.txt", "line\n")
    interp.bindSig("append_file", {pStr("path"), pStr("contents")},
    [&interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;

        fs::path p(args[0].asString());
        fs::path resolved = p.is_absolute()
            ? p
            : (!interp.scriptDir.empty() ? fs::path(interp.scriptDir) / p : fs::current_path() / p);

        std::ofstream f(resolved, std::ios::out | std::ios::app);
        if (!f)
            raiseError("append_file", "cannot open file for appending: " + resolved.string());

        f << args[1].asString();
        return Value(0.0);
    });

    // range(start: num, end: num, step?: num) -> arr
    interp.bindSig("range", {pNum("start"), pNum("end"), pOpt("step", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        // if every argument was given as an int literal, produce int elements instead of silently downgrading to float
        bool asInt = args[0].isInt() && args[1].isInt() && (args.size() < 3 || args[2].isInt());

        double start = args[0].asNumber();
        double end   = args[1].asNumber();
        double step;
        if (args.size() >= 3) {
            step = args[2].asNumber();
            if (step == 0.0) raiseError("range", "step must not be 0");
        } else {
            step = (end < start) ? -1.0 : 1.0;
        }
        if ((step > 0 && start >= end) || (step < 0 && start <= end))
            return Value(Value::array_type{});
        if ((step > 0) != (end > start))
            raiseError("range", "step direction never reaches end");

        Value::array_type out;
        auto emit = [&](double v) { out.push_back(asInt ? Value((int64_t)v) : Value(v)); };
        if (step > 0) for (double v = start; v < end; v += step) emit(v);
        else          for (double v = start; v > end; v += step) emit(v);
        return Value(std::move(out));
    });

    // map_merge(a: map, b: map) -> map
    //
    // shallow-merges two maps; keys in b win on conflict.
    //
    //   map_merge({"a":1}, {"a":2,"b":3})   # {"a":2,"b":3}
    interp.bindSig("map_merge", {pMap("a"), pMap("b")},
    [](const std::vector<Value>& args) -> Value {
        Value::map_type out = args[0].asMap();
        for (const auto& [k, v] : args[1].asMap()) out[k] = v;
        return Value(std::move(out));
    });

    // map_pick(m: map, keys: arr) -> map
    //
    // projects m down to the given keys (keys not present in m are skipped).
    //
    //   map_pick({"a":1,"b":2,"c":3}, ["a","c"])   # {"a":1,"c":3}
    interp.bindSig("map_pick", {pMap("m"), pArr("keys")},
    [](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        Value::map_type out;
        for (const auto& k : args[1].asArray()) {
            if (!k.isString()) raiseError("map_pick", "keys must be strings");
            auto it = m.find(k.asString());
            if (it != m.end()) out[it->first] = it->second;
        }
        return Value(std::move(out));
    });

    // map_omit(m: map, keys: arr) -> map
    //
    // like map_pick but excludes the given keys instead of selecting them.
    //
    //   map_omit({"a":1,"b":2,"c":3}, ["b"])   # {"a":1,"c":3}
    interp.bindSig("map_omit", {pMap("m"), pArr("keys")},
    [](const std::vector<Value>& args) -> Value {
        Value::map_type out = args[0].asMap();
        for (const auto& k : args[1].asArray()) {
            if (!k.isString()) raiseError("map_omit", "keys must be strings");
            out.erase(k.asString());
        }
        return Value(std::move(out));
    });

    // load_module(path: str, reload?: num) -> 0
    //
    // loads and executes an embr script module, exporting its non-local bindings into the current scope
    // subsequent calls with the same resolved path are no-ops unless reload is truthy
    //
    //   load_module("utils")        # loads utils.embr
    //   load_module("lib/math")     # loads lib/math.embr relative to caller
    //   load_module("/abs/path")    # absolute
    //   load_module("utils", 1)     # force reload
    //
    // only available when the tree-walker backend is compiled in
    // it needs Runner::importEmbrModule, which the VM backend doesn't have an equivalent for yet
#ifdef EMBR_WITH_TREE_WALKER
    interp.bindSig("load_module",
        {pStr("path"), pOpt("reload")},
    [&interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;

        const std::string& rawPath = args[0].asString();
        bool forceReload = args.size() >= 2 && args[1].truthy();

        // if forcing a reload, remove the canonical path from the loaded set
        // so execImportEmbrModule will run it again
        if (forceReload) {
            fs::path p(rawPath);

            auto withEmbr = [](fs::path base) -> fs::path {
                if (base.extension() == ".embr") return base;
                return fs::path(base.string() + ".embr");
            };

            std::vector<fs::path> cands;
            if (p.is_absolute()) {
                cands.push_back(withEmbr(p));
            } else if (p.has_parent_path()) {
                if (!interp.scriptDir.empty())
                    cands.push_back((fs::path(interp.scriptDir) / withEmbr(p)).lexically_normal());
                cands.push_back((fs::current_path() / withEmbr(p)).lexically_normal());
            } else {
                fs::path name = withEmbr(p);
                if (!interp.scriptDir.empty()) {
                    cands.push_back(fs::path(interp.scriptDir) / name);
                    cands.push_back(fs::path(interp.scriptDir) / "modules" / name);
                }
                cands.push_back(fs::current_path() / name);
                cands.push_back(fs::current_path() / "modules" / name);
            }
            for (const auto& c : cands) {
                std::error_code ec;
                fs::path canon = fs::canonical(c, ec);
                if (!ec) interp.loadedModules_.erase(canon.string());
            }
        }

        // delegate to the interpreter's import mechanism
        Runner r(interp);
        r.importEmbrModule(rawPath, {});
        return Value(0.0);
    });
#endif // EMBR_WITH_TREE_WALKER

} // registerEmbrlib

EMBR_PLUGIN {
    registerEmbrLib(*interp);
}
