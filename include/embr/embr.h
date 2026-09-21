#ifndef EMBR_CPP_INCLUDED
#define EMBR_CPP_INCLUDED


//   EMBR_PLUGIN { interp->bind("myfn", ...); }
#define EMBR_PLUGIN \
    extern "C" void embr_register(embr::Interpreter *interp)

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <variant>
#include <functional>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cctype>
#include <stdexcept>
#include <memory>
#include <algorithm>
#include <charconv>
#include <system_error>
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <numeric>
#include <set>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace embr {

// cxxdroid ships an old libc++ without double from_chars
struct parse_result { const char* ptr; std::errc ec; };

inline parse_result parse_double(const char* first, const char* last, double& value) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611
    auto res = std::from_chars(first, last, value);
    return {res.ptr, res.ec};
#else
    char* end;
    value = std::strtod(first, &end);
    if (end == first) return {first, std::errc::invalid_argument};
    return {end, std::errc{}};
#endif
}

static const char* R = "\033[31m", *Y = "\033[33m", *X = "\033[0m";

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN

   inline void enableAnsi()
   {
       HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
       if (h == INVALID_HANDLE_VALUE)
           return;
   
       DWORD mode;
       if (!GetConsoleMode(h, &mode))
           return;
   
       SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
   }

   using PluginHandle = HMODULE;
   inline PluginHandle pluginOpen (const char *p)                { return LoadLibraryA(p); }
   inline void        *pluginSym  (PluginHandle h, const char *s){ return (void*)GetProcAddress(h, s); }
   inline void         pluginClose(PluginHandle h)               { FreeLibrary(h); }
   inline std::string  pluginError()                             { return "LoadLibrary error " + std::to_string(GetLastError()); }
   static constexpr const char *PLUGIN_EXT = ".dll";
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
   using PluginHandle = void*;
   inline PluginHandle pluginOpen (const char *p)                { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
   inline void        *pluginSym  (PluginHandle h, const char *s){ return dlsym(h, s); }
   inline void         pluginClose(PluginHandle h)               { dlclose(h); }
   inline std::string  pluginError()                             { const char *e = dlerror(); return e ? e : "unknown error"; }
#  if defined(__APPLE__)
   static constexpr const char *PLUGIN_EXT = ".dylib";
#  else
   static constexpr const char *PLUGIN_EXT = ".so";
#  endif
#else
#  error "embr: no plugin loading backend for this platform"
#endif

// split source in lines for error
class SourceMap {
public:
    SourceMap() = default;
    explicit SourceMap(const std::string& src) { load(src); }

    void load(const std::string& src) {
        lines_.clear();
        size_t start = 0, end = src.find('\n');
        while (end != std::string::npos) {
            lines_.push_back(src.substr(start, end - start));
            start = end + 1;
            end   = src.find('\n', start);
        }
        lines_.push_back(src.substr(start));
    }

    const std::string& get(int line) const {
        static const std::string empty;
        if (line < 1 || line > static_cast<int>(lines_.size())) return empty;
        return lines_[line - 1];
    }

    bool empty() const { return lines_.empty(); }

private:
    std::vector<std::string> lines_;
};

enum class TokenType {
    Identifier, Number, String,
    LParen, RParen,  LBracket, RBracket, LCurly, RCurly,
    Colon,  Comma,   Dot,
    Plus,   Minus,   Star,   Slash,
    PlusEq, MinusEq, StarEq, SlashEq,
    Percent,
    Equal,  EqualEqual,
    Greater, Less, GreaterEqual, LessEqual,
    Bang, BangEqual,
    LArrow, RArrow,
    And, Or,
    If, Else, End, Fn, Return, While, Import, Local,
    Eof
};

struct Token {
    TokenType   type;
    std::string text;
    int         line, col;
};

struct SourceRange {
    int startLine = 0, startCol = 0;
    int endLine   = 0, endCol   = 0;

    SourceRange() = default;
    SourceRange(int sl, int sc, int el, int ec)
        : startLine(sl), startCol(sc), endLine(el), endCol(ec) {}

    bool valid() const { return startLine > 0; }

    static SourceRange fromToken(const Token& tok) {
        return {tok.line, tok.col, tok.line, tok.col + (int)tok.text.size()};
    }

    static SourceRange merge(const SourceRange& a, const SourceRange& b) {
        if (!a.valid()) return b;
        if (!b.valid()) return a;
        int sl = std::min(a.startLine, b.startLine);
        int sc = (a.startLine == b.startLine) ? std::min(a.startCol, b.startCol)
                                               : (a.startLine < b.startLine ? a.startCol : b.startCol);
        int el = std::max(a.endLine, b.endLine);
        int ec = (a.endLine == b.endLine) ? std::max(a.endCol, b.endCol)
                                           : (a.endLine > b.endLine ? a.endCol : b.endCol);
        return {sl, sc, el, ec};
    }
};

struct EmbrError : std::runtime_error {
    bool        hasLocation;
    SourceRange range;
    explicit EmbrError(const std::string& msg, bool loc = false, SourceRange r = {})
        : std::runtime_error(msg), hasLocation(loc), range(r) {}
};

[[noreturn]] inline void raiseError(
    const std::string& context,
    const std::string& msg,
    const SourceRange& range,
    const SourceMap&   src)
{
    std::ostringstream out;
    out << Y << "[" << context << "] " << X << msg;
    if (range.valid())
        out << Y << " at line " << range.startLine << ", col " << range.startCol << X;
    out << "\n";
    if (range.valid()) {
        const std::string& ln = src.get(range.startLine);
        if (!ln.empty()) {
            out << "  " << ln << "\n  "
                << std::string(std::max(0, range.startCol - 1), ' ')
                << R << std::string(std::max(1, range.endCol - range.startCol), '^') << X
                << "\n";
        }
    }
    throw EmbrError(out.str(), range.valid(), range);
}

[[noreturn]] inline void raiseError(const std::string& context, const std::string& msg,
                                    const SourceRange& range = {}) {
    std::ostringstream out;
    out << "[" << context << "] " << msg;
    if (range.valid()) out << " at line " << range.startLine << ", col " << range.startCol;
    out << "\n";
    throw EmbrError(out.str(), range.valid(), range);
}

// fwd declarations
class Interpreter;
struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// Value type bitmask
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

// convenience
namespace TS {
    inline constexpr TypeSet Num  = TypeSet(TypeTag::Number);
    inline constexpr TypeSet Str  = TypeSet(TypeTag::String);
    inline constexpr TypeSet Arr  = TypeSet(TypeTag::Array);
    inline constexpr TypeSet Map  = TypeSet(TypeTag::Map);
    inline constexpr TypeSet Fn   = TypeSet(TypeTag::Callable);
    inline constexpr TypeSet Ptr  = TypeSet(TypeTag::Pointer);
    inline constexpr TypeSet Any  = TypeSet(0xFF);
    //inline           TypeSet Any() { return TypeSet::Any(); };
}

struct Param {
    std::string name;
    TypeSet     type     = TS::Any;  // accepted types
    bool        optional = false;
    bool        variadic = false;    // OwO eatid the remaining arg

    // named constructors 
    static Param req (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, false, false}; }
    static Param opt (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, true,  false}; }
    static Param rest(std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, true,  true }; }
};

// fwd declare
struct CaptureFrame;
struct Value;
inline std::string valueRepr(const Value& v);

// WARNING! dented bullshit for the VM
#ifndef EMBR_CHUNK_TYPE
#  define EMBR_CHUNK_TYPE void
#endif

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
        /*
        switch (data.index()) {
            case 0: return TypeTag::Number;
            case 1: return TypeTag::String;
            case 2: return TypeTag::Array;
            case 3: return TypeTag::Map;
            case 4: return TypeTag::Callable;
        }
        */
        return static_cast<TypeTag>(1 << data.index());  // for less typing
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

