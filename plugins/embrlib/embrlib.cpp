// embrlib.cpp
// the stock library for embr: containers, functional helpers, control flow as functions, file io, module loading
//
// usage
//   import "embrlib"
//   print(len([1, 2, 3]))            # 3
//   print(sort([3, 1, 2]))           # [1, 2, 3]
//   print(join(["a", "b"], "-"))     # a-b
//
// can be used two ways:
//   1. linked directly: #include "embrlib.cpp" and call embr::registerStdlib(interp, runner)
//   2. dynamic plugin:  import "embrlib"  (calls embr_register via dlopen)

#include <embr/embr.h>
#include "../vec/vec.h"
#include <climits>

using namespace embr;

static Param pAny (std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pNum (std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pStr (std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pArr (std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pMap (std::string n)                      { return Param::req(std::move(n), TS::Map); }
static Param pOpt (std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }
static Param pRest(std::string n, TypeSet m = TS::Any) { return Param::rest(std::move(n), m);      }

// like pFn, but also accepts a typed pointer. for a parameter that is just handed to embr::invoke() (which resolves
// a pointer's __<tag>_call dunder itself, see resolveDunder() in core/registry.h), widening the signature is all it
// needs. invoke() raises its own clear error for a pointer with no matching dunder
static Param pFnOrPtr(std::string n) { return Param::req(std::move(n), TS::Fn|TS::Ptr); }

// resolves v to itself if it's already Callable, or to the Callable behind its __<tag>_call dunder if v is a typed
// pointer that opts into being called. shared by fn_arity/fn_name/fn_sig/is_native so all four treat anything callable
// the same way. sets *viaDunder so fn_arity can drop the dunder's own leading self parameter from the arity
// (a script calling `p(x, y)` never writes that argument)
static Value resolveIntrospectable(Interpreter& interp, const std::string& fn,
                                   const Value& v, bool& viaDunder) {
    viaDunder = false;
    if (v.isCallable()) return v;
    Value dfn;
    if (v.isPointer() && resolveDunder(interp, v, "call", dfn)) {
        viaDunder = true;
        return dfn;
    }
    raiseError(fn, "value is not callable and has no __<tag>_call (got " + v.typeName() +
               (v.isPointer() ? " tag '" + v.asPointer().type + "'" : "") + ")");
}

// stable merge sort driven by a "does a sort strictly before b" predicate. std::sort is undefined behavior (it can
// read out of bounds) when its comparator isn't a strict weak ordering, and every comparator here is script-controlled
// (a user cmp fn, a __<tag>_lt dunder, NaN weights). so anything sorting on script-supplied order goes through this:
// an inconsistent comparator just gives some order, never a crash. stability keeps equal elements in input order on both backends
template <typename T, typename Less>
static void stableSort(std::vector<T>& v, Less less) {
    if (v.size() < 2) return;
    std::vector<T> tmp(v.size());
    for (size_t width = 1; width < v.size(); width *= 2) {
        for (size_t lo = 0; lo < v.size(); lo += 2 * width) {
            size_t mid = std::min(lo + width, v.size());
            size_t hi  = std::min(lo + 2 * width, v.size());
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = less(v[j], v[i]) ? std::move(v[j++]) : std::move(v[i++]);
            while (i < mid) tmp[k++] = std::move(v[i++]);
            while (j < hi)  tmp[k++] = std::move(v[j++]);
        }
        v.swap(tmp);
    }
}

// embrlib's file natives reach the real filesystem; a restricted sandbox (Interpreter::pluginPolicy.fileIo)
// turns them into errors instead of silently letting a "pure" plugin read or write anything
static void requireFileIo(const Interpreter& interp, const char* fn) {
    if (!interp.pluginPolicy.fileIo)
        raiseError(fn, "file access is disabled in this sandbox");
}

// script number -> int index without the UB of a raw (int) cast on NaN/Inf/huge
static int idxArg(const Value& v) {
    double d = v.asNumber();
    if (!(d > -2147483648.0)) return INT_MIN;   // also catches NaN
    if (d >= 2147483647.0)    return INT_MAX;
    return (int)d;
}

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
    //
    // a typed pointer whose tag has a __<tag>_type global gets that called instead of the generic "pointer" name, so a
    // plugin can report a more specific type for its own values (like str()/len()/has() use __<tag>_str/__<tag>_len/
    // __<tag>_has). everything else uses the ordinary typeName(). like str(), type() never has to raise
    interp.bindSig("type", {pAny("val")},
    [&interp](const std::vector<Value>& args) -> Value {
        if (args[0].isPointer()) {
            Value dfn;
            if (resolveDunder(interp, args[0], "type", dfn))
                return invoke(interp, dfn, {args[0]});
        }
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
    //
    // a typed pointer whose tag has a __<tag>_str global gets that called instead of the generic <TAG 0xADDR> form, so a
    // plugin can give its values a meaningful string (a table's contents, say), like Python's __str__. everything else
    // uses formatAsString(). unlike len()/has() below, str() never has to raise
    interp.bindSig("str", {Param::req("val", TS::Any)},
    [&interp](const std::vector<Value>& args) -> Value {
        if (args[0].isPointer()) {
            Value dfn;
            if (resolveDunder(interp, args[0], "str", dfn))
                return invoke(interp, dfn, {args[0]});
        }
        return args[0].formatAsString();
    });


    // immmut container operations

    // len(val: str|arr|map, or a typed pointer with a __<tag>_len) -> num
    interp.bindSig("len", {Param::req("val", TS::Any)},
    [&interp](const std::vector<Value>& args) -> Value {
        if (args[0].isString()) return Value((double)args[0].asString().size());
        if (args[0].isArray())  return Value((double)args[0].asArray().size());
        if (args[0].isMap())    return Value((double)args[0].asMap().size());
        if (args[0].isPointer()) {
            Value dfn;
            if (resolveDunder(interp, args[0], "len", dfn))
                return invoke(interp, dfn, {args[0]});
        }
        raiseError("len", "no __<tag>_len defined and val is not a str/arr/map (got " +
                   args[0].typeName() +
                   (args[0].isPointer() ? " tag '" + args[0].asPointer().type + "'" : "") + ")");
    });

    // push(arr: arr|vec, val) -> arr. on a vec this appends in place and returns the same vec, so `v = push(v, x)` is O(1)
    interp.bindSig("push", {Param::req("arr", TS::Arr|TS::Ptr), pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isPointer()) {
            embrvec::Vec* v = embrvec::vecOf(args[0]);
            if (!v) raiseError("push", "expected an array or a vec, got ptr with tag '" + args[0].asPointer().type + "'");
            checkAllocSize("push", "vec length", (int64_t)v->items.size() + 1);
            v->items.push_back(args[1]);
            return args[0];
        }
        auto arr = args[0].asArray();
        arr.push_back(args[1]);
        return Value(std::move(arr));
    });

    // pop(arr: arr|vec) -> arr, without the last element. on a vec it removes it in place and returns the same vec
    // (vec_pop gives you the element instead)
    interp.bindSig("pop", {Param::req("arr", TS::Arr|TS::Ptr)},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isPointer()) {
            embrvec::Vec* v = embrvec::vecOf(args[0]);
            if (!v) raiseError("pop", "expected an array or a vec, got ptr with tag '" + args[0].asPointer().type + "'");
            if (v->items.empty()) raiseError("pop", "cannot pop empty array");
            v->items.pop_back();
            return args[0];
        }
        auto arr = args[0].asArray();
        if (arr.empty()) raiseError("pop", "cannot pop empty array");
        arr.pop_back();
        return Value(std::move(arr));
    });

    // the in-place natives below change the variable they are given, so they take it as &name (see "in-out parameters" on the
    // wiki architecture page). push and pop above still return a new array and leave the original alone

    // append(&arr, val) -> the new length
    interp.bindSig("append", {Param::io("arr", TS::Arr), pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        Value& a = inoutTarget(args[0], args[1].nestDepth);
        a.arrayAppend(args[1]);
        return Value((double)a.asArray().size());
    });

    // insert(&arr, pos, val) -> the new length. pos can be the length, which appends
    interp.bindSig("insert", {Param::io("arr", TS::Arr), pNum("pos"), pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        Value& a = inoutTarget(args[0], args[2].nestDepth);
        int pos = idxArg(args[1]);
        if (pos < 0 || pos > (int)a.asArray().size())
            raiseError("insert", "position " + std::to_string(pos) + " is out of bounds for an array of length " +
                       std::to_string(a.asArray().size()));
        a.arrayInsert((size_t)pos, args[2]);
        return Value((double)a.asArray().size());
    });

    // remove_at(&arr, pos) -> the element that was there
    interp.bindSig("remove_at", {Param::io("arr", TS::Arr), pNum("pos")},
    [](const std::vector<Value>& args) -> Value {
        Value& a = inoutTarget(args[0]);
        int pos = idxArg(args[1]);
        if (pos < 0 || pos >= (int)a.asArray().size())
            raiseError("remove_at", "position " + std::to_string(pos) + " is out of bounds for an array of length " +
                       std::to_string(a.asArray().size()));
        return a.arrayRemoveAt((size_t)pos);
    });

    // remove_last(&arr) -> the element that was last
    interp.bindSig("remove_last", {Param::io("arr", TS::Arr)},
    [](const std::vector<Value>& args) -> Value {
        Value& a = inoutTarget(args[0]);
        if (a.asArray().empty()) raiseError("remove_last", "cannot remove from an empty array");
        return a.arrayRemoveAt(a.asArray().size() - 1);
    });

    // remove_key(&m, key) -> 1 if the key was there and is gone now, 0 if it was never there
    interp.bindSig("remove_key", {Param::io("m", TS::Map), pStr("key")},
    [](const std::vector<Value>& args) -> Value {
        Value& m = inoutTarget(args[0]);
        return Value(m.mapRemove(args[1].asString()));
    });

    // keys(m: map) -> arr (alphabetically sorted, so iteration order is deterministic
    // across runs, the underlying map_type is an unordered_map)
    interp.bindSig("keys", {pMap("m")},
    [](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        for (const auto& k : sortedMapKeys(args[0].asMap())) out.push_back(Value(k));
        return Value(std::move(out));
    });

    // values(m: map) -> arr (ordered to match keys()'s alphabetical order)
    interp.bindSig("values", {pMap("m")},
    [](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        Value::array_type out;
        for (const auto& k : sortedMapKeys(m)) out.push_back(m.at(k));
        return Value(std::move(out));
    });

    // has(container: arr|map, key) -> num
    // a typed pointer with a __<tag>_has global is dispatched to as
    // __<tag>_has(container, key) instead of raising.
    interp.bindSig("has", {Param::req("container", TS::Arr|TS::Map|TS::Ptr), pAny("key")},
    [&interp](const std::vector<Value>& args) -> Value {
        if (args[0].isMap()) {
            if (!args[1].isString()) raiseError("has", "map key must be string");
            return Value(args[0].asMap().count(args[1].asString()) ? 1.0 : 0.0);
        }
        if (args[0].isPointer()) {
            Value dfn;
            if (resolveDunder(interp, args[0], "has", dfn))
                return invoke(interp, dfn, {args[0], args[1]});
            raiseError("has", "no __<tag>_has defined for tag '" + args[0].asPointer().type + "'");
        }
        // array: check index in range
        int64_t ii = numToInt64(args[1], "has", "index");
        return Value(ii >= 0 && ii < (int64_t)args[0].asArray().size() ? 1.0 : 0.0);
    });

    // slice(container: arr|str, start: num, end?: num) -> arr
    interp.bindSig("slice", {Param::req("container", TS::Arr|TS::Str|TS::Map), pNum("start"), pOpt("end", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isArray()) {
            const auto& arr = args[0].asArray();
            int start = idxArg(args[1]);
            int end   = args.size() >= 3 ? idxArg(args[2]) : (int)arr.size();
            start = std::max(0,     std::min(start, (int)arr.size()));
            end   = std::max(start, std::min(end,   (int)arr.size()));
            return Value(Value::array_type(arr.begin() + start, arr.begin() + end));
        } else if (args[0].isMap()) {
            // slices by alphabetical key order (see sortedMapKeys), matching keys()/values()
            const auto& m = args[0].asMap();
            std::vector<std::string> keys = sortedMapKeys(m);
            int start = idxArg(args[1]);
            int end   = args.size() >= 3 ? idxArg(args[2]) : (int)keys.size();
            start = std::max(0,     std::min(start, (int)keys.size()));
            end   = std::max(start, std::min(end,   (int)keys.size()));
            Value::map_type out;
            for (int i = start; i < end; ++i) out[keys[i]] = m.at(keys[i]);
            return Value(std::move(out));
        } else {
            const auto& s = args[0].asString();
            int start = idxArg(args[1]);
            int end   = args.size() >= 3 ? idxArg(args[2]) : (int)s.size();
            start = std::max(0,     std::min(start, (int)s.size()));
            end   = std::max(start, std::min(end,   (int)s.size()));
            return Value(s.substr((size_t)start, (size_t)(end - start)));
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
    // (the returned map is unordered, so printing it in a stable order is formatAsString's job via sortedMapKeys)
    interp.bindSig("globals", {},
    [&interp](const std::vector<Value>&) -> Value {
        Value::map_type m;
        for (const auto& [k, v] : interp.globals())
            m[k] = v;
        return Value(std::move(m));
    });

    // call(fn: fn|ptr, args?: arr) -> any
    // fn may be a typed pointer with a __<tag>_call dunder instead of a real Callable. invoke() resolves that
    // (see resolveDunder() in core/registry.h), so this only has to accept one
    interp.bindSig("call", {pFnOrPtr("fn"), pOpt("args", TS::Arr)},
    [&interp](const std::vector<Value>& args) -> Value {
        std::vector<Value> fargs;
        if (args.size() >= 2) fargs = args[1].asArray();
        return invoke(interp, args[0], fargs);
    });

    // fn_name(fn: fn|ptr) -> str display name includes file:line for script fns
    // fn may also be a typed pointer with a __<tag>_call dunder, reports
    // the dunder function's own name (see resolveIntrospectable()).
    interp.bindSig("fn_name", {pFnOrPtr("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        bool viaDunder;
        Value target = resolveIntrospectable(interp, "fn_name", args[0], viaDunder);
        const auto& c = target.asCallable();
        if (c.isScript() && !c.script.sourceFile.empty() && c.script.definedAt.valid()) {
            return Value(c.script.name + "@" + c.script.sourceFile +
                         ":" + std::to_string(c.script.definedAt.startLine));
        }
        return Value(c.name());
    });

    // fn_arity(fn: fn|ptr) -> num declared param count, -1 for native (open arity)
    // for a dunder-callable pointer it reports the __<tag>_call dunder's arity minus 1, because the leading self
    // parameter is added by invoke() and a script calling p(...) never counts it
    interp.bindSig("fn_arity", {pFnOrPtr("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        bool viaDunder;
        Value target = resolveIntrospectable(interp, "fn_arity", args[0], viaDunder);
        const auto& c = target.asCallable();
        long n;
        if (c.isNative() && c.sig.empty()) {
            n = -1;
        } else if (c.isNative()) {
            // count non-variadic required params as the effective arity
            size_t cnt = 0;
            for (const auto& p : c.sig) { if (!p.optional && !p.variadic) ++cnt; }
            n = (long)cnt;
        } else {
            n = (long)c.script.params.size();
        }
        if (viaDunder && n > 0) --n;
        return Value((double)n);
    });

    // fn_sig(fn: fn|ptr) -> str person-readable signature string
    // for a dunder-callable pointer it describes the __<tag>_call dunder itself, including its leading self parameter
    // (unlike fn_arity, this is a literal description of the underlying function)
    interp.bindSig("fn_sig", {pFnOrPtr("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        bool viaDunder;
        Value target = resolveIntrospectable(interp, "fn_sig", args[0], viaDunder);
        const auto& c = target.asCallable();
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

    // is_native(fn: fn|ptr) -> num
    interp.bindSig("is_native", {pFnOrPtr("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        bool viaDunder;
        return Value(resolveIntrospectable(interp, "is_native", args[0], viaDunder).asCallable().isNative() ? 1.0 : 0.0);
    });



    // if_do(cond, then_fn: fn|ptr, else_fn?: fn|ptr) -> any
    // inline if: calls then_fn() or else_fn() based on whether cond is truthy
    // then_fn/else_fn may be a typed pointer with a __<tag>_call dunder, invoke() resolves it
    interp.bindSig("if_do", {pAny("cond"), pFnOrPtr("then_fn"), pOpt("else_fn", TS::Fn|TS::Ptr)},
    [&interp](const std::vector<Value>& args) -> Value {
        if (args[0].truthy()) return invoke(interp, args[1], {});
        if (args.size() >= 3) return invoke(interp, args[2], {});
        return Value(0.0);
    });

    // while_do(cond_fn: fn|ptr, body_fn: fn|ptr) -> 0
    // calls cond_fn() before each iteration and body_fn() while truthy
    interp.bindSig("while_do", {pFnOrPtr("cond_fn"), pFnOrPtr("body_fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        while (invoke(interp, args[0], {}).truthy()) invoke(interp, args[1], {});
        return Value(0.0);
    });

    // do(fn: fn|ptr) -> any
    interp.bindSig("do", {pFnOrPtr("fn")},
    [&interp](const std::vector<Value>& args) -> Value {
        return invoke(interp, args[0], {});
    });



    // for_each_do(container: arr|map|str, fn: fn|ptr) -> arr|map
    interp.bindSig("for_each_do", {Param::req("container", TS::Arr|TS::Map|TS::Str), pFnOrPtr("fn")},
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

    // filter(arr: arr, pred: fn|ptr) -> arr
    interp.bindSig("filter", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) out.push_back(el);
        return Value(std::move(out));
    });

    // reduce(arr: arr, fn: fn|ptr, init) -> any
    interp.bindSig("reduce", {pArr("arr"), pFnOrPtr("fn"), pAny("init")},
    [&interp](const std::vector<Value>& args) -> Value {
        Value acc = args[2];
        for (const auto& el : args[0].asArray())
            acc = invoke(interp, args[1], {acc, el});
        return acc;
    });

    // sort(arr: arr, descending?) -> arr
    // sorts a flat numeric array via std::sort (0 = ascend, 1 = descend)
    // if every element is a typed pointer with one shared tag that has a __<tag>_lt dunder, that dunder is the comparator
    // (sorted ascending, then reversed for descending, so no __<tag>_gt is needed)
    // raises an error if neither applies (mixed types, or no comparator)
    interp.bindSig("sort", {pArr("arr"), pOpt("descending")},
    [&interp](const std::vector<Value>& args) -> Value {
        auto arr = args[0].asArray();
        bool desc = args.size() >= 2 && args[1].truthy();

        bool allNumeric = true;
        for (const auto& el : arr) {
            if (!el.isNumeric()) { allNumeric = false; break; }
        }

        if (allNumeric) {
            std::sort(arr.begin(), arr.end(),
                [desc](const Value& a, const Value& b) {
                    return desc ? a.asNumber() > b.asNumber()
                                : a.asNumber() < b.asNumber();
                });
            return Value(std::move(arr));
        }

        bool allTaggedPointers = !arr.empty();
        std::string tag;
        for (const auto& el : arr) {
            if (!el.isPointer() || el.asPointer().type.empty()) {
                allTaggedPointers = false;
                break;
            }
            if (tag.empty()) tag = el.asPointer().type;
            else if (el.asPointer().type != tag) { allTaggedPointers = false; break; }
        }

        Value ltFn;
        if (allTaggedPointers && resolveDunder(interp, arr[0], "lt", ltFn)) {
            stableSort(arr,
                [&interp, &ltFn](const Value& a, const Value& b) {
                    return invoke(interp, ltFn, {a, b}).truthy();
                });
            if (desc) std::reverse(arr.begin(), arr.end());
            return Value(std::move(arr));
        }

        raiseError("sort_",
            "array elements are not all numbers, and not all typed pointers "
            "sharing a tag with a __tag_lt dunder. "
            "use sort_do with a weight function for other arrays");
    });

    // sort_do(arr: arr, weight: fn|ptr, descending?) -> arr
    // sorts an array of any elements by a caller-supplied weight function
    // calls weight(elem) per elem, must return num (0 = ascending, 1 = descending)
    //
    //   users = [{"name":"Alex","followers":123}, ...]
    //   sort_do(users, fn(u) return u["followers"] end, 0)
    interp.bindSig("sort_do", {pArr("arr"), pFnOrPtr("weight"), pOpt("descending")},
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

        // NaN weights would break std::sort's ordering contract; stableSort
        // tolerates them (they just compare as neither less nor greater)
        stableSort(weighted,
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

    // sort_cmp(arr: arr, cmp: fn|ptr, descending?) -> arr
    // stable sort driven by a comparator: cmp(a, b) returns a number, negative
    // if a sorts before b, positive if after, 0 if equivalent. use it when a
    // single numeric weight (sort_do) can't express the order, e.g. by one key
    // then another. an inconsistent comparator gives an unspecified order but
    // never crashes.
    //
    //   sort_cmp(words, fn(a, b) return len(a) - len(b) end)
    interp.bindSig("sort_cmp", {pArr("arr"), pFnOrPtr("cmp"), pOpt("descending")},
    [&interp](const std::vector<Value>& args) -> Value {
        Value::array_type arr = args[0].asArray();
        bool desc = args.size() >= 3 && args[2].truthy();
        stableSort(arr, [&](const Value& a, const Value& b) {
            Value r = invoke(interp, args[1], desc ? std::vector<Value>{b, a}
                                                   : std::vector<Value>{a, b});
            if (!r.isNumeric())
                raiseError("sort_cmp", "comparator returned " + r.typeName() +
                           ", expected a number");
            return r.asNumber() < 0;
        });
        return Value(std::move(arr));
    });

    // zip(a: arr, b: arr, ...more: arr) -> arr of arr
    // pairs up elements by index; stops at the shortest input
    //
    //   zip([1,2,3], ["a","b"])   # [[1,"a"],[2,"b"]]
    interp.bindSig("zip", {pArr("a"), pArr("b"), pRest("more", TS::Arr)},
    [](const std::vector<Value>& args) -> Value {
        size_t n = args[0].asArray().size();
        for (const auto& a : args) n = std::min(n, a.asArray().size());
        Value::array_type out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            Value::array_type row;
            row.reserve(args.size());
            for (const auto& a : args) row.push_back(a.asArray()[i]);
            out.push_back(Value(std::move(row)));
        }
        return Value(std::move(out));
    });

    // enumerate(arr: arr, start?: num) -> arr of [index, element]
    //
    //   enumerate(["a","b"])      # [[0,"a"],[1,"b"]]
    //   enumerate(["a","b"], 1)   # [[1,"a"],[2,"b"]]
    interp.bindSig("enumerate", {pArr("arr"), pOpt("start", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        int64_t start = 0;
        if (args.size() >= 2) {
            if (!args[1].isInt()) raiseError("enumerate", "start must be an integer");
            start = args[1].asInt();
        }
        Value::array_type out;
        const auto& arr = args[0].asArray();
        out.reserve(arr.size());
        for (size_t i = 0; i < arr.size(); ++i)
            out.push_back(Value(Value::array_type{Value(start + (int64_t)i), arr[i]}));
        return Value(std::move(out));
    });

    // concat(a: arr|str, b: arr|str, ...more) -> arr|str
    // joins arrays into a new array, or strings into a new string; every
    // argument must be the same kind as the first
    //
    //   concat([1,2], [3], [4,5])   # [1,2,3,4,5]
    //   concat("ab", "cd")          # "abcd"
    interp.bindSig("concat",
        {Param::req("a", TS::Arr|TS::Str), Param::req("b", TS::Arr|TS::Str),
         pRest("more", TS::Arr|TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        bool str = args[0].isString();
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i].isString() != str)
                raiseError("concat", "cannot mix " + args[0].typeName() + " and " +
                           args[i].typeName() + " (argument " + std::to_string(i) + ")");
        if (str) {
            std::string out;
            for (const auto& a : args) out += a.asString();
            return Value(std::move(out));
        }
        Value::array_type out;
        for (const auto& a : args)
            out.insert(out.end(), a.asArray().begin(), a.asArray().end());
        return Value(std::move(out));
    });

    // items(m: map) -> arr of [key, value]
    // same (alphabetical) key order as keys()/values()
    interp.bindSig("items", {pMap("m")},
    [](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        std::vector<const Value::map_type::value_type*> entries;
        entries.reserve(m.size());
        for (const auto& kv : m) entries.push_back(&kv);
        std::sort(entries.begin(), entries.end(),
                  [](auto* a, auto* b) { return a->first < b->first; });
        Value::array_type out;
        out.reserve(entries.size());
        for (auto* kv : entries)
            out.push_back(Value(Value::array_type{Value(kv->first), kv->second}));
        return Value(std::move(out));
    });

    // min(arr: arr) / max(arr: arr) -> num
    // numeric arrays only; raises on an empty array or a non-number element.
    // keeps the element's own int/float type.
    auto extremum = [](const char* fname, bool wantMax) {
        return [fname, wantMax](const std::vector<Value>& args) -> Value {
            const auto& arr = args[0].asArray();
            if (arr.empty()) raiseError(fname, "empty array has no " + std::string(wantMax ? "max" : "min"));
            const Value* best = nullptr;
            for (size_t i = 0; i < arr.size(); ++i) {
                if (!arr[i].isNumeric())
                    raiseError(fname, "element " + std::to_string(i) + " is " +
                               arr[i].typeName() + ", expected a number");
                if (!best || (wantMax ? arr[i].asNumber() > best->asNumber()
                                      : arr[i].asNumber() < best->asNumber()))
                    best = &arr[i];
            }
            return *best;
        };
    };
    interp.bindSig("min", {pArr("arr")}, extremum("min", false));
    interp.bindSig("max", {pArr("arr")}, extremum("max", true));

    // sum(arr: arr) -> num
    // exact int64 sum while every element is an int and nothing overflows;
    // falls back to a double sum otherwise. empty array -> 0.
    interp.bindSig("sum", {pArr("arr")},
    [](const std::vector<Value>& args) -> Value {
        const auto& arr = args[0].asArray();
        int64_t  isum = 0;
        double   dsum = 0.0;
        bool     exact = true;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].isNumeric())
                raiseError("sum", "element " + std::to_string(i) + " is " +
                           arr[i].typeName() + ", expected a number");
            dsum += arr[i].asNumber();
            if (exact) {
                if (!arr[i].isInt() || __builtin_add_overflow(isum, arr[i].asInt(), &isum))
                    exact = false;
            }
        }
        return exact ? Value(isum) : Value(dsum);
    });

    // ---- predicate / reshaping helpers ----------------------------------------------------------
    // convention: a helper that takes a script function is named *_do (like for_each_do, sort_do); every
    // callback goes through embr::invoke so both backends and __call pointers work. embr has no nil, so
    // "not found" is an error, a default argument, or -1, never a missing value.

    // strict equality for unique()/index_of(): same core type AND ==, so 5 and "5" (equal under the
    // loose cross-type == fallback) and 5 and 5.0 stay distinct
    auto sameValue = [](const Value& a, const Value& b) { return a.tag() == b.tag() && a == b; };

    // find_do(arr, pred, default?) -> the first element for which pred(el) is truthy; `default` if none
    // match and one was given, otherwise raises
    interp.bindSig("find_do", {pArr("arr"), pFnOrPtr("pred"), pOpt("default")},
    [&interp](const std::vector<Value>& args) -> Value {
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) return el;
        if (args.size() >= 3) return args[2];
        raiseError("find_do", "no element matches (pass a default to avoid this error)");
    });

    // find_index_do(arr, pred) -> index of the first match, or -1
    interp.bindSig("find_index_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        const auto& a = args[0].asArray();
        for (size_t i = 0; i < a.size(); ++i)
            if (invoke(interp, args[1], {a[i]}).truthy()) return Value((int64_t)i);
        return Value((int64_t)-1);
    });

    // index_of(arr, value) -> index of the first element strictly equal to value, or -1
    interp.bindSig("index_of", {pArr("arr"), pAny("value")},
    [sameValue](const std::vector<Value>& args) -> Value {
        const auto& a = args[0].asArray();
        for (size_t i = 0; i < a.size(); ++i)
            if (sameValue(a[i], args[1])) return Value((int64_t)i);
        return Value((int64_t)-1);
    });

    // any_do(arr, pred) / all_do(arr, pred) -> 1/0, short-circuiting (all_do of [] is 1, any_do of [] is 0)
    interp.bindSig("any_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) return Value(1.0);
        return Value(0.0);
    });
    interp.bindSig("all_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        for (const auto& el : args[0].asArray())
            if (!invoke(interp, args[1], {el}).truthy()) return Value(0.0);
        return Value(1.0);
    });

    // count_do(arr, pred) -> how many elements satisfy pred
    interp.bindSig("count_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        int64_t n = 0;
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) ++n;
        return Value(n);
    });

    // partition(arr, pred) -> [matching, non_matching], each in the original order
    interp.bindSig("partition", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const std::vector<Value>& args) -> Value {
        Value::array_type yes, no;
        for (const auto& el : args[0].asArray())
            (invoke(interp, args[1], {el}).truthy() ? yes : no).push_back(el);
        return Value(Value::array_type{Value(std::move(yes)), Value(std::move(no))});
    });

    // unique(arr) -> first occurrence of each distinct element, original order (strict equality: see
    // sameValue). bucketed by type + printed form first, so typical arrays are O(n), not O(n^2)
    interp.bindSig("unique", {pArr("arr")},
    [sameValue](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        std::unordered_map<std::string, std::vector<size_t>> seen;      // bucket key -> indices into out
        for (const auto& el : args[0].asArray()) {
            std::string key = std::to_string((int)el.tag()) + ":" + el.formatAsString();
            auto& bucket = seen[key];
            bool dup = false;
            for (size_t idx : bucket) if (sameValue(out[idx], el)) { dup = true; break; }
            if (dup) continue;
            bucket.push_back(out.size());
            out.push_back(el);
        }
        return Value(std::move(out));
    });

    // flatten(arr, depth?) -> arr with nested arrays spliced in, `depth` levels deep (default 1, max 100)
    interp.bindSig("flatten", {pArr("arr"), pOpt("depth", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        int64_t depth = args.size() >= 2 ? numToInt64(args[1], "flatten", "depth") : 1;
        if (depth < 0 || depth > 100) raiseError("flatten", "depth must be between 0 and 100");
        std::function<void(const Value::array_type&, int64_t, Value::array_type&)> go =
            [&](const Value::array_type& in, int64_t d, Value::array_type& out) {
                for (const auto& el : in) {
                    if (d > 0 && el.isArray()) go(el.asArray(), d - 1, out);
                    else out.push_back(el);
                }
            };
        Value::array_type out;
        go(args[0].asArray(), depth, out);
        return Value(std::move(out));
    });

    // reverse(arr|str) -> a reversed copy (strings are reversed byte-wise; see unicode_chars for code points)
    interp.bindSig("reverse", {Param::req("v", TS::Arr|TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].isString()) { std::string s = args[0].asString(); std::reverse(s.begin(), s.end()); return Value(std::move(s)); }
        Value::array_type a = args[0].asArray();
        std::reverse(a.begin(), a.end());
        return Value(std::move(a));
    });

    // chunk(arr, n) -> arrays of up to n elements (the last may be shorter); n must be >= 1
    interp.bindSig("chunk", {pArr("arr"), pNum("n")},
    [](const std::vector<Value>& args) -> Value {
        int64_t n = numToInt64(args[1], "chunk", "n");
        if (n < 1) raiseError("chunk", "n must be >= 1");
        const auto& a = args[0].asArray();
        Value::array_type out;
        for (size_t i = 0; i < a.size(); i += (size_t)n)
            out.push_back(Value(Value::array_type(a.begin() + i, a.begin() + std::min(a.size(), i + (size_t)n))));
        return Value(std::move(out));
    });

    // take(arr, n) / drop(arr, n) -> the first n elements / everything after them (n clamped to 0..len)
    interp.bindSig("take", {pArr("arr"), pNum("n")},
    [](const std::vector<Value>& args) -> Value {
        const auto& a = args[0].asArray();
        int64_t n = std::max<int64_t>(0, std::min<int64_t>(numToInt64(args[1], "take", "n"), (int64_t)a.size()));
        return Value(Value::array_type(a.begin(), a.begin() + n));
    });
    interp.bindSig("drop", {pArr("arr"), pNum("n")},
    [](const std::vector<Value>& args) -> Value {
        const auto& a = args[0].asArray();
        int64_t n = std::max<int64_t>(0, std::min<int64_t>(numToInt64(args[1], "drop", "n"), (int64_t)a.size()));
        return Value(Value::array_type(a.begin() + n, a.end()));
    });

    // group_by(arr, keyfn) -> map of key -> array of the elements with that key, in original order.
    // keyfn must return a string or a number (numbers become their printed form: embr map keys are strings)
    interp.bindSig("group_by", {pArr("arr"), pFnOrPtr("keyfn")},
    [&interp](const std::vector<Value>& args) -> Value {
        std::unordered_map<std::string, Value::array_type> tmp;      // built as plain vectors, converted once
        for (const auto& el : args[0].asArray()) {
            Value k = invoke(interp, args[1], {el});
            if (!k.isString() && !k.isNumeric())
                raiseError("group_by", "keyfn must return a string or number, got " + k.typeName());
            tmp[k.isString() ? k.asString() : k.formatAsString()].push_back(el);
        }
        Value::map_type groups;
        for (auto& [key, vec] : tmp) groups.emplace(key, Value(std::move(vec)));
        return Value(std::move(groups));
    });

    // min_do(arr, weight) / max_do(arr, weight) -> the element with the smallest / largest weight(el)
    // (the first one on ties); weight must return a number; raises on an empty array
    auto extremeBy = [&interp](const char* fname, bool wantMax) {
        return [&interp, fname, wantMax](const std::vector<Value>& args) -> Value {
            const auto& a = args[0].asArray();
            if (a.empty()) raiseError(fname, "empty array");
            size_t best = 0; double bestW = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                Value w = invoke(interp, args[1], {a[i]});
                if (!w.isNumeric())
                    raiseError(fname, "weight returned " + w.typeName() + " for element " + std::to_string(i) + ", expected a number");
                double d = w.asNumber();
                if (i == 0 || (wantMax ? d > bestW : d < bestW)) { best = i; bestW = d; }
            }
            return a[best];
        };
    };
    interp.bindSig("min_do", {pArr("arr"), pFnOrPtr("weight")}, extremeBy("min_do", false));
    interp.bindSig("max_do", {pArr("arr"), pFnOrPtr("weight")}, extremeBy("max_do", true));

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
        requireFileIo(interp, "load_file");
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
        requireFileIo(interp, "write_file");
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
        requireFileIo(interp, "append_file");
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
        // element count up front, so a huge span is an error instead of exhausting memory
        // (an array element is a ~48-byte Value, so the limit is far below the byte-oriented kMaxScriptAlloc)
        static constexpr double kMaxRangeElements = 10000000.0;
        double count = std::ceil((end - start) / step);
        if (!(count >= 0) || count > kMaxRangeElements)
            raiseError("range", "range would have " + formatNumber(count) + " elements (limit " +
                       formatNumber(kMaxRangeElements) + "); iterate with a while loop instead");

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

    // load_module(path: str, reload?: num) -> map
    //
    // loads and runs an embr script module and returns its non-local bindings as a map. unlike `import "foo.embr"` it
    // does NOT touch the caller's scope (see loadEmbrModule in core/invoke.h). later calls with the same resolved path do
    // nothing (returning {}) unless reload is truthy
    //
    //   utils = load_module("utils")        # loads utils.embr
    //   m     = load_module("lib/math")     # loads lib/math.embr relative to caller
    //   a     = load_module("/abs/path")    # absolute
    //   utils = load_module("utils", 1)     # force reload
    //
    // works under either backend
    interp.bindSig("load_module",
        {pStr("path"), pOpt("reload")},
    [&interp](const std::vector<Value>& args) -> Value {
        const std::string& rawPath = args[0].asString();
        bool forceReload = args.size() >= 2 && args[1].truthy();
        return loadEmbrModule(interp, rawPath, {}, /*mutateCallerScope=*/false, forceReload);
    });

} // registerEmbrlib

EMBR_PLUGIN {
    registerEmbrLib(*interp);
}
