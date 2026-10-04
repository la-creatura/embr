#ifndef EMBR_CORE_VALUE_H
#define EMBR_CORE_VALUE_H

#include "types.h"
#include "fwd.h"
#include "diagnostics.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <functional>
#include <memory>
#include <sstream>
#include <cstdint>
#include <type_traits>
#include <algorithm>

namespace embr {

struct ScriptFn {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    const std::vector<StmtPtr>* body = nullptr;  // Interpreter owns; the tree-walker backend executes this directly
    SourceRange        definedAt;
    std::string        sourceFile;
    std::shared_ptr<CaptureFrame> captured;
    // type-erased embr::vm::CompiledChunk*. set only when the vm's Compiler compiled this function, null otherwise
    // it's type-erased because core/ can't depend on backends/vm.h (a peer header)
    // callers that need it use std::static_pointer_cast<vm::CompiledChunk>(compiledChunk)
    // see core/invoke.h for the dispatch this exists for
    std::shared_ptr<void> compiledChunk;
};

struct Value;

// how a plugin tells the gc plugin which Values its opaque userdata holds. the gc can't look inside a type-erased
// `owner`, so a handle whose object stores Values (a table's entries, a callback's closure) attaches a TraceFn
// that calls `visit` on each of them. without it, a closure reachable only through such a handle looks
// unreachable, and gc_collect() would clear its captured scope and break a live closure
using TraceVisitor = std::function<void(const Value&)>;
using TraceFn      = std::function<void(const TraceVisitor&)>;

// a plugin-typed opaque pointer/userdata handle
struct TypedPtr {
    void*                 ptr = nullptr;
    std::string            type;   // empty = untyped/raw
    std::shared_ptr<void>  owner;  // non-null only for makeUserdata()-created handles
    // optional, shared by every copy of this handle; see TraceFn. it may capture a raw pointer to the
    // object in `owner`: it is only ever invoked through a TypedPtr that also holds `owner`, so the
    // object is alive. (A handle without an owner, e.g. ffi callbacks, must not capture a raw
    // pointer; look the object up by key in weakly-held plugin state instead.)
    std::shared_ptr<const TraceFn> tracer;

    TypedPtr() = default;
    explicit TypedPtr(void* p) : ptr(p) {}
    TypedPtr(void* p, std::string t) : ptr(p), type(std::move(t)) {}
    TypedPtr(void* p, std::string t, std::shared_ptr<void> own)
        : ptr(p), type(std::move(t)), owner(std::move(own)) {}