// shared_ptr in ScriptFn so all closures created in the same scope share the same frame and see each other's mutations
struct CaptureFrame : std::unordered_map<std::string, Value> {
    using std::unordered_map<std::string, Value>::unordered_map;
};


class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src), map_(src) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        while (!eof()) {
            skipWs();
            if (eof()) break;
            char c = peek();
            if (std::isalpha((unsigned char)c) || c == '_') out.push_back(lexIdent());
            else if (std::isdigit(c))                       out.push_back(lexNum());
            else if (c == '"')                              out.push_back(lexStr());
            else                                            out.push_back(lexSym());
        }
        out.push_back({TokenType::Eof, "", line_, col_});
        return out;
    }

    const SourceMap& sourceMap() const { return map_; }

private:
    const std::string& s_;
    SourceMap          map_;
    size_t pos_  = 0;
    int    line_ = 1, col_ = 1;

    bool eof()  const { return pos_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[pos_]; }
    char get() {
        char c = s_[pos_++];
        if (c == '\n') { ++line_; col_ = 1; } else ++col_;
        return c;
    }

    void skipWs() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') get();
            else if (c == '#') { while (!eof() && peek() != '\n') get(); }
            else break;
        }
    }

    Token lexIdent() {
        int sl = line_, sc = col_; std::string t;
        while (!eof() && (std::isalnum((unsigned char)peek()) || peek() == '_')) t += get();
        static const std::unordered_map<std::string, TokenType> kw = {
            {"if",TokenType::If},        {"else",TokenType::Else},    {"end",TokenType::End},
            {"fn",TokenType::Fn},        {"return",TokenType::Return},{"while",TokenType::While},
            {"import",TokenType::Import},{"local",TokenType::Local},
            {"and",TokenType::And},      {"or",TokenType::Or},
        };
        auto it = kw.find(t);
        return {it != kw.end() ? it->second : TokenType::Identifier, t, sl, sc};
    }

    Token lexNum() {
        int sl = line_, sc = col_; std::string t; bool dot = false;
        while (!eof() && (std::isdigit(peek()) || peek() == '.')) {
            if (peek() == '.') {
                if (dot) raiseError("lexer","multiple decimal points",{sl,sc,line_,col_},map_);
                dot = true;
            }
            t += get();
        }
        if (t.back() == '.')
            raiseError("lexer","trailing decimal point",{sl,sc,line_,col_},map_);
        return {TokenType::Number, t, sl, sc};
    }

    Token lexStr() {
        int sl = line_, sc = col_; get(); std::string t;
        while (!eof()) {
            char c = get();
            if (c == '"') return {TokenType::String, t, sl, sc};
            if (c == '\\') {
                if (eof()) raiseError("lexer","unterminated escape",{sl,sc,line_,col_},map_);
                switch (char nx = get()) {
                    case 'n':t+='\n';break; case 't':t+='\t';break;
                    case 'r':t+='\r';break; case '0':t+='\0';break;
                    case '"':t+='"'; break; case '\\':t+='\\';break;
                    default: raiseError("lexer",std::string("unknown escape \\")+nx,
                                        {sl,sc,line_,col_},map_);
                }
            } else t += c;
        }
        raiseError("lexer","unterminated string literal",{sl,sc,line_,col_},map_);
    }

    Token lexSym() {
        int sl = line_, sc = col_; char c = get();
        switch (c) {
            case '+': if(peek()=='='){get();return{TokenType::PlusEq,      "+=",sl,sc};} return{TokenType::Plus,   "+",sl,sc};
            case '-': if(peek()=='>'){get();return{TokenType::RArrow,      "->",sl,sc};}
                      if(peek()=='='){get();return{TokenType::MinusEq,     "-=",sl,sc};} return{TokenType::Minus,  "-",sl,sc};
            case '*': if(peek()=='='){get();return{TokenType::StarEq,      "*=",sl,sc};} return{TokenType::Star,   "*",sl,sc};
            case '/': if(peek()=='='){get();return{TokenType::SlashEq,     "/=",sl,sc};} return{TokenType::Slash,  "/",sl,sc};
            case '%': return {TokenType::Percent, "%",sl,sc};

            case '(': return {TokenType::LParen,  "(",sl,sc};
            case ')': return {TokenType::RParen,  ")",sl,sc};
            case '[': return {TokenType::LBracket,"[",sl,sc};
            case ']': return {TokenType::RBracket,"]",sl,sc};
            case '{': return {TokenType::LCurly,  "{",sl,sc};
            case '}': return {TokenType::RCurly,  "}",sl,sc};

            case ',': return {TokenType::Comma,   ",",sl,sc};
            case '.': return {TokenType::Dot,     ".",sl,sc};
            case ':': return {TokenType::Colon,   ":",sl,sc};

            case '=': if(peek()=='='){get();return{TokenType::EqualEqual,  "==",sl,sc};} return{TokenType::Equal,  "=",sl,sc};
            case '>': if(peek()=='='){get();return{TokenType::GreaterEqual,">=",sl,sc};} return{TokenType::Greater,">",sl,sc};
            case '<': if(peek()=='='){get();return{TokenType::LessEqual,   "<=",sl,sc};} return{TokenType::Less,   "<",sl,sc};
            case '!': if(peek()=='='){get();return{TokenType::BangEqual,   "!=",sl,sc};} return{TokenType::Bang,   "!",sl,sc};

        }
        raiseError("lexer", std::string("unexpected character: ")+c, {sl,sc,line_,col_}, map_);
    }
};


struct Expr {
    enum class Kind { Number, String, Var, Unary, Binary, Call, Array, Map, Index, Lambda, And, Or };
    Kind        kind;
    SourceRange range;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};

struct NumberExpr : Expr { double      v; NumberExpr(double      v,SourceRange r):Expr(Kind::Number),v(v)           {range=r;} };
struct StringExpr : Expr { std::string v; StringExpr(std::string v,SourceRange r):Expr(Kind::String),v(std::move(v)){range=r;} };
struct VarExpr    : Expr { std::string name; VarExpr(std::string n,SourceRange r):Expr(Kind::Var),name(std::move(n)){range=r;} };

struct UnaryExpr  : Expr { std::string op;      ExprPtr right;            UnaryExpr():Expr(Kind::Unary) {} };
struct BinaryExpr : Expr { std::string op;      ExprPtr left,right;      BinaryExpr():Expr(Kind::Binary){} };
struct LogicExpr  : Expr { ExprPtr left, right;                           LogicExpr(Kind k) : Expr(k)   {} };

struct CallExpr   : Expr { ExprPtr callee;      std::vector<ExprPtr> args; CallExpr():Expr(Kind::Call)  {} };
struct ArrayExpr  : Expr { std::vector<ExprPtr> elements;                 ArrayExpr():Expr(Kind::Array) {} };
struct MapExpr    : Expr { std::vector<std::pair<ExprPtr,ExprPtr>> entries; MapExpr():Expr(Kind::Map)   {} };
struct IndexExpr  : Expr { ExprPtr object,index;                          IndexExpr():Expr(Kind::Index) {} };

// body heap-owned unlike FnStmt owned by program vector
struct LambdaExpr : Expr {
    std::vector<Param>          params;
    TypeSet                     returnType = TypeSet::Any();
    const std::vector<StmtPtr>* body;
    std::vector<StmtPtr>        ownedBody;
    LambdaExpr() : Expr(Kind::Lambda) {}
};

struct Stmt {
    enum class Kind { Assign, Local, If, Fn, Return, While, Import, Expr };
    Kind kind; SourceRange range;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};

