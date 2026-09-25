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
// embr has no dedicated null/bool tag, only number/string/array/map/fn/ptr.
// mapping used here:
//   JSON null   -> Value(0.0)   (same as everywhere else in embr — 0 is falsy/"nil")
//   JSON true   -> Value(1.0)
//   JSON false  -> Value(0.0)
//   JSON number -> Value(double) if it has a '.' or exponent, else Value(int64) exactly
//   JSON string -> Value(string)
//   JSON array  -> Value(array_type)
//   JSON object -> Value(map_type)   (JSON object keys are always strings, matches embr maps)
//
// this is a lossy round trip on purpose: json_stringify(json_parse("false")) and
// json_stringify(json_parse("null")) both come back as "0", since embr can't tell
// null, false and the number 0 apart once they're a Value. document, don't "fix" -
// there's nowhere else to put the distinction without adding a new Value tag.

#include <embr/embr.h>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <charconv>

using namespace embr;

static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

namespace {

// recursive-descent JSON parser over a std::string, in the same rough style as
// embr's own Lexer/Parser (see include/embr/core/lexer.h, core/parser.h)
struct JsonParser {
    const std::string& s;
    size_t             pos = 0;
    std::string        context;   // native fn name, for error messages

    JsonParser(const std::string& src, std::string ctx)
        : s(src), context(std::move(ctx)) {}

    [[noreturn]] void fail(const std::string& msg) {
        raiseError(context, msg + " at position " + std::to_string(pos));
    }

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
        skipWs();
        if (eof()) fail("unexpected end of input");
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == 't') { if (consumeLit("true"))  return Value(1.0); fail("invalid literal (expected 'true')"); }
        if (c == 'f') { if (consumeLit("false")) return Value(0.0); fail("invalid literal (expected 'false')"); }
        if (c == 'n') { if (consumeLit("null"))  return Value(0.0); fail("invalid literal (expected 'null')"); }
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
                        // surrogate pair (\uD800-\uDBFF followed by \uDC00-\uDFFF)
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                            pos += 2;
                            unsigned lo = parseHex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: fail(std::string("invalid escape '\\") + e + "'");
                }
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

void jsonEscapeInto(std::string& out, const std::string& str) {
    out += '"';
    for (unsigned char c : str) {
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
void stringifyInto(std::string& out, const Value& v, int indent, int depth) {
    auto nl = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (v.tag()) {
        case TypeTag::Number: jsonNumberInto(out, v.asNumber()); break;
        case TypeTag::Int:    out += std::to_string(v.asInt()); break;
        case TypeTag::String: jsonEscapeInto(out, v.asString()); break;
        case TypeTag::Array: {
            const auto& a = v.asArray();
            if (a.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                stringifyInto(out, a[i], indent, depth + 1);
            }
            nl(depth);
            out += ']';
            break;
        }
        case TypeTag::Map: {
            const auto& m = v.asMap();
            if (m.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (const auto& [k, val] : m) {
                if (!first) out += ',';
                first = false;
                nl(depth + 1);
                jsonEscapeInto(out, k);
                out += indent > 0 ? ": " : ":";
                stringifyInto(out, val, indent, depth + 1);
            }
            nl(depth);
            out += '}';
            break;
        }
        case TypeTag::Callable:
            raiseError("json_stringify", "cannot serialize a function value to JSON");
        case TypeTag::Pointer:
            raiseError("json_stringify", "cannot serialize a pointer value to JSON");
    }
}

} // namespace

// json_parse(text: str) -> any
//
// parses a full JSON document and returns the equivalent embr value.
// raises an error (with position) on malformed input or trailing garbage
// after the top-level value.
Value json_parse(const std::vector<Value>& args) {
    const std::string& src = args[0].asString();
    JsonParser p(src, "json_parse");
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
        double n = args[1].asNumber();
        indent = n > 0 ? (int)n : 0;
    }
    std::string out;
    stringifyInto(out, args[0], indent, 0);
    return Value(std::move(out));
}

EMBR_PLUGIN {
    interp->bindSig("json_parse",     {pStr("text")}, json_parse);
    interp->bindSig("json_valid",     {pStr("text")}, json_valid);
    interp->bindSig("json_stringify", {pAny("value"), pOpt("indent", TS::Num)}, json_stringify);
}