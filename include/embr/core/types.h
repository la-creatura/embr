#ifndef EMBR_CORE_TYPES_H
#define EMBR_CORE_TYPES_H

// the embr value-type tag system
// the TypeTag/TypeSet bitmask and the Param struct used to describe native + script function signatures

#include <cstdint>
#include <string>

namespace embr {

enum class TypeTag : uint8_t {
    Number   = 1 << 0,
    String   = 1 << 1,
    Array    = 1 << 2,
    Map      = 1 << 3,
    Callable = 1 << 4,
    Pointer  = 1 << 5,
};

struct TypeSet {
    uint8_t mask;

    constexpr TypeSet()                    : mask(0)              {}
    constexpr explicit TypeSet(uint8_t m)  : mask(m)              {}
    constexpr TypeSet(TypeTag t)           : mask((uint8_t)t)     {}

    static constexpr TypeSet Any()         { return TypeSet(0xFF); }
    static constexpr TypeSet None()        { return TypeSet(0x00); }
    static constexpr TypeSet of(TypeTag t) { return TypeSet(t);    }

    constexpr TypeSet operator|(TypeSet  o) const { return TypeSet(uint8_t(mask | o.mask)); }
    constexpr TypeSet operator|(TypeTag  t) const { return TypeSet(uint8_t(mask | (uint8_t)t)); }
    constexpr bool contains(TypeTag t)      const { return mask & (uint8_t)t; }
    constexpr bool isAny()                  const { return mask == 0xFF; }
    constexpr bool isNone()                 const { return mask == 0; }
    constexpr bool operator==(TypeSet o)    const { return mask == o.mask; }
    constexpr bool operator!=(TypeSet o)    const { return mask != o.mask; }

    std::string name() const {
        if (isAny()) return "any";
        struct Row { TypeTag tag; const char* nm; };
        static constexpr Row kTypes[] = {
            {TypeTag::Number,   "number"},
            {TypeTag::String,   "string"},
            {TypeTag::Array,    "array"},
            {TypeTag::Map,      "map"},
            {TypeTag::Callable, "function"},
            {TypeTag::Pointer,  "pointer"}
        };
        std::string s;
        for (auto& r : kTypes)
            if (contains(r.tag)) { if (!s.empty()) s += '|'; s += r.nm; }
        return s.empty() ? "?" : s;
    }

    static TypeSet fromName(const std::string& n) {
        if (n == "num" || n == "number")                  return TypeSet(TypeTag::Number);
        if (n == "str" || n == "string")                  return TypeSet(TypeTag::String);
        if (n == "arr" || n == "array")                   return TypeSet(TypeTag::Array);
        if (n == "map")                                   return TypeSet(TypeTag::Map);
        if (n == "fn"  || n == "func" || n == "function") return TypeSet(TypeTag::Callable);
        if (n == "ptr" || n == "pointer")                 return TypeSet(TypeTag::Pointer);
        if (n == "any")                                   return TypeSet::Any();
        return TypeSet::None();
    }
};

namespace TS {
    inline constexpr TypeSet Num  = TypeSet(TypeTag::Number);
    inline constexpr TypeSet Str  = TypeSet(TypeTag::String);
    inline constexpr TypeSet Arr  = TypeSet(TypeTag::Array);
    inline constexpr TypeSet Map  = TypeSet(TypeTag::Map);
    inline constexpr TypeSet Fn   = TypeSet(TypeTag::Callable);
    inline constexpr TypeSet Ptr  = TypeSet(TypeTag::Pointer);
    inline constexpr TypeSet Any  = TypeSet(0xFF);
}

struct Param {
    std::string name;
    TypeSet     type     = TS::Any;
    bool        optional = false;
    bool        variadic = false;

    static Param req (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, false, false}; }
    static Param opt (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, true,  false}; }
    static Param rest(std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, true,  true }; }
};

} // namespace embr

#endif // EMBR_CORE_TYPES_H