    // same address and same tag. the tag matters so two null pointers with different tags (embrtypes' nil and false)
    // stay different. see the header of plugins/embrtypes/embrtypes.cpp
    bool operator==(const TypedPtr& o) const { return ptr == o.ptr && type == o.type; }
    bool operator!=(const TypedPtr& o) const { return !(*this == o); }
};

std::string formatNumber(double d) {
    if (d >= -1e15 && d <= 1e15 && d == static_cast<double>(static_cast<long long>(d))) {
        std::ostringstream oss; oss << static_cast<long long>(d); return oss.str();
    }
    std::string s = std::to_string(d);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

struct Value {
    using array_type = std::vector<Value>;
    using map_type   = std::unordered_map<std::string, Value>;
    using NativeFn   = std::function<Value(const std::vector<Value>&)>;
    using int_type   = int64_t;
    using ptr_type   = TypedPtr;

    // heap allocated via shared_ptr to stay copyable
    struct Callable {
        enum class Kind { Native, Script } kind = Kind::Native;
        NativeFn native;
        ScriptFn           script;  // script.name also holds native display name
        std::vector<Param> sig;

        static Callable fromNative(std::string nm, NativeFn fn, std::vector<Param> sig) {
            Callable c; c.kind = Kind::Native; c.native = std::move(fn);
            c.script.name = std::move(nm); c.sig = std::move(sig);
            return c;
        }
        static Callable fromScript(ScriptFn fn) {
            Callable c; c.kind = Kind::Script; c.script = std::move(fn);
            return c;
        }
        const std::string& name()     const { return script.name; }
        bool               isNative() const { return kind == Kind::Native; }
        bool               isScript() const { return kind == Kind::Script; }
    };
    using fn_type = std::shared_ptr<Callable>;

    // variant order must match TypeTag bit order
    std::variant<double, int_type, std::string, array_type, map_type, fn_type, ptr_type> data;

    // how deeply arrays/maps are nested in this value: 0 for anything else, 1 + the deepest element for arrays/maps
    // (an empty array is 1). kept up to date by the array/map constructors and copied along, so it's one pass over the
    // elements, not a recursive walk. exists because copying, destroying, comparing and printing recurse once per
    // level: `a = [a]` in a loop could build a value ~20,000 deep and crash the process. so building anything
    // deeper than kMaxValueDepth is an ordinary error
    static constexpr uint32_t kMaxValueDepth = 1000;
    uint32_t nestDepth = 0;

    Value()                     : data(0.0)           {}
    explicit Value(double v)    : data(v)             {}
    explicit Value(int_type v)  : data(v)             {}
    // constrained to an exact bool
    // so a stray raw pointer passed to Value(...) fails to compile via pointer-to-bool conversion instead of silently constructing Value(true)
    template<class B, typename = std::enable_if_t<std::is_same_v<B, bool>>>
    explicit Value(B b)         : data(b ? 1.0 : 0.0) {}
    Value(const std::string& v) : data(v)             {}
    Value(std::string&& v)      : data(std::move(v))  {}
    Value(array_type v)         : data(std::move(v))  { computeNestDepth(); }
    Value(map_type v)           : data(std::move(v))  { computeNestDepth(); }
    Value(fn_type v)            : data(std::move(v))  {}
    explicit Value(ptr_type p)  : data(std::move(p))  {}

    static Value makeNative(std::string name, NativeFn fn,
                            std::vector<Param> sig = {}) {
        return Value(std::make_shared<Callable>(
            Callable::fromNative(std::move(name), std::move(fn), std::move(sig))));
    }
    static Value makeScript(ScriptFn fn) {
        return Value(std::make_shared<Callable>(Callable::fromScript(std::move(fn))));
    }

    void computeNestDepth() {
        uint32_t deepest = 0;
        if (auto* a = std::get_if<array_type>(&data)) {
            for (const auto& el : *a) if (el.nestDepth > deepest) deepest = el.nestDepth;
        } else if (auto* m = std::get_if<map_type>(&data)) {
            for (const auto& kv : *m) if (kv.second.nestDepth > deepest) deepest = kv.second.nestDepth;
        }
        nestDepth = deepest + 1;
        if (nestDepth > kMaxValueDepth)
            raiseError("value", "value nested too deeply (max depth " + std::to_string(kMaxValueDepth) + ")");
    }

    // in-place edits of an array, map or string. the backends and the in-out natives use these instead of
    // copy, change, store back, and they keep nestDepth right. callers do their own range checks so the error
    // can name the script line. these only back-stop it
    void arrayAppend(Value v) {
        auto& a = arrayMut();
        checkElementDepth(v);
        uint32_t nw = v.nestDepth;
        a.push_back(std::move(v));
        noteReplaced(0, nw);
    }
    void arrayInsert(size_t i, Value v) {
        auto& a = arrayMut();
        if (i > a.size()) raiseError("value", "array insert position " + std::to_string(i) + " out of bounds");
        checkElementDepth(v);
        uint32_t nw = v.nestDepth;
        a.insert(a.begin() + (std::ptrdiff_t)i, std::move(v));
        noteReplaced(0, nw);
    }
    void arraySet(size_t i, Value v) {
        auto& a = arrayMut();
        if (i >= a.size()) raiseError("value", "array index " + std::to_string(i) + " out of bounds");
        checkElementDepth(v);
        uint32_t old = a[i].nestDepth, nw = v.nestDepth;
        a[i] = std::move(v);
        noteReplaced(old, nw);
    }
    Value arrayRemoveAt(size_t i) {
        auto& a = arrayMut();
        if (i >= a.size()) raiseError("value", "array index " + std::to_string(i) + " out of bounds");
        Value out = std::move(a[i]);
        a.erase(a.begin() + (std::ptrdiff_t)i);
        noteRemoved(out.nestDepth);
        return out;
    }
    void mapSet(const std::string& k, Value v) {
        auto& m = mapMut();
        checkElementDepth(v);
        uint32_t nw = v.nestDepth;
        auto it = m.find(k);
        if (it != m.end()) {
            uint32_t old = it->second.nestDepth;
            it->second = std::move(v);
            noteReplaced(old, nw);
        } else {
            m.emplace(k, std::move(v));
            noteReplaced(0, nw);
        }
    }
    // false if there was no such key. `removed` (if given) gets the old value
    bool mapRemove(const std::string& k, Value* removed = nullptr) {
        auto& m = mapMut();
        auto it = m.find(k);
        if (it == m.end()) return false;
        Value old = std::move(it->second);
        m.erase(it);
        noteRemoved(old.nestDepth);
        if (removed) *removed = std::move(old);
        return true;
    }
    // replaces s.size() characters from position i, like std::string::replace
    void stringSetAt(size_t i, const std::string& s) {
        if (!isString()) raiseError("value", "expected string, got " + typeName());
        auto& str = std::get<std::string>(data);
        if (i >= str.size()) raiseError("value", "string index " + std::to_string(i) + " out of bounds");
        str.replace(i, s.size(), s);
    }

    // raises if `elem`, put into a container that has `levelsAbove` more containers above it, would go past kMaxValueDepth
    static void checkElementDepth(const Value& elem, size_t levelsAbove = 0) {
        if ((size_t)elem.nestDepth + 1 + levelsAbove > kMaxValueDepth)
            raiseError("value", "value nested too deeply (max depth " + std::to_string(kMaxValueDepth) + ")");
    }
    // nestDepth bookkeeping after one element changed from oldDepth to newDepth. cheap unless the deepest element
    // was replaced by a shallower one
    void noteReplaced(uint32_t oldDepth, uint32_t newDepth) {
        if (newDepth + 1 > nestDepth) nestDepth = newDepth + 1;
        else if (oldDepth + 1 == nestDepth && newDepth < oldDepth) computeNestDepth();
    }
    void noteRemoved(uint32_t removedDepth) {
        if (removedDepth > 0 && removedDepth + 1 == nestDepth) computeNestDepth();
    }

    // constructs a typed pointer Value
    // type is a plugin-namespaced tag
    // leave it empty for an untyped raw pointer
    // named factory rather than an implicit Value(void*) constructor
    static Value makePointer(void* p, std::string type = "") {
        return Value(ptr_type(p, std::move(type)));
    }

    // constructs an owned, refcounted userdata handle
    // heap-allocates a T via make_shared, tags it type, and keeps it alive for as long as any copy of the returned Value exists
    // auto-freed when the last one is dropped
    template<class T, class... Args>
    static Value makeUserdata(std::string type, Args&&... args) {
        auto obj = std::make_shared<T>(std::forward<Args>(args)...);
        void* raw = obj.get();
        return Value(ptr_type(raw, std::move(type), std::move(obj)));
    }

    // attaches a TraceFn to this (pointer) Value's handle, call right after creating it, before any
    // copies are made, so every copy shares it. returns *this for chaining.
    Value& withTracer(TraceFn f) {
        if (!isPointer()) raiseError("value", "withTracer: expected pointer, got " + typeName());
        std::get<ptr_type>(data).tracer = std::make_shared<const TraceFn>(std::move(f));
        return *this;
    }

    TypeTag tag() const {
        // variant index order must match TypeTag bit order for bitshift hack
        return static_cast<TypeTag>(1 << data.index());
    }

    bool isNumber()   const { return std::holds_alternative<double>(data);      }
    bool isInt()      const { return std::holds_alternative<int_type>(data);    }
    bool isNumeric()  const { return isNumber() || isInt();                     }
    bool isString()   const { return std::holds_alternative<std::string>(data); }
    bool isArray()    const { return std::holds_alternative<array_type>(data);  }
    bool isMap()      const { return std::holds_alternative<map_type>(data);    }
    bool isCallable() const { return std::holds_alternative<fn_type>(data);     }
    bool isPointer()  const { return std::holds_alternative<ptr_type>(data);    }

    TypeSet typeSet() const { return TypeSet(tag()); }

    // ideally these would only produce errors when something went very, very terribly wrong

    double asNumber() const {
        if (isInt())     return (double)std::get<int_type>(data);
        if (!isNumber()) raiseError("value","expected number, got "+typeName());
        return std::get<double>(data);
    }

    int_type asInt() const {
        if (isNumber()) return (int_type)std::get<double>(data);
        if (!isInt())   raiseError("value","expected int, got "+typeName());
        return std::get<int_type>(data);
    }
    const std::string& asString()   const { if (!isString())   raiseError("value","expected string, got "  +typeName()); return std::get<std::string>(data); }
    const array_type&  asArray()    const { if (!isArray())    raiseError("value","expected array, got "   +typeName()); return std::get<array_type>(data); }
    const map_type&    asMap()      const { if (!isMap())      raiseError("value","expected map, got "     +typeName()); return std::get<map_type>(data); }
    array_type&        arrayMut()         { if (!isArray())    raiseError("value","expected array, got "   +typeName()); return std::get<array_type>(data); }
    map_type&          mapMut()           { if (!isMap())      raiseError("value","expected map, got "     +typeName()); return std::get<map_type>(data); }
    const Callable&    asCallable() const { if (!isCallable()) raiseError("value","expected function, got "+typeName()); return *std::get<fn_type>(data); }
    const ptr_type&    asPointer()  const { if (!isPointer())  raiseError("value","expected pointer, got " +typeName()); return std::get<ptr_type>(data); }

    // validating overload
    // also checks the pointer's plugin-assigned type tag
    const ptr_type& asPointer(const std::string& expectedType) const {
        const ptr_type& p = asPointer();
        if (!expectedType.empty() && p.type != expectedType)
            raiseError("value", "expected pointer of type '" + expectedType +
                       "', got '" + (p.type.empty() ? std::string("<untyped>") : p.type) + "'");
        return p;
    }

    // convenience accessor for a userdata payload's underlying object
    // does not check dynamic C++ type, only the plugin-assigned tag string
    template<class T>
    T* userdata(const std::string& expectedType = "") const {
        return static_cast<T*>(asPointer(expectedType).ptr);
    }

    std::string typeName() const { return TypeSet(tag()).name(); }

    std::string  formatAsString()   const {
        switch (tag()) {
            case TypeTag::Number:   return formatNumber(this->asNumber());
            case TypeTag::Int:      return std::to_string(std::get<int_type>(data));
            case TypeTag::String:   return asString();
            case TypeTag::Callable: return "<fn " + asCallable().name() + ">";
            case TypeTag::Array: {
                std::string s = "["; const auto& a = asArray();
                for (size_t i = 0; i < a.size(); ++i) { if (i) s += ", "; s += valueRepr(a[i]); }
                    return s + "]";
            }
            case TypeTag::Map: {
                // asMap() is an unordered_map (unspecified/unstable hash order),
                // so sort keys here for reproducible output across runs
                const auto& mp = asMap();
                std::vector<std::string> ks; ks.reserve(mp.size());
                for (const auto& [k, val] : mp) ks.push_back(k);
                std::sort(ks.begin(), ks.end());
                std::string s = "{"; bool first = true;
                for (const auto& k : ks) {
                    if (!first) s += ", ";
                        first = false;
                    s += k + ": " + valueRepr(mp.at(k));
                }
                return s + "}";
            }
            case TypeTag::Pointer: {
                // a tagged pointer prints as "<TAG 0xADDR>", the tag leads (nil prints "<nil 0x0>"). an untyped pointer
                // (empty tag) prints as "<ptr 0xADDR>"
                const auto& p = asPointer();
                std::ostringstream oss;
                oss << "<" << (p.type.empty() ? "ptr" : p.type)
                    << " 0x" << std::hex << reinterpret_cast<uintptr_t>(p.ptr) << ">";
                return oss.str();
            }
        }
        return "<unknown>";
    }

    bool operator==(Value o) const {
        if (typeSet() == o.typeSet()) {
            switch (tag()) {
                case TypeTag::Number: return asNumber() == o.asNumber();
                case TypeTag::Int:    return asInt()    == o.asInt();
                case TypeTag::String: return asString() == o.asString();
                case TypeTag::Array: {
                    const auto& arrA = asArray();
                    const auto& arrB = o.asArray();
                    if (arrA.size() != arrB.size()) return false;
                    for (size_t i = 0; i < arrA.size(); ++i)
                        if (!(arrA[i] == arrB[i])) return false;
                    return true;
                    break;
                }
                case TypeTag::Map: {
                    const auto& mapA = asMap();
                    const auto& mapB = o.asMap();
                    if (mapA.size() != mapB.size()) return false;
                    for (const auto& [key, valA] : mapA) {
                        auto it = mapB.find(key);
                        if (it == mapB.end()) return false;
                        if (!(valA == it->second)) return false;
                    }
                    return true;
                    break;
                }
                // identity: two callables are equal iff they are the same function object (every copy of a
                // closure value shares one Callable, so `f == f` and `g = f; g == f` are true, while two
                // separately created closures, even with identical source, are different)
                case TypeTag::Callable: return std::get<fn_type>(data) == std::get<fn_type>(o.data);
                case TypeTag::Pointer:  return asPointer() == o.asPointer();
            }
        }
        // cross-subtype numeric equality (5 == 5.0) falls out of the general string-formatting fallback below,
        // same as any other mismatched-tag comparison
        return formatAsString() == o.formatAsString();
    }

    bool operator!=(Value o) const { return !(*this == o); }

    // 0.0, "", [], {} are falsy
    // functions truthy
    bool truthy() const {
        switch (tag()) {
            case TypeTag::Number:   return  std::get<double>(data) != 0.0;
            case TypeTag::Int:      return  std::get<int_type>(data) != 0;
            case TypeTag::String:   return !std::get<std::string>(data).empty();
            case TypeTag::Array:    return !std::get<array_type>(data).empty();
            case TypeTag::Map:      return !std::get<map_type>(data).empty();
            case TypeTag::Callable: return true;
            case TypeTag::Pointer:  return std::get<ptr_type>(data).ptr != nullptr;
        }
        return false;
    }
};

// int64 arithmetic for `+ - * %` and unary `-` on two Ints, shared by both backends so they can't drift
// plain signed overflow is undefined behavior, and INT64_MIN % -1 raises SIGFPE and kills the process
// so: a result that doesn't fit in int64 widens to a double (like an integer literal that overflows),
// and INT64_MIN % -1 is 0
inline Value intAdd(int64_t a, int64_t b) {
    int64_t r; if (__builtin_add_overflow(a, b, &r)) return Value((double)a + (double)b); return Value(r);
}
inline Value intSub(int64_t a, int64_t b) {
    int64_t r; if (__builtin_sub_overflow(a, b, &r)) return Value((double)a - (double)b); return Value(r);
}
inline Value intMul(int64_t a, int64_t b) {
    int64_t r; if (__builtin_mul_overflow(a, b, &r)) return Value((double)a * (double)b); return Value(r);
}
inline Value intMod(int64_t a, int64_t b) {   // b != 0, checked by the caller
    return Value(b == -1 ? (int64_t)0 : a % b);
}
inline Value intNeg(int64_t a) {
    return a == INT64_MIN ? Value(-(double)a) : Value(-a);
}

// checked conversion of a script number to int64_t for natives. a raw (int64_t)d on NaN/Inf/out-of-range
// is undefined behaviour (and often a huge allocation or a wrapped-around size_t), so every native that
// turns a script number into a count, size, index or code point should go through this: it raises a
// catchable error instead. truncates toward zero, like the plain casts it replaces.
inline int64_t numToInt64(const Value& v, const std::string& fn, const std::string& what) {
    if (v.isInt()) return v.asInt();
    double d = v.asNumber();
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        raiseError(fn, what + " must be a finite number in integer range, got " + v.formatAsString());
    return (int64_t)d;
}

// the largest single string / buffer / array a native will build on a script's behalf. without a cap a
// one-line script (`str_repeat("ab", 4000000000000)`, `buffer(1e18)`, `range(0, 1e11)`) makes the process
// exhaust memory or die from an uncatchable std::bad_alloc / std::length_error.
constexpr int64_t kMaxScriptAlloc = 256LL * 1024 * 1024;
inline void checkAllocSize(const std::string& fn, const std::string& what, int64_t n) {
    if (n < 0 || n > kMaxScriptAlloc)
        raiseError(fn, what + " (" + std::to_string(n) + ") exceeds the limit of " +
                   std::to_string(kMaxScriptAlloc) + " elements/bytes");
}

inline std::string valueRepr(const Value& v) {
    if (v.isString()) return '"' + v.asString() + '"';
    return v.formatAsString();
}

// references for in-out parameters
//
// `append(&list, x)` hands the native a reference to the caller's variable instead of a copy of its value. a reference
// is never a script value: the call site makes it just before the call and drops it right after, and the backends
// refuse to let one come back out of a native. two stages:
//   kRefPathTag  what `&a[i]["k"]` evaluates to: the variable's name and the already worked-out keys. it points at
//                nothing yet, so evaluating later arguments can't leave it dangling
//   kRefTag      made by the backend once every argument is in: .ptr is the Value to change in place
// a native with an in-out parameter gets a kRefTag Value and calls inoutTarget() on it. the native must keep the
// target's type and must not run script code (which could resize the container the pointer lives in)
inline constexpr const char* kRefPathTag = "embr.refpath";
inline constexpr const char* kRefTag     = "embr.ref";

struct RefPath {
    int                nameIdx = 0;   // string-table index (VM) of the variable
    int                slot    = -1;  // VM local slot, -1 for a global
    std::string        name;          // the same name, for the tree-walker and for error messages
    std::vector<Value> keys;          // one per [..] after the name
};

// the containers between the variable and the target, outermost first, and what each one's child nestDepth was
// before the call. settleRefs() uses it to fix their nestDepth afterwards
struct RefChain {
    struct Level { Value* container; uint32_t childDepth; };
    std::vector<Level> above;
};

inline bool isRefPath(const Value& v) { return v.isPointer() && v.asPointer().type == kRefPathTag; }
inline bool isRef(const Value& v)     { return v.isPointer() && v.asPointer().type == kRefTag; }
inline RefPath& refPathOf(const Value& v) { return *static_cast<RefPath*>(v.asPointer().ptr); }
inline Value&   refTarget(const Value& v) { return *static_cast<Value*>(v.asPointer().ptr); }

inline Value makeRefPath(RefPath p) {
    auto sp = std::make_shared<RefPath>(std::move(p));
    return Value(TypedPtr(sp.get(), kRefPathTag, sp));
}

// follows path.keys down from `root` (the variable). returns the target, or null with `err` set. arrays are indexed in
// range, maps by a key they hold themselves (not through __proto__): a reference must name something that exists
inline Value* resolveRefPath(Value* root, const RefPath& path, std::shared_ptr<RefChain>& chain, std::string& err) {
    Value* cur = root;
    for (const Value& key : path.keys) {
        if (!chain) chain = std::make_shared<RefChain>();
        chain->above.push_back({cur, 0});
        if (cur->isArray()) {
            if (!key.isNumeric()) { err = "array index must be a number"; return nullptr; }
            int64_t i = (int64_t)key.asNumber();
            auto& a = std::get<Value::array_type>(cur->data);
            if (i < 0 || i >= (int64_t)a.size()) { err = "array index " + std::to_string(i) + " out of bounds"; return nullptr; }
            cur = &a[(size_t)i];
        } else if (cur->isMap()) {
            if (!key.isString()) { err = "map key must be string"; return nullptr; }
            auto& m = std::get<Value::map_type>(cur->data);
            auto it = m.find(key.asString());
            if (it == m.end()) { err = "key not found: " + key.asString(); return nullptr; }
            cur = &it->second;
        } else {
            err = "cannot take a reference into " + cur->typeName();
            return nullptr;
        }
        chain->above.back().childDepth = cur->nestDepth;
    }
    return cur;
}

inline Value makeRef(Value* target, std::shared_ptr<RefChain> chain) {
    return Value(TypedPtr(target, kRefTag, std::move(chain)));
}

// for natives: the Value an in-out parameter points at. `incoming` is the nestDepth of anything the native is about
// to put into it, so a reference deep inside a structure can't push the whole thing past the depth limit
inline Value& inoutTarget(const Value& ref, uint32_t incoming = 0) {
    if (!isRef(ref)) raiseError("value", "internal error: in-out parameter without a reference");
    const TypedPtr& p = ref.asPointer();
    size_t above = p.owner ? static_cast<RefChain*>(p.owner.get())->above.size() : 0;
    if ((size_t)incoming + 1 + above > Value::kMaxValueDepth)
        raiseError("value", "value nested too deeply (max depth " + std::to_string(Value::kMaxValueDepth) + ")");
    return *static_cast<Value*>(p.ptr);
}

// brings the nestDepth of the containers in `chain` up to date once the value under them (now `childDepth` deep) has changed
inline void settleChain(const RefChain& chain, uint32_t childDepth) {
    for (size_t i = chain.above.size(); i-- > 0; ) {
        Value* c = chain.above[i].container;
        c->noteReplaced(chain.above[i].childDepth, childDepth);
        childDepth = c->nestDepth;
    }
}

// after a call: the containers above each reference's target get their nestDepth brought up to date
inline void settleRefs(const std::vector<Value>& args) {
    for (const Value& a : args) {
        if (!isRef(a)) continue;
        const TypedPtr& p = a.asPointer();
        if (p.owner) settleChain(*static_cast<RefChain*>(p.owner.get()), static_cast<Value*>(p.ptr)->nestDepth);
    }
}

// Value::map_type is unordered_map (hash order, unspecified/unstable across runs);
// callers that need reproducible iteration (formatAsString, keys()/values()/globals())
// go through this instead of iterating the map directly
inline std::vector<std::string> sortedMapKeys(const Value::map_type& m) {
    std::vector<std::string> keys;
    keys.reserve(m.size());
    for (const auto& [k, v] : m) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// a plain lexical scope layer: a name -> Value map. always held through a shared_ptr, never by value, so a
// closure that captures a layer shares the same live object (see CaptureFrame below)
using Scope = std::unordered_map<std::string, Value>;

// a closure's captured environment: an ordered list of the actual live Scope objects the defining code had on its
// scope stack when the closure was made (outermost first, innermost last). a name found in more than one layer
// resolves to the innermost. the objects are shared, never copied, so a variable in an enclosing scope keeps
// resolving live, repeated calls and sibling closures see each other's changes, and an exported closure keeps its
// module's top scope alive. the VM keeps a frame's locals in plain slots and only builds a Scope once a closure
// captures it (see CallFrame::locals in vm.h). more on the wiki, scoping-and-closures
struct CaptureFrame {
    std::vector<std::shared_ptr<Scope>> layers;

    // innermost-first lookup across every captured layer. returns a
    // pointer into the live shared layer (not a copy), so *find(name) = v changes the object every other
    // reference to that layer sees. that is the whole point. null if name isn't in any captured layer
    Value* find(const std::string& name) const {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            auto f = (*it)->find(name);
            if (f != (*it)->end()) return &f->second;
        }
        return nullptr;
    }
};

// what `try ... catch e ... end` binds e to, on both backends: a map, so a script can tell an assertion failure
// from a type error without matching message text. e["message"] gives the plain text
//   message: the raw string given to raiseError()/error()
//   kind:    raiseError()'s first argument ("assert", "error", "runtime", "value", "parser", "vm", or a plugin's
//            own tag like "ffi_call")
//   line:    the source line, or 0 if unknown (embr has no nil type, see the header of
//            plugins/embrtypes/embrtypes.cpp, so 0 means "no line", same as SourceRange)
inline Value errorToMap(const EmbrError& e) {
    Value::map_type m;
    m["message"] = Value(e.message);
    m["kind"]    = Value(e.context);
    m["line"]    = Value((double)(e.hasLocation ? e.range.startLine : 0));
    // call stack, innermost first: [{"fn": name, "file": caller's file, "line": call-site line}, ...];
    // line 0 means "called from native code" (see TraceFrame). empty for an error raised at top level
    Value::array_type trace;
    for (const auto& f : e.trace) {
        Value::map_type fm;
        fm["fn"]   = Value(f.fn);
        fm["file"] = Value(f.file);
        fm["line"] = Value((int64_t)f.line);
        trace.push_back(Value(std::move(fm)));
    }
    m["trace"] = Value(std::move(trace));
    return Value(std::move(m));
}

} // namespace embr

#endif // EMBR_CORE_VALUE_H
