// vec.cpp
// a growable array that you change in place. embr's own arrays are values: every read, write or push copies the
// whole array, so building or updating a big one is slow. a vec is a handle, copying it shares the same elements.
//
// usage
//   import "vec"
//   v = vec()
//   vec_push(v, 10)
//   vec_push(v, 20)
//   vec_set(v, 0, 11)
//   print(vec_get(v, 0))      # 11
//   print(vec_len(v))         # 2
//   print(vec_to_array(v))    # [11, 20]
//
// good to know
//   - `b = a` makes b the same vec, not a copy. use vec_copy(a) for a separate one
//   - there is no v[i] syntax, use vec_get and vec_set. every call is O(1) except insert, remove, extend, slice, copy
//   - for loops only walk arrays and maps. use vec_each_do(v, fn) (fn gets the element and its index), or
//     `for x in vec_to_array(v)` when you want a copy to loop over
//   - len(v) and has(v, i) work, and embrlib's push(v, x) and pop(v) change a vec in place. nothing else in embrlib
//     takes a vec: those functions would only copy it, so say so with vec_to_array(v)
//   - json_stringify, csv_stringify and rand_choice read a vec where it is
//   - not synchronized, use a vec from one thread only

#include "vec.h"

using namespace embr;
using namespace embrvec;

static Param pPtr(std::string n) { return Param::req(std::move(n), TS::Ptr); }
static Param pAny(std::string n) { return Param::req(std::move(n), TS::Any); }
static Param pNum(std::string n) { return Param::req(std::move(n), TS::Num); }

static Vec* requireVec(const std::string& fn, const Value& v) {
    Vec* p = vecOf(v);
    if (!p) raiseError(fn, "expected a vec, got " + v.typeName());
    return p;
}

// index into 0..size-1, or raise
static size_t checkIndex(const std::string& fn, const Value& idx, size_t size) {
    int64_t i = numToInt64(idx, fn, "index");
    if (i < 0 || i >= (int64_t)size)
        raiseError(fn, "index " + std::to_string(i) + " out of bounds (length " + std::to_string(size) + ")");
    return (size_t)i;
}

static Value makeVec(Interpreter* interp, Value::array_type items) {
    Value h = Value::makeUserdata<Vec>(kTag);
    Vec* v = static_cast<Vec*>(h.asPointer().ptr);
    v->items = std::move(items);
    // empty it at interpreter teardown, so a vec that holds itself (or a closure over it) doesn't leak
    interp->addClearable(h.asPointer().owner, [w = std::weak_ptr<Vec>(std::static_pointer_cast<Vec>(h.asPointer().owner))] {
        if (auto p = w.lock()) p->items.clear();
    });
    // gc_collect() must see what the vec holds, or a closure stored in it looks unreachable
    h.withTracer([v](const TraceVisitor& visit) { for (const auto& x : v->items) visit(x); });
    return h;
}