struct AssignStmt : Stmt { ExprPtr target,value; AssignStmt():Stmt(Kind::Assign){} };
struct LocalStmt  : Stmt { std::string name; ExprPtr value; LocalStmt():Stmt(Kind::Local){} };
struct IfStmt     : Stmt { ExprPtr cond; std::vector<StmtPtr> thenBlock,elseBlock; IfStmt():Stmt(Kind::If){} };
struct FnStmt     : Stmt {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    std::vector<StmtPtr> body;
    FnStmt():Stmt(Kind::Fn){}
};
struct ReturnStmt : Stmt { ExprPtr value; ReturnStmt():Stmt(Kind::Return){} };
struct WhileStmt  : Stmt { ExprPtr cond; std::vector<StmtPtr> body; WhileStmt():Stmt(Kind::While){} };
struct ExprStmt   : Stmt { ExprPtr expr; ExprStmt():Stmt(Kind::Expr){} };
struct ImportStmt : Stmt { std::string path; ImportStmt():Stmt(Kind::Import){} };


class Parser {
public:
    Parser(std::vector<Token> toks, SourceMap src)
        : toks_(std::move(toks)), src_(std::move(src)) {}

    std::vector<StmtPtr> parse() {
        std::vector<StmtPtr> out;
        while (!check(TokenType::Eof)) {
            try { out.push_back(stmt()); }
            catch (const EmbrError& e) { std::cerr << e.what(); sync(); }
            catch (const std::exception& e) { std::cerr << "[parser] " << e.what() << "\n"; sync(); }
        }
        return out;
    }

private:
    std::vector<Token> toks_;
    SourceMap          src_;
    size_t             i_ = 0;

    Token& peek() { return toks_[i_]; }
    Token& prev() { return toks_[i_-1]; }
    bool match(TokenType t) { if (peek().type==t){++i_;return true;} return false; }
    bool check(TokenType t) const { return toks_[i_].type==t; }
    bool checkAt(size_t off, TokenType t) const {
        return i_+off < toks_.size() && toks_[i_+off].type == t;
    }
    Token consume(TokenType t, const char* msg) {
        if (match(t)) return prev();
        raiseError("parser", msg, SourceRange::fromToken(peek()), src_);
    }
    SourceRange tr(const Token& tok) const { return SourceRange::fromToken(tok); }

    void sync() {
        while (!check(TokenType::Eof)) {
            switch (peek().type) {
                case TokenType::If:    case TokenType::Else:   case TokenType::End:
                case TokenType::Fn:    case TokenType::While:  case TokenType::Import:
                case TokenType::Local: case TokenType::Return: return;
                default: ++i_;
            }
        }
    }

    TypeSet parseTypeName(const Token& t) {
        TypeSet ts = TypeSet::fromName(t.text);
        if (ts.isNone())
            raiseError("parser", "unknown type '" + t.text +
                       "' expected: num, str, arr, map, fn, ptr, any", tr(t), src_);
        return ts;
    }
    // name : type
    TypeSet parseTypeAnnotation() {
        if (!match(TokenType::Colon)) return TypeSet::Any();
        Token t = consume(TokenType::Identifier, "expected type name after ':'");
        return parseTypeName(t);
    }

    // caller already eatid (
    std::vector<Param> parseParamList() {
        std::vector<Param> params;
        if (!check(TokenType::RParen)) {
            do {
                Token nm = consume(TokenType::Identifier, "expected parameter name");
                TypeSet mask = parseTypeAnnotation();
                params.push_back(Param::req(nm.text, mask));
            } while (match(TokenType::Comma));
        }
        return params;
    }
    
    // function -> type
    TypeSet parseReturnAnnotation() {
        if (!match(TokenType::RArrow)) return TypeSet::Any();
        Token t = consume(TokenType::Identifier, "expected return type after '->'");
        return parseTypeName(t);
    }

    StmtPtr stmt() {
        if (match(TokenType::If))     return parseIf();
        if (match(TokenType::While))  return parseWhile();
        if (match(TokenType::Return)) return parseReturn();
        if (match(TokenType::Import)) return parseImport();
        if (match(TokenType::Local))  return parseLocal();
        if (match(TokenType::Fn))     return parseFnBlock();

        // name(params) = expr
        // IDENT ( [IDENT {, IDENT}] ) = (not ==)
        if (isFnAssignHead()) return parseFnAssign();

        auto lhs = expr();
        if (match(TokenType::Equal)) {
            auto rhs = expr();
            auto s = std::make_unique<AssignStmt>();
            s->target = std::move(lhs); s->value = std::move(rhs);
            return s;
        }

        // name +=, -=, *=, /= expr
        // \/\/\/
        // name = name +, -, *, / expr
        {
            std::string binOp;
            if      (match(TokenType::PlusEq))  binOp = "+";
            else if (match(TokenType::MinusEq)) binOp = "-";
            else if (match(TokenType::StarEq))  binOp = "*";
            else if (match(TokenType::SlashEq)) binOp = "/";
            if (!binOp.empty()) {
                auto rhs = expr();
                auto bin = std::make_unique<BinaryExpr>();
                bin->op    = binOp;
                bin->range = SourceRange::merge(lhs->range, rhs->range);
                if (lhs->kind == Expr::Kind::Var) {
                    auto* v = static_cast<VarExpr*>(lhs.get());
                    bin->left  = std::make_unique<VarExpr>(v->name, v->range);
                    bin->right = std::move(rhs);
                    auto s = std::make_unique<AssignStmt>();
                    s->target  = std::move(lhs);
                    s->value   = std::move(bin);
                    return s;
                }
                raiseError("parser", "compound assignment target must be a simple variable",
                           lhs->range, src_);
            }
        }
        auto s = std::make_unique<ExprStmt>();
        s->range = lhs->range; s->expr = std::move(lhs);
        return s;
    }

