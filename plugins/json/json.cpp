// json.cpp
// JSON plugin for embr
//
// usage
//   import "json"
//   v = json_parse("{\"a\": [1,2,3], \"b\": true}")
//   print(v["a"][1])
//   print(json_stringify(v))         # compact
//   print(json_stringify(v, 2))      # pretty-printed, 2-space indent
//   print(json_valid("{not json"))   # 0
//
// embr has no bool or nil type (on purpose, see plugins/embrtypes/embrtypes.cpp, which adds both as typed pointers).
// mapping used here:
//   JSON null   -> a "nil" typed pointer, null (see makeNil() below; falsy)
//   JSON true   -> a "bool" typed pointer, a fixed non-null sentinel (see makeBool() below; truthy)
//   JSON false  -> a "bool" typed pointer, null (falsy)
//   JSON number -> Value(double) if it has a '.' or exponent, else Value(int64) exactly
//   JSON string -> Value(string)
//   JSON array  -> Value(array_type)
//   JSON object -> Value(map_type)   (JSON object keys are always strings, matches embr maps)
//
// the "bool"/"nil" tags are the same convention embrtypes.cpp uses (null pointer = false, a fixed non-null
// sentinel = true, a null pointer with the "nil" tag = nil). a Value from either plugin is recognized by the other
// because they share tag strings, not a compile-time link. true also uses the same fixed sentinel address as
// embrtypes.h (a literal, identical across .so files), so a true from either plugin compares equal.
// keep the two in sync if this representation ever changes

#include <embr/embr.h>
#include "../vec/vec.h"
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <charconv>

using namespace embr;

static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

// see the "bool"/"nil" tag convention note in the file header comment above.
// the same fixed literal sentinel address as embrtypes.h's g_truthySentinel
// (not `&someLocalStatic`, which would be a different address in this .so
// than in embrtypes.so), see that header's own comment for why.
static void* const trueSentinel = reinterpret_cast<void*>(1);
static Value makeBool(bool b) { return Value::makePointer(b ? trueSentinel : nullptr, "bool"); }
static bool  isBool(const Value& v) { return v.isPointer() && v.asPointer().type == "bool"; }

static Value makeNil() { return Value::makePointer(nullptr, "nil"); }
static bool  isNil(const Value& v) { return v.isPointer() && v.asPointer().type == "nil"; }