EMBR_PLUGIN {

    // vec() -> ptr<vec.handle>
    interp->bind("vec", [interp](const std::vector<Value>&) -> Value {
        return makeVec(interp, {});
    });

    // vec_from(arr) -> vec, a copy of the array's elements
    interp->bindSig("vec_from", {Param::req("arr", TS::Arr)},
    [interp](const std::vector<Value>& args) -> Value {
        return makeVec(interp, args[0].asArray());
    });

    interp->bindSig("vec_len", {pPtr("v")},
    [](const std::vector<Value>& args) -> Value {
        return Value((int64_t)requireVec("vec_len", args[0])->items.size());
    });

    interp->bindSig("vec_get", {pPtr("v"), pNum("i")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_get", args[0]);
        return v->items[checkIndex("vec_get", args[1], v->items.size())];
    });

    interp->bindSig("vec_set", {pPtr("v"), pNum("i"), pAny("x")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_set", args[0]);
        v->items[checkIndex("vec_set", args[1], v->items.size())] = args[2];
        return Value(0.0);
    });

    interp->bindSig("vec_push", {pPtr("v"), pAny("x")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_push", args[0]);
        checkAllocSize("vec_push", "vec length", (int64_t)v->items.size() + 1);
        v->items.push_back(args[1]);
        return Value(0.0);
    });

    // vec_pop(v) -> the removed last element. raises on an empty vec
    interp->bindSig("vec_pop", {pPtr("v")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_pop", args[0]);
        if (v->items.empty()) raiseError("vec_pop", "vec is empty");
        Value last = std::move(v->items.back());
        v->items.pop_back();
        return last;
    });

    // vec_insert(v, i, x): i may be vec_len(v), which appends
    interp->bindSig("vec_insert", {pPtr("v"), pNum("i"), pAny("x")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_insert", args[0]);
        int64_t i = numToInt64(args[1], "vec_insert", "index");
        if (i < 0 || i > (int64_t)v->items.size())
            raiseError("vec_insert", "index " + std::to_string(i) + " out of bounds (length " +
                       std::to_string(v->items.size()) + ")");
        checkAllocSize("vec_insert", "vec length", (int64_t)v->items.size() + 1);
        v->items.insert(v->items.begin() + i, args[2]);
        return Value(0.0);
    });

    // vec_remove(v, i) -> the removed element
    interp->bindSig("vec_remove", {pPtr("v"), pNum("i")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_remove", args[0]);
        size_t i = checkIndex("vec_remove", args[1], v->items.size());
        Value out = std::move(v->items[i]);
        v->items.erase(v->items.begin() + i);
        return out;
    });

    interp->bindSig("vec_swap", {pPtr("v"), pNum("i"), pNum("j")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_swap", args[0]);
        size_t i = checkIndex("vec_swap", args[1], v->items.size());
        size_t j = checkIndex("vec_swap", args[2], v->items.size());
        std::swap(v->items[i], v->items[j]);
        return Value(0.0);
    });

    interp->bindSig("vec_reverse", {pPtr("v")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_reverse", args[0]);
        std::reverse(v->items.begin(), v->items.end());
        return Value(0.0);
    });

    interp->bindSig("vec_clear", {pPtr("v")},
    [](const std::vector<Value>& args) -> Value {
        requireVec("vec_clear", args[0])->items.clear();
        return Value(0.0);
    });

    // vec_extend(v, other): other is an array or another vec (or v itself) and is appended in order
    interp->bindSig("vec_extend", {pPtr("v"), Param::req("other", TS::Arr | TS::Ptr)},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_extend", args[0]);
        Value::array_type extra;   // copied first, so extending a vec with itself is fine
        if (args[1].isArray()) extra = args[1].asArray();
        else extra = requireVec("vec_extend", args[1])->items;
        checkAllocSize("vec_extend", "vec length", (int64_t)(v->items.size() + extra.size()));
        v->items.insert(v->items.end(), extra.begin(), extra.end());
        return Value(0.0);
    });

    // vec_to_array(v) -> arr, a copy of the elements
    interp->bindSig("vec_to_array", {pPtr("v")},
    [](const std::vector<Value>& args) -> Value {
        return Value(Value::array_type(requireVec("vec_to_array", args[0])->items));
    });

    // vec_slice(v, start, end?) -> arr of v[start..end), both ends clamped to the vec
    interp->bindSig("vec_slice", {pPtr("v"), pNum("start"), Param::opt("end", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_slice", args[0]);
        int64_t n = (int64_t)v->items.size();
        int64_t s = std::max<int64_t>(0, std::min(numToInt64(args[1], "vec_slice", "start"), n));
        int64_t e = args.size() >= 3 ? std::max<int64_t>(s, std::min(numToInt64(args[2], "vec_slice", "end"), n)) : n;
        return Value(Value::array_type(v->items.begin() + s, v->items.begin() + e));
    });

    // vec_copy(v) -> a new vec with the same elements (the elements themselves are values, so they are copied)
    interp->bindSig("vec_copy", {pPtr("v")},
    [interp](const std::vector<Value>& args) -> Value {
        return makeVec(interp, requireVec("vec_copy", args[0])->items);
    });

    // vec_each_do(v, fn): calls fn(x, i) for each element, in order. stops early and returns fn's result as soon as
    // it is truthy, otherwise returns 0. it walks the vec where it is (no copy), checking the length every step, so
    // fn may push (the new elements are visited too), set, or remove without anything breaking
    interp->bindSig("vec_each_do", {pPtr("v"), Param::req("fn", TS::Fn | TS::Ptr)},
    [interp](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_each_do", args[0]);
        for (size_t i = 0; i < v->items.size(); ++i) {
            Value x = v->items[i];   // a copy: fn may grow the vec and move its storage
            Value r = invoke(*interp, args[1], {x, Value((int64_t)i)});
            if (r.truthy()) return r;
        }
        return Value(0.0);
    });

    // vec_map_do(v, fn): replaces each element x with fn(x, i), in place. same live walk as vec_each_do
    interp->bindSig("vec_map_do", {pPtr("v"), Param::req("fn", TS::Fn | TS::Ptr)},
    [interp](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_map_do", args[0]);
        for (size_t i = 0; i < v->items.size(); ++i) {
            Value x = v->items[i];
            Value r = invoke(*interp, args[1], {x, Value((int64_t)i)});
            if (i < v->items.size()) v->items[i] = std::move(r);   // fn may have removed it
        }
        return Value(0.0);
    });

    // vec_assign(v, other): replace everything in v with the elements of other (an array or another vec).
    // the in-place way to sort or filter a vec: vec_assign(v, sort(v))
    interp->bindSig("vec_assign", {pPtr("v"), Param::req("other", TS::Arr | TS::Ptr)},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("vec_assign", args[0]);
        Value::array_type items = args[1].isArray() ? args[1].asArray() : requireVec("vec_assign", args[1])->items;
        v->items = std::move(items);
        return Value(0.0);
    });

    // len(v) and has(v, i) from embrlib find these by name (the dunder convention in core/registry.h), so the
    // usual calls work on a vec: len(v), has(v, 2)
    interp->bindSig("__vec_handle_len", {pPtr("v")},
    [](const std::vector<Value>& args) -> Value {
        return Value((int64_t)requireVec("len", args[0])->items.size());
    });
    interp->bindSig("__vec_handle_has", {pPtr("v"), pAny("i")},
    [](const std::vector<Value>& args) -> Value {
        Vec* v = requireVec("has", args[0]);
        int64_t i = numToInt64(args[1], "has", "index");
        return Value(i >= 0 && i < (int64_t)v->items.size() ? 1.0 : 0.0);
    });

    // is_vec(x) -> 1 if x is a vec handle, else 0
    interp->bindSig("is_vec", {pAny("x")},
    [](const std::vector<Value>& args) -> Value {
        return Value(vecOf(args[0]) ? 1.0 : 0.0);
    });
}