    // scan tokens without consuming owo to detect fn assign
    bool isFnAssignHead() const {
        if (!checkAt(0, TokenType::Identifier)) return false;
        if (!checkAt(1, TokenType::LParen))     return false;
        size_t j = i_ + 2;
 
        auto skipParam = [&]() -> bool {
            // ident
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j;
            // : type
            if (j < toks_.size() && toks_[j].type == TokenType::Colon) {
                ++j; // skip :
                if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
                ++j; // skip type
            }
            return true;
        };
 
        // f() = ...
        if (j < toks_.size() && toks_[j].type == TokenType::RParen) {
            ++j;
            return j < toks_.size() && toks_[j].type == TokenType::Equal &&
                   (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
        }
        // params
        if (!skipParam()) return false;
        while (j < toks_.size() && toks_[j].type == TokenType::Comma) {
            ++j;
            if (!skipParam()) return false;
        }
        if (j >= toks_.size() || toks_[j].type != TokenType::RParen) return false;
        ++j;
        // -> type
        if (j < toks_.size() && toks_[j].type == TokenType::RArrow) {
            ++j; // skip ->
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j; // skip type
        }
        return j < toks_.size() && toks_[j].type == TokenType::Equal &&
               (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
    }

    // name(params) = expr
    //  \/\/\/
    // fn name(params) return expr end
    StmtPtr parseFnAssign() {
        Token nameTok = toks_[i_++];
        SourceRange start = tr(nameTok);
        consume(TokenType::LParen, "expected '('");
        auto params = parseParamList();
        consume(TokenType::RParen, "expected ')'");
        TypeSet retType = parseReturnAnnotation();
        consume(TokenType::Equal,  "expected '='");
        auto bodyExpr = expr();
        // wrap in return stmt
        auto ret   = std::make_unique<ReturnStmt>(); ret->range = bodyExpr->range; ret->value = std::move(bodyExpr);
        auto fn    = std::make_unique<FnStmt>();
        fn->name   = nameTok.text; fn->params = std::move(params); fn->returnType = retType;
        fn->body.push_back(std::move(ret));
        fn->range  = SourceRange::merge(start, fn->body.back()->range);
        return fn;
    }

    // fn name(params) body end
    StmtPtr parseFnBlock() {
        SourceRange start = tr(prev());
        Token name = consume(TokenType::Identifier, "expected function name after 'fn'");
        consume(TokenType::LParen, "expected '('");
        auto params = parseParamList();
        consume(TokenType::RParen, "expected ')'");
        TypeSet retType = parseReturnAnnotation();
        std::vector<StmtPtr> body;
        while (!check(TokenType::End) && !check(TokenType::Eof)) body.push_back(stmt());
        Token endTok = consume(TokenType::End, "expected 'end' to close 'fn'");
        auto fn = std::make_unique<FnStmt>();
        fn->name = name.text; fn->params = std::move(params); fn->returnType = retType;
        fn->body = std::move(body);
        fn->range = SourceRange::merge(start, tr(endTok));
        return fn;
    }

    StmtPtr parseLocal() {
        SourceRange start = tr(prev());
        Token name = consume(TokenType::Identifier, "expected identifier after 'local'");
        consume(TokenType::Equal, "expected '='");
        auto e = expr();
        auto s = std::make_unique<LocalStmt>();
        s->name = name.text; s->range = SourceRange::merge(start, e->range); s->value = std::move(e);
        return s;
    }

    StmtPtr parseIf() {
        SourceRange start = tr(prev());
        auto cond = expr();
        std::vector<StmtPtr> thenB, elseB;
        while (!check(TokenType::Else) && !check(TokenType::End) && !check(TokenType::Eof)) thenB.push_back(stmt());
        if (match(TokenType::Else))
            while (!check(TokenType::End) && !check(TokenType::Eof)) elseB.push_back(stmt());
        Token endTok = consume(TokenType::End, "expected 'end' to close 'if'");
        auto s = std::make_unique<IfStmt>();
        s->cond = std::move(cond); s->thenBlock = std::move(thenB); s->elseBlock = std::move(elseB);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    StmtPtr parseReturn() {
        SourceRange start = tr(prev());
        auto e = expr();
        auto s = std::make_unique<ReturnStmt>();
        s->range = SourceRange::merge(start, e->range); s->value = std::move(e);
        return s;
    }

    StmtPtr parseWhile() {
        SourceRange start = tr(prev());
        auto cond = expr();
        std::vector<StmtPtr> body;
        while (!check(TokenType::End) && !check(TokenType::Eof)) body.push_back(stmt());
        Token endTok = consume(TokenType::End, "expected 'end' to close 'while'");
        auto s = std::make_unique<WhileStmt>();
        s->cond = std::move(cond); s->body = std::move(body);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    StmtPtr parseImport() {
        SourceRange start = tr(prev());
        Token path = consume(TokenType::String, "expected string path after 'import'");
        auto s = std::make_unique<ImportStmt>();
        s->path = path.text; s->range = SourceRange::merge(start, tr(path));
        return s;
    }

    ExprPtr expr(int minPrec = 0) {
        ExprPtr left = unary();
        while (true) {
            if (minPrec <= 0 && check(TokenType::Or)) {
                ++i_;
                auto right = expr(1); // and binds tighter than or
                auto log = std::make_unique<LogicExpr>(Expr::Kind::Or);
                log->range = SourceRange::merge(left->range, right->range);
                log->left  = std::move(left);
                log->right = std::move(right);
                left = std::move(log);
                continue;
            }
            if (minPrec <= 1 && check(TokenType::And)) {
                ++i_;
                auto right = expr(2); // and is right-associative at prec 1
                auto log = std::make_unique<LogicExpr>(Expr::Kind::And);
                log->range = SourceRange::merge(left->range, right->range);
                log->left  = std::move(left);
                log->right = std::move(right);
                left = std::move(log);
                continue;
            }

            int prec = infixPrec(peek().type);
            if (prec < minPrec) break;
            Token op = toks_[i_++];
            auto bin  = std::make_unique<BinaryExpr>();
            bin->op   = op.text;
            bin->left = std::move(left);
            bin->right = expr(prec + 1);
            bin->range = SourceRange::merge(bin->left->range, bin->right->range);
            left = std::move(bin);
        }
        return left;
    }

    ExprPtr unary() {
        if (match(TokenType::Minus) || match(TokenType::Bang)) {
            Token op = prev(); auto right = unary();
            auto u = std::make_unique<UnaryExpr>();
            u->op = op.text; u->right = std::move(right);
            u->range = SourceRange::merge(tr(op), u->right->range);
            return u;
        }
        auto expr_ = primary();
        // handle postfix operators: [ ... ], identifier() and .identifier
        while (true) {
            if (match(TokenType::LParen)) {
                auto call = std::make_unique<CallExpr>();
                call->callee = std::move(expr_);

                if (!check(TokenType::RParen))
                    do {
                        call->args.push_back(expr());
                    } while (match(TokenType::Comma));

                Token rp = consume(TokenType::RParen, "expected ')'");
                call->range = SourceRange::merge(
                    call->callee->range,
                    tr(rp)
                );

                expr_ = std::move(call);
            }
            if (match(TokenType::LBracket)) {
                auto idx = std::make_unique<IndexExpr>();
                idx->object = std::move(expr_);
                idx->index = expr();
                Token rb = consume(TokenType::RBracket, "expected ']'");
                idx->range = SourceRange::merge(idx->object->range, tr(rb));
                expr_ = std::move(idx);
            }
            else if (match(TokenType::Dot)) {
                Token id = consume(TokenType::Identifier, "expected identifier after '.'");
                auto idx = std::make_unique<IndexExpr>();
                idx->object = std::move(expr_);
                // convert the identifier into a string literal expression
                auto keyExpr = std::make_unique<StringExpr>(id.text, SourceRange::fromToken(id));
                idx->index = std::move(keyExpr);
                idx->range = SourceRange::merge(idx->object->range, SourceRange::fromToken(id));
                expr_ = std::move(idx);
            }
            else {
                break;
            }
        }
        return expr_;
    }

    ExprPtr primary() {
        Token tok = toks_[i_++];
        SourceRange r = tr(tok);
        switch (tok.type) {
            case TokenType::Number: {
                double v; auto [ptr,ec] = parse_double(tok.text.data(), tok.text.data()+tok.text.size(), v);
                if (ec != std::errc()) raiseError("parser","invalid number: "+tok.text, r, src_);
                return std::make_unique<NumberExpr>(v, r);
            }
            case TokenType::String:
                return std::make_unique<StringExpr>(tok.text, r);
            case TokenType::Identifier: {
                return std::make_unique<VarExpr>(tok.text, r);
            }
            case TokenType::LParen: {
                auto e = expr(); Token rp = consume(TokenType::RParen, "expected ')'");
                e->range = SourceRange::merge(r, tr(rp)); return e;
            }
            case TokenType::LBracket: {
                auto arr = std::make_unique<ArrayExpr>();
                if (!check(TokenType::RBracket))
                    do { arr->elements.push_back(expr()); }
                    while (match(TokenType::Comma) && !check(TokenType::RBracket));
                Token rb = consume(TokenType::RBracket, "expected ']'");
                arr->range = SourceRange::merge(r, tr(rb)); return arr;
            }
            case TokenType::LCurly: {
                auto map = std::make_unique<MapExpr>();
                if (!check(TokenType::RCurly)) {
                    do {
                        ExprPtr key;
                        if (check(TokenType::Identifier) && checkAt(1, TokenType::Colon)) {
                            Token id = toks_[i_++];
                            key = std::make_unique<StringExpr>(id.text, tr(id));
                            consume(TokenType::Colon, "expected ':'");
                        } else { key = expr(); consume(TokenType::Colon, "expected ':'"); }
                        map->entries.emplace_back(std::move(key), expr());
                    } while (match(TokenType::Comma) && !check(TokenType::RCurly));
                }
                Token rc = consume(TokenType::RCurly, "expected '}'");
                map->range = SourceRange::merge(r, tr(rc)); return map;
            }
            // fn(params) body end
            case TokenType::Fn: {
                consume(TokenType::LParen, "expected '(' after 'fn'");
                auto params = parseParamList();
                consume(TokenType::RParen, "expected ')'");
                TypeSet retType = parseReturnAnnotation();
                auto lam = std::make_unique<LambdaExpr>();
                lam->params = std::move(params);
                lam->returnType = retType;
                while (!check(TokenType::End) && !check(TokenType::Eof))
                    lam->ownedBody.push_back(stmt());
                Token endTok = consume(TokenType::End, "expected 'end' after fn expression body");
                lam->range  = SourceRange::merge(r, tr(endTok));
                return lam;
            }
            default:
                raiseError("parser", "unexpected token '" + tok.text + "'", r, src_);
        }
    }

    int infixPrec(TokenType t) const {
        switch (t) {
            case TokenType::EqualEqual:   case TokenType::BangEqual:
            case TokenType::Greater:      case TokenType::Less:
            case TokenType::GreaterEqual: case TokenType::LessEqual: return 2;
            case TokenType::Plus:         case TokenType::Minus:     return 3;
            case TokenType::Star:         case TokenType::Slash:
            case TokenType::Percent:                                 return 4;

            default: return -1;
        }
    }
};

class Interpreter {
public:
    using NativeFn = Value::NativeFn;

    Interpreter() {
        scopes_.emplace_back();
        bindSig("print", {Param::rest("args", TS::Any)},
        [](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << args[i].formatAsString();
            }
            std::cout << '\n';
            return Value(0.0);
        });
        bindSig("globals", {},
        [this](const std::vector<Value>&) -> Value {
            Value::map_type m;
            for (const auto& [k, v] : this->globals())
                m[k] = v;
            return Value(std::move(m));
        });
    }
    ~Interpreter() {
        // remove anything holding plugin function pointers
        scopes_.clear();

        for (auto h : pluginHandles_)
            pluginClose(h);
    }

    Interpreter(const Interpreter&)            = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    class ScopeGuard {
        Interpreter& i_;
    public:
        explicit ScopeGuard(Interpreter& i) : i_(i) { i_.push(); }
        ~ScopeGuard()                               { i_.pop();  }
    };

    void push() { scopes_.emplace_back(); }
    void pop()  { scopes_.pop_back(); }

    // assign into nearest scope that already holds the name,
    // falling back to the current module floor (not necessarily scopes_[0])
    void set(const std::string& k, Value v) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->count(k)) { (*it)[k] = std::move(v); return; }
        scopes_[moduleBase_][k] = std::move(v);
    }

    // ideally should be wrapped in has() where used to throw a more meaningful error
    Value get(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        raiseError("runtime", "undefined variable: " + name);
    }

    bool has(const std::string& name) const {
        for (auto& sc : scopes_) if (sc.count(name)) return true;
        return false;
    }

    void define(const std::string& k, Value v) { scopes_.back()[k] = std::move(v); }

    // bind nativefn with no sigcheck
    void bind(const std::string& name, NativeFn fn) {
        scopes_.front()[name] = Value::makeNative(name, std::move(fn), {});
    }

    // bind nativefn with sig
    void bindSig(const std::string& name, std::vector<Param> sig, NativeFn fn) {
        scopes_.front()[name] = Value::makeNative(name, std::move(fn), std::move(sig));
    }

    // register scriptfn into innermost scope
    void defineScriptFn(ScriptFn fn) {
        std::string nm = fn.name;
        scopes_.back()[nm] = Value::makeScript(std::move(fn));
    }

    void setSource(const std::string& src,
                   const std::string& filename = "<input>") {
        srcMap_.load(src); currentFile_ = filename;
    }
    void setSource(SourceMap m, const std::string& filename = "<input>") {
        srcMap_ = std::move(m); currentFile_ = filename;
    }
    const SourceMap&     sourceMap() const { return srcMap_; }
    const std::string& currentFile() const { return currentFile_; }

    // exposes C++ variable as getter/setter pair
    // reference must outlive interpreter
    template<typename T>
    void bindVar(const std::string& name, T& ref) {
        // name() -> value
        bind(name, [&ref](const std::vector<Value>&) -> Value {
            return toValue(ref);
        });
        // set_name(val)
        bindSig("set_" + name, {Param::req("val", typeConstraintFor<T>())},
                [&ref](const std::vector<Value>& args) -> Value {
            fromValue(args[0], ref);
            return Value(0.0);
        });
    }

    // capture non-native scopes visible from the current call depth
    // closures capture everything from moduleBase_ upward so they can
    // read/write module globals as well as genuinely-local upvalues
    std::shared_ptr<CaptureFrame> captureLocals() const {
        if (scopes_.size() <= moduleBase_ + 1) return nullptr;
        auto frame = std::make_shared<CaptureFrame>();
        for (size_t i = moduleBase_; i < scopes_.size(); ++i)
            for (const auto& [k, v] : scopes_[i])
                (*frame)[k] = v;
        return frame;
    }

    void pushCapture(const std::shared_ptr<CaptureFrame>& cap) {
        if (cap) scopes_.push_back(*cap);
        else     scopes_.emplace_back();
    }
    // returns the current module's "global" scope (the assignment floor)
    // for the top-level script this is scopes_[0] (the native layer)
    // for an imported module it is the module's own export scope
    const std::unordered_map<std::string, Value>& globals() const { return scopes_[moduleBase_]; }
    // plugin fns, built-ins always scopes_[0]
    const std::unordered_map<std::string, Value>& nativeGlobals() const { return scopes_[0]; }

    const std::vector<std::unordered_map<std::string, Value>>& allScopes() const { return scopes_; }
    const std::vector<StmtPtr>* storeProgram(std::vector<StmtPtr> prog) {
        ownedPrograms_.push_back(std::move(prog));
        return &ownedPrograms_.back();
    }

    std::string               scriptDir;
    std::vector<PluginHandle> pluginHandles_;


    // push a new module scope
    // sets moduleBase_ to the new scope
    size_t pushModuleScope() {
        scopes_.emplace_back();
        size_t idx = scopes_.size() - 1;
        prevModuleBases_.push_back(moduleBase_);
        moduleBase_ = idx;
        moduleLocalSets_.emplace_back();
        return idx;
    }

    struct ModuleScopeResult {
        std::unordered_map<std::string, Value> scope;
        std::set<std::string>                  locals;  // names declared with 'local'
    };

    // pop the current module scope and restores moduleBase_ to the parent
    // returns the scope and its local-name set so the caller can filter exports
    ModuleScopeResult popModuleScope() {
        ModuleScopeResult res;
        res.scope  = std::move(scopes_.back());
        res.locals = std::move(moduleLocalSets_.back());
        scopes_.pop_back();
        moduleBase_ = prevModuleBases_.back();
        prevModuleBases_.pop_back();
        moduleLocalSets_.pop_back();
        return res;
    }

    // mark a name as module-local at the current module top scope
    // called by exec(LocalStmt) when exactly at the module floor
    void markModuleLocal(const std::string& name) {
        if (!moduleLocalSets_.empty())
            moduleLocalSets_.back().insert(name);
    }

    // true when executing statements directly in the module top scope
    bool atModuleTopScope() const {
        return scopes_.size() - 1 == moduleBase_;
    }

    // canonical set of already-loaded module paths (deduplication)
    std::set<std::string> loadedModules_;

private:
    std::deque<std::vector<StmtPtr>> ownedPrograms_;
    std::vector<std::unordered_map<std::string, Value>> scopes_;

    size_t                             moduleBase_ = 0;
    std::vector<size_t>                prevModuleBases_;
    std::vector<std::set<std::string>> moduleLocalSets_;

    SourceMap   srcMap_;
    std::string currentFile_ = "<input>";

    // c++ type to Value
    static Value toValue(double v)             { return Value(v); }
    static Value toValue(float v)              { return Value((double)v); }
    static Value toValue(int v)                { return Value((double)v); }
    static Value toValue(bool v)               { return Value(v); }
    static Value toValue(const std::string& v) { return Value(v); }
    static Value toValue(void* p)              { return Value(p); }

    // Value to c++ variable
    static void fromValue(const Value& v, double& out)      { out = v.asNumber(); }
    static void fromValue(const Value& v, float& out)       { out = (float)v.asNumber(); }
    static void fromValue(const Value& v, int& out)         { out = (int)v.asNumber(); }
    static void fromValue(const Value& v, bool& out)        { out = v.truthy(); }
    static void fromValue(const Value& v, std::string& out) { out = v.asString(); }
    static void fromValue(const Value& v, void*& out)       { out = v.asPointer(); }

    // return typeset for T
    template<typename T> static constexpr TypeSet typeConstraintFor() { return TypeSet::Any(); }
};

// typeConstraintFor<T> specialisations
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<double>()      { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<float>()       { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<int>()         { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<bool>()        { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<std::string>() { return TypeSet(TypeTag::String); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<void*>()       { return TypeSet(TypeTag::Pointer);}

inline std::string valueRepr(const Value& v) {
    if (v.isString()) return '"' + v.asString() + '"';
    return v.formatAsString();
}

struct ReturnSignal { Value value; };

class Runner {
public:
    explicit Runner(Interpreter& interpreter) : interp(interpreter) {}

    void run(const std::vector<StmtPtr>& prog) {
        for (auto& s : prog) {
            try { exec(s.get()); }
            catch (const EmbrError& e) { std::cerr << e.what(); }
        }
    }
    void run(const std::vector<StmtPtr>& prog, const SourceMap& src,
             const std::string& filename = "<input>") {
        interp.setSource(src, filename);
        run(prog);
    }

    // public invoke for embrlib
    Value invoke(const Value& callee, const std::vector<Value>& args,
                 const SourceRange& site = {}) {
        return doInvoke(callee, args, site);
    }

    // public entry point for load_module in embrlib
    void importEmbrModule(const std::string& path, const SourceRange& site) {
        execImportEmbrModule(path, site);
    }

    Interpreter& interp;
private:
    [[noreturn]] void error(const std::string& msg, const SourceRange& r = {}) const {
        raiseError("runtime", msg, r, interp.sourceMap());
    }

    static size_t editDistance(
        const std::string& a,
        const std::string& b,
        size_t maxDist = 3)
    {
        const size_t na = a.size();
        const size_t nb = b.size();

        if (na > nb + maxDist || nb > na + maxDist)
            return maxDist + 1;

        std::vector<std::vector<size_t>> dp(na + 1, std::vector<size_t>(nb + 1));

        for (size_t i = 0; i <= na; ++i)
            dp[i][0] = i;

        for (size_t j = 0; j <= nb; ++j)
            dp[0][j] = j;

        for (size_t i = 1; i <= na; ++i) {
            size_t rowMin = SIZE_MAX;

            for (size_t j = 1; j <= nb; ++j) {
                const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;

                size_t v = std::min({
                    dp[i - 1][j] + 1,        // deletion
                    dp[i][j - 1] + 1,        // insertion
                    dp[i - 1][j - 1] + cost  // substitution
                });

                if (i > 1 && j > 1 &&        // trans
                    a[i - 1] == b[j - 2] &&
                    a[i - 2] == b[j - 1])
                {
                    v = std::min(v, dp[i - 2][j - 2] + 1);
                }

                dp[i][j] = v;
                rowMin = std::min(rowMin, v);
            }

            if (rowMin > maxDist)
                return maxDist + 1;
        }

        return dp[na][nb];
    }
    
    std::string suggest(const std::string& name) const {
        std::string best; size_t bestDist = 3; 
        for (auto& sc : interp.allScopes()) {
            for (auto& [k, _] : sc) {
                size_t d = editDistance(name, k, bestDist);
                if (d < bestDist) { bestDist = d; best = k; }
            }
        }
        return best;
    }

    [[noreturn]] void errorUndefined(const std::string& kind,
                                     const std::string& name,
                                     const SourceRange& r) const {
        std::string msg = "undefined " + kind + ": " + name;
        std::string hint = suggest(name);
        if (!hint.empty()) msg += "\n  did you mean '" + hint + "'?";
        raiseError("runtime", msg, r, interp.sourceMap());
    }


    Value doInvoke(const Value& callee, const std::vector<Value>& args,
                   const SourceRange& site) {
        if (!callee.isCallable())
            error("value is not callable (got " + callee.typeName() + ")", site);
        const auto& c = callee.asCallable();

        if (c.isNative()) {
            if (!c.sig.empty()) {
                checkNativeSig(c.name(), c.sig, args, site);
            }
            try {
                return c.native(args);
            } catch (EmbrError& e) {
                if (!e.hasLocation && site.valid()) {
                    raiseError(c.name(), e.what(), site, interp.sourceMap());
                }
                throw;
            }
        }
        const ScriptFn& fn = c.script;
        if (args.size() < fn.params.size())
            error("'" + fn.name + "' expects " + std::to_string(fn.params.size()) +
                  " args, got " + std::to_string(args.size()), site);
        // typecheck
        for (size_t k = 0; k < fn.params.size() && k < args.size(); ++k) {
            const auto& p = fn.params[k];
            if (p.type.isAny()) continue;
            if (!p.type.contains(args[k].tag()))
                error("argument '" + p.name + "' to '" + fn.name + "': expected " +
                      p.type.name() + " but got " + args[k].typeName(), site);
        }

        // push capture frame first (outermost), then a fresh param scope on top
        // params shadow captured names
        // mutations to captured names inside the body update the body's capture scope (not the original frame)
        if (fn.captured) {
            interp.pushCapture(fn.captured);  // capture layer
            interp.push();                    // param layer
        } else {
            interp.push();                    // just a normal scope
        }
        // use a manual guard that pops the right number of scopes
        struct MultiPop {
            Interpreter& i; int n;
            ~MultiPop() { while (n-- > 0) i.pop(); }
        } guard{interp, fn.captured ? 2 : 1};

        for (size_t k = 0; k < fn.params.size(); ++k)
            interp.define(fn.params[k].name, args[k]);
        Value result(0.0);

        try { for (auto& s : *fn.body) exec(s.get()); }
        catch (ReturnSignal& ret) { result = std::move(ret.value); }
        // enforce return type
        if (!fn.returnType.isAny() && !fn.returnType.contains(result.tag()))
            error("'" + fn.name + "' declared return type " +
                  fn.returnType.name() + " but returned " + result.typeName(), site);
        return result;
    }

    // validates args against a native sig
    void checkNativeSig(const std::string& fname,
                        const std::vector<Param>& sig,
                        const std::vector<Value>& args,
                        const SourceRange& site) {
        // count required params and find variadic slot
        size_t minArgs = 0;
        bool   hasVariadic = false;
        for (const auto& p : sig) {
            if (p.variadic) { hasVariadic = true; break; }
            if (!p.optional) ++minArgs;
        }
        size_t maxArgs = hasVariadic ? SIZE_MAX : sig.size();

        if (args.size() < minArgs) {
            // build person-readable sig
            std::string sig_str = buildSigString(fname, sig);
            error("too few arguments to '" + fname + "': expected " +
                  std::to_string(minArgs) + " but got " +
                  std::to_string(args.size()) + "\n  signature: " + sig_str, site);
        }
        if (args.size() > maxArgs) {
            std::string sig_str = buildSigString(fname, sig);
            error("too many arguments to '" + fname + "': expected at most " +
                  std::to_string(maxArgs) + " but got " +
                  std::to_string(args.size()) + "\n  signature: " + sig_str, site);
        }

        // typecheck pos args
        for (size_t i = 0; i < args.size(); ++i) {
            const Param* p = (i < sig.size() && !sig[i].variadic) ? &sig[i] : nullptr;
            if (!p)
                for (auto it = sig.rbegin(); it != sig.rend(); ++it)
                    if (it->variadic) { p = &*it; break; }
            if (!p || p->type.isAny()) continue;
            if (!p->type.contains(args[i].tag()))
                error("argument '" + p->name + "' to '" + fname + "': expected " +
                      p->type.name() + " but got " + args[i].typeName(), site);
        }
    }

    static std::string buildSigString(const std::string& fname, const std::vector<Param>& sig) {
        std::string s = fname + "(";
        for (size_t i = 0; i < sig.size(); ++i) {
            if (i) s += ", ";
            const auto& p = sig[i];
            if (p.variadic) s += "...";
            s += p.name;
            if (!p.type.isAny()) s += ": " + p.type.name();
            if (p.optional && !p.variadic) s += "?";
        }
        return s + ")";
    }

    Value eval(Expr* e) {
        switch (e->kind) {
            case Expr::Kind::Number: return Value(static_cast<NumberExpr*>(e)->v);
            case Expr::Kind::String: return Value(static_cast<StringExpr*>(e)->v);
            case Expr::Kind::Var: {
                auto* v = static_cast<VarExpr*>(e);
                if (!interp.has(v->name)) errorUndefined("variable", v->name, v->range);
                return interp.get(v->name);
            }
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                Value r = eval(u->right.get());
                if (u->op == "-") return Value(-r.asNumber());
                if (u->op == "!") return Value(!r.truthy());
                error("unknown unary op: " + u->op, u->range);
            }
            case Expr::Kind::Binary: return evalBinary(static_cast<BinaryExpr*>(e));
            case Expr::Kind::Call:   return evalCall(static_cast<CallExpr*>(e));
            case Expr::Kind::Array: {
                auto* a = static_cast<ArrayExpr*>(e);
                Value::array_type vals; vals.reserve(a->elements.size());
                for (auto& el : a->elements) vals.push_back(eval(el.get()));
                return Value(std::move(vals));
            }
            case Expr::Kind::Map: {
                auto* m = static_cast<MapExpr*>(e);
                Value::map_type res;
                for (auto& [kx,vx] : m->entries) {
                    Value k = eval(kx.get());
                    if (!k.isString()) error("map keys must be strings. use str() if the index is an expression", kx->range);
                    res[k.asString()] = eval(vx.get());
                }
                return Value(std::move(res));
            }
            case Expr::Kind::And: {
                auto* log = static_cast<LogicExpr*>(e);
                Value l = eval(log->left.get());
                if (!l.truthy()) return l; 
                return eval(log->right.get());
            }
            case Expr::Kind::Or: {
                auto* log = static_cast<LogicExpr*>(e);
                Value l = eval(log->left.get());
                if (l.truthy()) return l;   
                return eval(log->right.get());
            }
            case Expr::Kind::Lambda: {
                auto* lam = static_cast<LambdaExpr*>(e);
                ScriptFn sf;
                sf.name       = "<lambda>";
                sf.params     = lam->params;
                sf.returnType = lam->returnType;
                sf.sourceFile = interp.currentFile();
                sf.body       = &lam->ownedBody;  // stable: lam is in ownedPrograms_
                sf.captured   = interp.captureLocals();
                return Value::makeScript(std::move(sf));
            }
            case Expr::Kind::Index: return evalIndex(static_cast<IndexExpr*>(e));
        }
        error("unhandled expression kind", e->range);
    }

    Value evalBinary(BinaryExpr* b) {
        Value l = eval(b->left.get()), r = eval(b->right.get());
        const auto& op = b->op;
        if (l.isString()||r.isString()) {
            std::string a=l.formatAsString(), c=r.formatAsString();
            if (op=="+")  return Value(a+c);
        } else if (l.isNumber()&&r.isNumber()) {
            double a=l.asNumber(), c=r.asNumber();
            if (op=="+")  return Value(a+c);
            if (op=="-")  return Value(a-c);
            if (op=="*")  return Value(a*c);
            if (op=="/")  { if(c==0.0) error("division by zero",b->range); return Value(a/c); }
            if (op=="%")  { if(c==0.0) error("modulo by zero",b->range);   return Value(std::fmod(a,c)); }
            if (op==">")  return Value(a>c);
            if (op=="<")  return Value(a<c);
            if (op==">=") return Value(a>=c);
            if (op=="<=") return Value(a<=c);
        }
        if (!(l.isCallable()||r.isCallable())) {
            if (op=="==") return Value(l==r);
            if (op=="!=") return Value(l!=r);
        }
        error("unsupported operator "+op+" between "+l.typeName()+" and "+r.typeName(), b->range);
    }

    Value evalIndex(IndexExpr* idx) {
        Value cont = eval(idx->object.get()), key = eval(idx->index.get());
        if (cont.isString()) {
            int ii = (int)key.asNumber(); const auto& s = cont.asString();
            if (ii<0||ii>=(int)s.size()) error("string index "+std::to_string(ii)+" out of bounds. make sure the index is valid",idx->range);
            return Value(std::string(1,s[ii]));
        }
        if (cont.isMap()) {
            if (!key.isString()) error("map key must be string. use str() if the index is an expression",idx->range);
            const auto& m = cont.asMap(); auto it = m.find(key.asString());
            if (it==m.end()) error("key not found: "+key.asString(),idx->range);
            return it->second;
        }
        if (cont.isArray()) {
            int ii=(int)key.asNumber(); const auto& a=cont.asArray();
            if (ii<0||ii>=(int)a.size()) error("array index "+std::to_string(ii)+" out of bounds. make sure the index is valid",idx->range);
            return a[ii];
        }
        error("cannot index "+cont.typeName(),idx->range);
    }

    Value evalCall(CallExpr* call) {
        Value callee = eval(call->callee.get());

        std::vector<Value> args;
        args.reserve(call->args.size());

        for (auto& a : call->args)
            args.push_back(eval(a.get()));

        return doInvoke(callee, args, call->range);
    }

    void exec(Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::Local: {
                auto* l = static_cast<LocalStmt*>(s);
                interp.define(l->name, eval(l->value.get()));
                // track as module-local so it is excluded from exports
                if (interp.atModuleTopScope())
                    interp.markModuleLocal(l->name);

                break;
            }
            case Stmt::Kind::Assign: execAssign(static_cast<AssignStmt*>(s)); break;
            case Stmt::Kind::If: {
                auto* i = static_cast<IfStmt*>(s);
                Interpreter::ScopeGuard guard(interp);
                auto& blk = eval(i->cond.get()).truthy() ? i->thenBlock : i->elseBlock;
                for (auto& st : blk) exec(st.get());
                break;
            }
            case Stmt::Kind::Fn: {
                auto* f = static_cast<FnStmt*>(s);
                ScriptFn sf;
                sf.name       = f->name;
                sf.params     = f->params;
                sf.returnType = f->returnType;
                sf.definedAt  = f->range;
                sf.sourceFile = interp.currentFile();
                sf.body       = &f->body;
                sf.captured   = interp.captureLocals();
                interp.defineScriptFn(std::move(sf));
                break;
            }
            case Stmt::Kind::Return:
                throw ReturnSignal{eval(static_cast<ReturnStmt*>(s)->value.get())};
            case Stmt::Kind::Expr:
                eval(static_cast<ExprStmt*>(s)->expr.get());
                break;
            case Stmt::Kind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                while (eval(w->cond.get()).truthy()) {
                    Interpreter::ScopeGuard guard(interp);
                    for (auto& st : w->body) exec(st.get());
                }
                break;
            }
            case Stmt::Kind::Import: {
                auto* im = static_cast<ImportStmt*>(s);
                execImport(im->path, im->range);
                break;
            }
        }
    }

    void execAssign(AssignStmt* a) {
        if (a->target->kind == Expr::Kind::Var) {
            interp.set(static_cast<VarExpr*>(a->target.get())->name,
                        eval(a->value.get()));
            return;
        }
        if (a->target->kind == Expr::Kind::Index) {
            writeBack(a->target.get(), eval(a->value.get()), a->range);
            return;
        }
        error("invalid assignment target", a->range);
    }

    void writeBack(Expr* target, Value v, const SourceRange& r) {
        if (target->kind == Expr::Kind::Var) {
            interp.set(static_cast<VarExpr*>(target)->name, std::move(v));
            return;
        }
        if (target->kind == Expr::Kind::Index) {
            auto* idx = static_cast<IndexExpr*>(target);
            Value cont = eval(idx->object.get());
            Value key  = eval(idx->index.get());
            if (cont.isString()) {
                int ii = (int)key.asNumber();
                auto str = cont.asString();
                if (ii < 0 || ii >= (int)str.size())
                    error("string index out of bounds. make sure the index is valid", r);
                auto val = v.asString();
                str.replace(ii, val.size(), val);
                writeBack(idx->object.get(), Value(std::move(str)), r);  // recurse
                return;
            }
            if (cont.isArray()) {
                int ii = (int)key.asNumber();
                auto arr = cont.asArray();
                if (ii < 0 || ii >= (int)arr.size())
                    error("array index out of bounds. make sure the index is valid", r);
                arr[ii] = std::move(v);
                writeBack(idx->object.get(), Value(std::move(arr)), r);  // recurse
                return;
            }
            if (cont.isMap()) {
                if (!key.isString()) error("map key must be string. use str() if the index is an expression", r);
                auto m = cont.asMap();
                m[key.asString()] = std::move(v);
                writeBack(idx->object.get(), Value(std::move(m)), r);    // recurse
                return;
            }
            error("index assignment on " + cont.typeName(), r);
        }
        error("cannot assign to temporary", r);
    }

    void execImport(const std::string& rawPath, const SourceRange& r) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);

        const bool isEmbrModule = (p.extension() == ".embr");

        if (isEmbrModule) {
            execImportEmbrModule(rawPath, r);
        } else {
            execImportPlugin(rawPath, r);
        }
    }

    // import an embr script as a module
    //
    // after the module runs, its non-local top level bindings are exported into the importing scope and the module's scope is deleted
    // multiple imports of the same path are no-ops
    void execImportEmbrModule(const std::string& rawPath, const SourceRange& r) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);