namespace {

// length (1-4) of the valid UTF-8 sequence starting at s[i], or 0 if the bytes there are not valid UTF-8
// (stray continuation byte, truncated sequence, overlong form, surrogate half, or above U+10FFFF).
// same rules as plugins/unicode/unicode.cpp's decodeOne, plugins share conventions by copy, not by a
// runtime dependency, so keep the two in sync if either changes.
size_t utf8SeqLen(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t extra;
    unsigned cp;
    if      (c < 0x80)           return 1;
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0;
    if (i + extra >= s.size()) return 0;                  // truncated sequence
    for (size_t k = 1; k <= extra; ++k) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    static const unsigned minForLen[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < minForLen[extra] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;
    return extra + 1;
}

// recursive-descent JSON parser over a std::string, in the style of embr's own Lexer/Parser
// parseValue -> parseObject/parseArray -> parseValue recurses once per nesting level of the input. json_parse's
// input is untrusted, and a deeply nested payload ("[[[[...]]]]" hundreds of thousands deep) would overflow the
// C++ stack and kill the process where try/catch can't help. so recursion depth is capped explicitly
static constexpr int kMaxJsonDepth = 1000;

struct JsonParser {
    const std::string& s;
    size_t             pos = 0;
    std::string        context;   // native fn name, for error messages
    int                depth = 0;
    bool               strictUtf8 = false;   // json_parse(text, {"strict_utf8": 1}): see parseString

    JsonParser(const std::string& src, std::string ctx)
        : s(src), context(std::move(ctx)) {}

    [[noreturn]] void fail(const std::string& msg) {
        // 1-based line:column (column counted in bytes) plus the raw offset
        size_t end = pos < s.size() ? pos : s.size();
        size_t line = 1, lineStart = 0;
        for (size_t i = 0; i < end; ++i)
            if (s[i] == '\n') { ++line; lineStart = i + 1; }
        raiseError(context, msg + " at line " + std::to_string(line) +
                   ", column " + std::to_string(end - lineStart + 1) +
                   " (position " + std::to_string(pos) + ")");
    }

    struct DepthGuard {
        JsonParser& p;
        DepthGuard(JsonParser& p_, const char* what) : p(p_) {
            if (++p.depth > kMaxJsonDepth)
                p.fail(std::string("input nested too deeply (max depth ") +
                       std::to_string(kMaxJsonDepth) + ") while parsing " + what);
        }
        ~DepthGuard() { --p.depth; }
    };

    bool eof()  const { return pos >= s.size(); }
    char peek() const { return eof() ? '\0' : s[pos]; }
    char get()        { return s[pos++]; }

    void skipWs() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    bool consumeLit(const char* lit) {
        size_t n = std::strlen(lit);
        if (s.compare(pos, n, lit) == 0) { pos += n; return true; }
        return false;
    }

    Value parseValue() {
        DepthGuard guard(*this, "a nested value");
        skipWs();
        if (eof()) fail("unexpected end of input");
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == 't') { if (consumeLit("true"))  return makeBool(true);  fail("invalid literal (expected 'true')"); }
        if (c == 'f') { if (consumeLit("false")) return makeBool(false); fail("invalid literal (expected 'false')"); }
        if (c == 'n') { if (consumeLit("null"))  return makeNil(); fail("invalid literal (expected 'null')"); }
        if (c == '-' || std::isdigit((unsigned char)c)) return parseNumber();
        fail(std::string("unexpected character '") + c + "'");
    }

    Value parseObject() {
        get(); // {
        Value::map_type m;
        skipWs();
        if (peek() == '}') { get(); return Value(std::move(m)); }
        while (true) {
            skipWs();
            if (peek() != '"') fail("expected string key");
            std::string key = parseString();
            skipWs();
            if (peek() != ':') fail("expected ':' after object key");
            get();
            m[key] = parseValue();
            skipWs();
            if (peek() == ',') { get(); continue; }
            if (peek() == '}') { get(); break; }
            fail("expected ',' or '}' in object");
        }
        return Value(std::move(m));
    }

    Value parseArray() {
        get(); // [
        Value::array_type a;
        skipWs();
        if (peek() == ']') { get(); return Value(std::move(a)); }
        while (true) {
            a.push_back(parseValue());
            skipWs();
            if (peek() == ',') { get(); continue; }
            if (peek() == ']') { get(); break; }
            fail("expected ',' or ']' in array");
        }
        return Value(std::move(a));
    }

    std::string parseString() {
        get(); // opening quote
        std::string out;
        while (true) {
            if (eof()) fail("unterminated string");
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                if (eof()) fail("unterminated escape");
                char e = get();
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned cp = parseHex4();
                        // surrogate pair (\uD800-\uDBFF followed by \uDC00-\uDFFF);
                        // a lone or mismatched surrogate can't be encoded as
                        // valid UTF-8, so it's rejected rather than emitted
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (!(pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u'))
                                fail("unpaired high surrogate in \\u escape");
                            pos += 2;
                            unsigned lo = parseHex4();
                            if (lo < 0xDC00 || lo > 0xDFFF)
                                fail("high surrogate not followed by a low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("unpaired low surrogate in \\u escape");
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: fail(std::string("invalid escape '\\") + e + "'");
                }
            } else if (strictUtf8) {
                // RFC 8259: JSON text is UTF-8 and a raw control character must be escaped. the default
                // parser is lenient (raw bytes pass through); this opt-in mode rejects both.
                unsigned char uc = (unsigned char)c;
                if (uc < 0x20) { --pos; fail("unescaped control character in string"); }
                if (uc >= 0x80) {
                    size_t n = utf8SeqLen(s, pos - 1);
                    if (n == 0) { --pos; fail("invalid UTF-8 in string"); }
                    out.append(s, pos - 1, n);
                    pos += n - 1;
                } else out += c;
            } else {
                out += c;
            }
        }
        return out;
    }

    unsigned parseHex4() {
        if (pos + 4 > s.size()) fail("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[pos++];
            v <<= 4;
            if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else fail("invalid hex digit in \\u escape");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp <= 0x7F) {
            out += (char)cp;
        } else if (cp <= 0x7FF) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    Value parseNumber() {
        size_t start = pos;
        bool isFloat = false;
        if (peek() == '-') get();
        if (eof() || !std::isdigit((unsigned char)peek())) fail("invalid number");
        while (!eof() && std::isdigit((unsigned char)peek())) get();
        if (!eof() && peek() == '.') {
            isFloat = true;
            get();
            if (eof() || !std::isdigit((unsigned char)peek())) fail("invalid number (digits must follow '.')");
            while (!eof() && std::isdigit((unsigned char)peek())) get();
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            isFloat = true;
            get();
            if (!eof() && (peek() == '+' || peek() == '-')) get();
            if (eof() || !std::isdigit((unsigned char)peek())) fail("invalid number exponent");
            while (!eof() && std::isdigit((unsigned char)peek())) get();
        }
        std::string tok = s.substr(start, pos - start);

        // no '.' or exponent -> parse as an exact int64 so large integer IDs
        // round-trip precisely instead of losing precision through a double
        // (falls through to float parsing on overflow, e.g. a >64-bit literal)
        if (!isFloat) {
            int64_t iv = 0;
            auto [iptr, iec] = std::from_chars(tok.data(), tok.data() + tok.size(), iv);
            if (iec == std::errc() && iptr == tok.data() + tok.size())
                return Value(iv);
        }

        double d;
        auto [ptr, ec] = parse_double(tok.data(), tok.data() + tok.size(), d);
        if (ec != std::errc()) fail("invalid number literal: " + tok);
        return Value(d);
    }

    void expectEnd() {
        skipWs();
        if (!eof()) fail("trailing characters after JSON value");
    }
};

// serialization

void jsonEscapeInto(std::string& out, const std::string& str, bool lossy) {
    out += '"';
    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 0x80) {
            // a string that is not valid UTF-8 cannot be represented in JSON: emitting the raw bytes
            // would silently produce a document other parsers reject. raise, or (opt-in) substitute U+FFFD
            size_t n = utf8SeqLen(str, i);
            if (n == 0) {
                if (!lossy)
                    raiseError("json_stringify", "string contains invalid UTF-8 at byte " + std::to_string(i) +
                               " (JSON text must be UTF-8; pass {\"lossy\": 1} to replace such bytes with U+FFFD)");
                out += "\xEF\xBF\xBD";
                continue;
            }
            out.append(str, i, n);
            i += n - 1;
            continue;
        }
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void jsonNumberInto(std::string& out, double d) {
    if (std::isnan(d) || std::isinf(d)) { out += "null"; return; } // JSON has no NaN/Infinity
    out += formatNumber(d);
}

// indent <= 0 means compact (no inserted whitespace at all)
// depth is also a recursion guard (see kMaxJsonDepth). arrays/maps hold Values by value, so a too-deep value can
// only come from a script building one on purpose (wrapping an array in itself in a loop), not from a cycle
void stringifyInto(std::string& out, const Value& v, int indent, int depth, bool lossy) {
    if (depth > kMaxJsonDepth)
        raiseError("json_stringify", "value nested too deeply (max depth " +
                   std::to_string(kMaxJsonDepth) + ")");
    auto nl = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    // an array, or the elements of a vec, written as they are (no copy)
    auto writeArray = [&](const Value::array_type& a) {
        if (a.empty()) { out += "[]"; return; }
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            stringifyInto(out, a[i], indent, depth + 1, lossy);
        }
        nl(depth);
        out += ']';
    };
    switch (v.tag()) {
        case TypeTag::Number: jsonNumberInto(out, v.asNumber()); break;
        case TypeTag::Int:    out += std::to_string(v.asInt()); break;
        case TypeTag::String: jsonEscapeInto(out, v.asString(), lossy); break;
        case TypeTag::Array:
            writeArray(v.asArray());
            break;
        case TypeTag::Map: {
            const auto& m = v.asMap();
            if (m.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            // sorted so the same map stringifies the same way across runs (m is an
            // unordered_map; see embr::sortedMapKeys)
            for (const auto& k : embr::sortedMapKeys(m)) {
                if (!first) out += ',';
                first = false;
                nl(depth + 1);
                jsonEscapeInto(out, k, lossy);
                out += indent > 0 ? ": " : ":";
                stringifyInto(out, m.at(k), indent, depth + 1, lossy);
            }
            nl(depth);
            out += '}';
            break;
        }
        case TypeTag::Callable:
            raiseError("json_stringify", "cannot serialize a function value to JSON");
        case TypeTag::Pointer:
            if (isBool(v)) { out += v.truthy() ? "true" : "false"; break; }
            if (isNil(v))  { out += "null"; break; }
            // a vec (plugins/vec) is written as the array it holds. keep the tag in sync with vec.h
            if (embrvec::Vec* vp = embrvec::vecOf(v)) { writeArray(vp->items); break; }
            raiseError("json_stringify", "cannot serialize a pointer value to JSON");
    }
}

} // namespace

// reads a boolean-ish option from args[idx] (an optional map), e.g. {"strict_utf8": 1}
static bool optFlag(const std::vector<Value>& args, size_t idx, const char* key, const char* fn) {
    if (args.size() <= idx) return false;
    if (!args[idx].isMap()) raiseError(fn, "options must be a map");
    const auto& m = args[idx].asMap();
    auto it = m.find(key);
    return it != m.end() && it->second.truthy();
}

// json_parse(text: str) -> any
//
// parses a full JSON document and returns the equivalent embr value.
// raises an error (with position) on malformed input or trailing garbage
// after the top-level value.
Value json_parse(const std::vector<Value>& args) {
    const std::string& src = args[0].asString();
    JsonParser p(src, "json_parse");
    p.strictUtf8 = optFlag(args, 1, "strict_utf8", "json_parse");
    Value v = p.parseValue();
    p.expectEnd();
    return v;
}

// json_valid(text: str) -> num
//
// returns 1 if text is a syntactically valid JSON document, 0 otherwise.
// never raises.
Value json_valid(const std::vector<Value>& args) {
    const std::string& src = args[0].asString();
    try {
        JsonParser p(src, "json_valid");
        p.strictUtf8 = optFlag(args, 1, "strict_utf8", "json_valid");
        p.parseValue();
        p.expectEnd();
        return Value(1.0);
    } catch (const EmbrError&) {
        return Value(0.0);
    }
}

// json_stringify(value: any, indent?: num) -> str
//
// serializes an embr value to a JSON string.
//   json_stringify(v)      -> compact, no whitespace
//   json_stringify(v, 2)   -> pretty-printed with a 2-space indent
//
// map key order follows embr's unordered_map iteration order (unspecified,
// same as everywhere else map contents are enumerated in embr).
// functions and pointers have no JSON representation and raise an error.
Value json_stringify(const std::vector<Value>& args) {
    int indent = 0;
    if (args.size() >= 2) {
        int64_t n = numToInt64(args[1], "json_stringify", "indent");
        indent = n > 0 ? (int)std::min<int64_t>(n, 16) : 0;      // 16 spaces per level is already absurd
    }
    std::string out;
    stringifyInto(out, args[0], indent, 0, optFlag(args, 2, "lossy", "json_stringify"));
    return Value(std::move(out));
}

EMBR_PLUGIN {
    interp->bindSig("json_parse",     {pStr("text"), pOpt("opts", TS::Map)}, json_parse);
    interp->bindSig("json_valid",     {pStr("text"), pOpt("opts", TS::Map)}, json_valid);
    interp->bindSig("json_stringify", {pAny("value"), pOpt("indent", TS::Num), pOpt("opts", TS::Map)}, json_stringify);
}