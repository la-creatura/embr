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

namespace embr {

struct ScriptFn {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    const std::vector<StmtPtr>* body = nullptr;  // Interpreter owns; the tree-walker backend executes this directly
    SourceRange        definedAt;
    std::string        sourceFile;
    std::shared_ptr<CaptureFrame> captured;
    // type-erased embr::vm::CompiledChunk*
    // set only when the vm's Compiler compiled this function and null otherwise
    // core/value.h can't name vm::CompiledChunk directly
    // backends/vm.h is a peer header, not a dependency of core/
    // so callers that need it go through std::static_pointer_cast<vm::CompiledChunk>(compiledChunk) themselves
    // see core/invoke.h for the backend-agnostic dispatch this exists for
    std::shared_ptr<void> compiledChunk;
};

// a plugin-typed opaque pointer/userdata handle
struct TypedPtr {
    void*                 ptr = nullptr;
    std::string            type;   // empty = untyped/raw
    std::shared_ptr<void>  owner;  // non-null only for makeUserdata()-created handles

    TypedPtr() = default;
    explicit TypedPtr(void* p) : ptr(p) {}
    TypedPtr(void* p, std::string t) : ptr(p), type(std::move(t)) {}
    TypedPtr(void* p, std::string t, std::shared_ptr<void> own)
        : ptr(p), type(std::move(t)), owner(std::move(own)) {}

    // same address counts as equal regardless of tag/ownership
    bool operator==(const TypedPtr& o) const { return ptr == o.ptr; }
    bool operator!=(const TypedPtr& o) const { return ptr != o.ptr; }
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

    Value()                     : data(0.0)           {}
    explicit Value(double v)    : data(v)             {}
    explicit Value(int_type v)  : data(v)             {}
    // constrained to an exact bool
    // so a stray raw pointer passed to Value(...) fails to compile via pointer-to-bool conversion instead of silently constructing Value(true)
    template<class B, typename = std::enable_if_t<std::is_same_v<B, bool>>>
    explicit Value(B b)         : data(b ? 1.0 : 0.0) {}
    Value(const std::string& v) : data(v)             {}
    Value(std::string&& v)      : data(std::move(v))  {}
    Value(array_type v)         : data(std::move(v))  {}
    Value(map_type v)           : data(std::move(v))  {}
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
                std::string s = "{"; bool first = true;
                for (auto& [k,val] : asMap()) {
                    if (!first) s += ", ";
                        first = false;
                    s += k + ": " + valueRepr(val);
                }
                return s + "}";
            }
            case TypeTag::Pointer: {
                const auto& p = asPointer();
                std::ostringstream oss;
                oss << "<ptr";
                if (!p.type.empty()) oss << ":" << p.type;
                oss << " 0x" << std::hex << reinterpret_cast<uintptr_t>(p.ptr) << ">";
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
                case TypeTag::Callable: raiseError("value","callable comparison unsupported"); break;
                case TypeTag::Pointer:  return asPointer() == o.asPointer();
            }
        }
        // cross-subtype numeric equality (5 == 5.0) falls out of the general string-formatting fallback below, same as any other mismatched-tag comparison
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

inline std::string valueRepr(const Value& v) {
    if (v.isString()) return '"' + v.asString() + '"';
    return v.formatAsString();
}

// shared_ptr in ScriptFn so all closures created in the same scope share the same frame and see each other's mutations
struct CaptureFrame : std::unordered_map<std::string, Value> {
    using std::unordered_map<std::string, Value>::unordered_map;
};

} // namespace embr

#endif // EMBR_CORE_VALUE_H