        auto withEmbr = [](fs::path base) -> fs::path {
            if (base.extension() == ".embr") return base;
            return fs::path(base.string() + ".embr");
        };

        std::vector<fs::path> cands;
        if (p.is_absolute()) {
            cands.push_back(withEmbr(p));
        } else if (p.has_parent_path()) {
            fs::path withE = withEmbr(p);
            if (!interp.scriptDir.empty())
                cands.push_back((fs::path(interp.scriptDir) / withE).lexically_normal());
            cands.push_back((fs::current_path() / withE).lexically_normal());
        } else {
            fs::path name = withEmbr(p);
            if (!interp.scriptDir.empty()) {
                fs::path base(interp.scriptDir);
                cands.push_back(base / name);
                cands.push_back(base / "modules" / name);
            }
            cands.push_back(fs::current_path() / name);
            cands.push_back(fs::current_path() / "modules" / name);
        }

        fs::path resolved;
        for (const auto& c : cands) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) { resolved = c; break; }
        }
        if (resolved.empty()) {
            std::string tried;
            for (const auto& c : cands) tried += "\n    " + c.string();
            error("cannot find module \"" + rawPath + "\"\n  tried:" + tried, r);
        }

        std::error_code ec;
        fs::path canon = fs::canonical(resolved, ec);
        if (ec) canon = resolved;
        const std::string canonStr = canon.string();

        // deduplication
        if (interp.loadedModules_.count(canonStr)) return;
        interp.loadedModules_.insert(canonStr);

        // read source
        std::ifstream f(resolved);
        if (!f) error("cannot open module file: " + resolved.string(), r);
        std::string src((std::istreambuf_iterator<char>(f)), {});

        // save interpreter context fields that runSource overwrites
        std::string savedDir  = interp.scriptDir;
        SourceMap   savedMap  = interp.sourceMap();
        std::string savedFile = interp.currentFile();

        interp.scriptDir = resolved.parent_path().string();

        // push a dedicated module scope
        interp.pushModuleScope();

        // parse & run inside the module scope
        try {
            Lexer     lex(src);
            auto      tokens = lex.tokenize();
            SourceMap sm     = lex.sourceMap();
            Parser    parser(std::move(tokens), sm);
            auto      prog   = parser.parse();
            const std::vector<StmtPtr>* stable = interp.storeProgram(std::move(prog));
            run(*stable, sm, resolved.string());
        } catch (...) {
            // pop module scope even on error, then restore context and rethrow
            interp.popModuleScope();  // discard result on error
            interp.scriptDir = savedDir;
            interp.setSource(savedMap, savedFile);
            throw;
        }

        // pop module scope and collect the exported bindings
        auto [moduleScope, moduleLocals] = interp.popModuleScope();

        // restore interpreter context
        interp.scriptDir = savedDir;
        interp.setSource(savedMap, savedFile);

        // export: copy non-local bindings into the importer's current scope
        for (auto& [name, val] : moduleScope) {
            if (!moduleLocals.count(name))
                interp.define(name, std::move(val));
        }
    }

    // import a native plugin (.so / .dll / .dylib)
    void execImportPlugin(const std::string& rawPath, const SourceRange& r) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);

        auto withExt = [&](fs::path base) -> fs::path {
            if (base.extension() == PLUGIN_EXT) return base;
            return fs::path(base.string() + PLUGIN_EXT);
        };

        std::vector<fs::path> cands;

        if (p.is_absolute()) {
            cands.push_back(withExt(p));

        } else if (p.has_parent_path()) {
            fs::path withE = withExt(p);
            if (!interp.scriptDir.empty())
                cands.push_back((fs::path(interp.scriptDir) / withE).lexically_normal());
            cands.push_back((fs::current_path() / withE).lexically_normal());

        } else {
            fs::path name = withExt(p);  // just the filename
            if (!interp.scriptDir.empty()) {
                fs::path base(interp.scriptDir);
                cands.push_back(base / name);                   // next to the script
                cands.push_back(base / "plugins" / name);       // plugins/ subdir
            }
            cands.push_back(fs::current_path() / name);         // cwd
            cands.push_back(fs::current_path() / "plugins" / name); // cwd/plugins/
        }

        PluginHandle handle = nullptr;
        std::string  loadedFrom;
        for (const auto& c : cands) {
            handle = pluginOpen(c.string().c_str());
            if (handle) { loadedFrom = c.string(); break; }
        }

        if (!handle) {
            // list of tried paths for the error message
            std::string tried;
            for (const auto& c : cands) tried += "\n    " + c.string();
            error("cannot load plugin \"" + rawPath + "\": " + pluginError() +
                  "\n  tried:" + tried, r);
        }

        using RegFn = void(*)(Interpreter*);
        auto reg = reinterpret_cast<RegFn>(pluginSym(handle, "embr_register"));
        if (!reg) {
            pluginClose(handle);
            error("'embr_register' not found in \"" + loadedFrom + "\"", r);
        }
        try {
            reg(&interp);
            interp.pluginHandles_.push_back(handle);
        }
        catch (...) {
            pluginClose(handle);
            throw;
        }
        std::cout << "[runtime] loaded plugin: " << loadedFrom << "\n";
    }
};

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

} // namespace embr

#endif // EMBR_CPP_INCLUDED
