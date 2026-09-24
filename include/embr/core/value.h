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

// WARNING! dented bullshit for the VM
#ifndef EMBR_CHUNK_TYPE
#  define EMBR_CHUNK_TYPE void
#endif

namespace embr {

struct ScriptFn {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    const std::vector<StmtPtr>* body = nullptr;  // Interpreter owns
    SourceRange        definedAt;
    std::string        sourceFile;
    std::shared_ptr<CaptureFrame> captured;
    std::shared_ptr<EMBR_CHUNK_TYPE> compiledChunk; // null for Interpreter. set by VM Compiler
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
    using ptr_type   = void*;

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

    std::variant<double, std::string, array_type, map_type, fn_type, void*> data;

    Value()                     : data(0.0)           {}
    explicit Value(double v)    : data(v)             {}
    explicit Value(bool b)      : data(b ? 1.0 : 0.0) {}
    Value(const std::string& v) : data(v)             {}
    Value(std::string&& v)      : data(std::move(v))  {}
    Value(array_type v)         : data(std::move(v))  {}
    Value(map_type v)           : data(std::move(v))  {}
    Value(fn_type v)            : data(std::move(v))  {}
    explicit Value(ptr_type p)  : data(p)             {}

    static Value makeNative(std::string name, NativeFn fn,
                            std::vector<Param> sig = {}) {
        return Value(std::make_shared<Callable>(
            Callable::fromNative(std::move(name), std::move(fn), std::move(sig))));
    }
    static Value makeScript(ScriptFn fn) {
        return Value(std::make_shared<Callable>(Callable::fromScript(std::move(fn))));
    }

    TypeTag tag() const {
        // variant index order must match TypeTag bit order for switch and bitshift hack
        return static_cast<TypeTag>(1 << data.index());
    }

    bool isNumber()   const { return std::holds_alternative<double>(data);      }
    bool isString()   const { return std::holds_alternative<std::string>(data); }
    bool isArray()    const { return std::holds_alternative<array_type>(data);  }
    bool isMap()      const { return std::holds_alternative<map_type>(data);    }
    bool isCallable() const { return std::holds_alternative<fn_type>(data);     }
    bool isPointer()  const { return std::holds_alternative<ptr_type>(data);    }

    TypeSet typeSet() const { return TypeSet(tag()); }

    // ideally these would only produce errors when something went very, very terribly wrong
    double             asNumber()   const { if (!isNumber())   raiseError("value","expected number, got "  +typeName()); return std::get<double>(data); }
    const std::string& asString()   const { if (!isString())   raiseError("value","expected string, got "  +typeName()); return std::get<std::string>(data); }
    const array_type&  asArray()    const { if (!isArray())    raiseError("value","expected array, got "   +typeName()); return std::get<array_type>(data); }
    const map_type&    asMap()      const { if (!isMap())      raiseError("value","expected map, got "     +typeName()); return std::get<map_type>(data); }
    const Callable&    asCallable() const { if (!isCallable()) raiseError("value","expected function, got "+typeName()); return *std::get<fn_type>(data); }
    const ptr_type&    asPointer()  const { if (!isPointer())  raiseError("value","expected pointer, got " +typeName()); return std::get<ptr_type>(data); }

    std::string typeName() const { return TypeSet(tag()).name(); }

    std::string  formatAsString()   const {
        switch (tag()) {
            case TypeTag::Number:   return formatNumber(this->asNumber());
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
                std::ostringstream oss;
                oss << "<ptr 0x" << std::hex << reinterpret_cast<uintptr_t>(asPointer()) << ">";
                return oss.str();
            }
        }
        return "<unknown>";
    }

    bool operator==(Value o) const {
        if (typeSet() == o.typeSet()) {
            switch (tag()) {
                case TypeTag::Number: return asNumber() == o.asNumber();
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
                case TypeTag::Pointer:  return asPointer()  == o.asPointer();
            }
        }
        return formatAsString() == o.formatAsString();
    }

    bool operator!=(Value o) const { return !(*this == o); }

    // 0.0, "", [], {} are falsy
    // functions truthy
    bool truthy() const {
        switch (tag()) {
            case TypeTag::Number:   return  std::get<double>(data) != 0.0;
            case TypeTag::String:   return !std::get<std::string>(data).empty();
            case TypeTag::Array:    return !std::get<array_type>(data).empty();
            case TypeTag::Map:      return !std::get<map_type>(data).empty();
            case TypeTag::Callable: return true;
            case TypeTag::Pointer:  return std::get<void*>(data) != nullptr;
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
