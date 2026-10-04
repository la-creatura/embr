// regex.cpp
// regular-expression plugin for embr (ECMAScript syntax). matching runs on our own engine (pike.h), with
// std::regex as a fallback for patterns that need backtracking (see "matching engine" below)
//
// usage
//   import "regex"
//   print(re_test("hello world", "wor.d"))          # 1
//   print(re_match("2024-01-02", "(\\d+)-(\\d+)-(\\d+)"))
//     # ["2024-01-02", "2024", "01", "02"], index 0 is the whole match
//   print(re_find_all("a1 b22 c333", "[a-z](\\d+)"))
//     # [["a1", "1"], ["b22", "22"], ["c333", "333"]]
//   print(re_replace("2024-01-02", "-", "/"))        # "2024/01/02"
//
// a malformed pattern would make std::regex throw std::regex_error, which would escape embr's exception handling
// and crash the process. every entry point builds its regex through compileRegex(), so a bad pattern is a normal,
// catchable error

#include <embr/embr.h>
#include <regex>
#include "pike.h"

using namespace embr;

// matching engine
//   most patterns run on rx:: (pike.h), a linear-time engine with no recursion. std::regex (libstdc++) recurses over
//   the subject, so even `[ab]*c` over ~30,000 characters overflowed the stack and killed the process, and nested
//   quantifiers like `(a+)+$` backtrack exponentially (ReDoS)
//   patterns that need backtracking (backreferences \1, lookahead/lookbehind, named groups) aren't supported by rx and
//   fall back to std::regex, only for subjects up to kBacktrackMaxSubject bytes (measured safe for the stack), and
//   raise a clear error beyond that. a fallback pattern can still be slow. that limit is documented, not hidden
static constexpr size_t kBacktrackMaxSubject = 5000;

static Param pStr(std::string n) { return Param::req(std::move(n), TS::Str); }

[[noreturn]] static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[regex:" + fn + "]", msg);
}

// a pattern compiled for whichever engine can run it
struct Compiled {
    bool        useRx = true;
    rx::Prog    prog;            // when useRx
    std::regex  re;              // when !useRx
    std::string why;             // why the fallback was needed
};

// compiles pattern, converting every malformed-pattern exception (rx::SyntaxError, std::regex_error)
// into a catchable EmbrError instead of letting it escape as an unrelated C++ exception
static Compiled compilePattern(const std::string& fn, const std::string& pattern) {
    Compiled c;
    try {
        c.prog = rx::compile(pattern);
        return c;
    } catch (const rx::SyntaxError& e) {
        throwError(fn, "invalid pattern \"" + pattern + "\": " + e.what());
    } catch (const rx::Unsupported& e) {
        c.useRx = false;
        c.why = e.what();
    }
    try {
        c.re = std::regex(pattern);
    } catch (const std::regex_error& e) {
        throwError(fn, "invalid pattern \"" + pattern + "\": " + e.what());
    }
    return c;
}

static void requireBacktrackSafe(const std::string& fn, const Compiled& c, const std::string& subject) {
    if (!c.useRx && subject.size() > kBacktrackMaxSubject)
        throwError(fn, "this pattern uses " + c.why + ", which needs the backtracking engine; subjects over " +
                   std::to_string(kBacktrackMaxSubject) + " bytes are refused for it (got " +
                   std::to_string(subject.size()) + "). rewrite the pattern without it, or split the input.");
}

// ---- engine-neutral results: capture positions, [start,end) pairs, -1 when a group did not take part
using Caps = std::vector<int>;

static Value capsToArray(const std::string& s, const Caps& caps) {
    Value::array_type out;
    out.reserve(caps.size() / 2);
    for (size_t g = 0; g + 1 < caps.size(); g += 2)
        out.push_back(Value(caps[g] < 0 ? std::string() : s.substr((size_t)caps[g], (size_t)(caps[g + 1] - caps[g]))));
    return Value(std::move(out));
}

static Caps smatchToCaps(const std::smatch& m, const std::string& s) {
    Caps c;
    for (size_t g = 0; g < m.size(); ++g) {
        if (!m[g].matched) { c.push_back(-1); c.push_back(-1); continue; }
        c.push_back((int)(m[g].first - s.begin()));
        c.push_back((int)(m[g].second - s.begin()));
    }
    return c;
}

// every non-overlapping match in order (empty matches advance one byte, like regex_iterator / JS)
static std::vector<Caps> allMatches(const Compiled& c, const std::string& s, bool firstOnly) {
    std::vector<Caps> out;
    if (c.useRx) {
        rx::Matcher m(c.prog, s);
        size_t pos = 0;
        Caps caps;
        bool prevEmpty = false;
        // the same iteration std::regex_iterator performs: after an empty match, first retry at the same
        // position demanding a non-empty match, and only if that fails step forward one byte
        while (pos <= s.size()) {
            bool found = false;
            if (prevEmpty) {
                found = m.search(pos, caps, /*anchoredNotNull=*/true);
                if (!found) { ++pos; prevEmpty = false; continue; }
            } else {
                found = m.search(pos, caps);
            }
            if (!found) break;
            out.push_back(caps);
            if (firstOnly) break;
            pos = (size_t)caps[1];
            prevEmpty = (caps[1] == caps[0]);
        }
    } else if (firstOnly) {
        std::smatch m;
        if (std::regex_search(s, m, c.re)) out.push_back(smatchToCaps(m, s));
    } else {
        for (auto it = std::sregex_iterator(s.begin(), s.end(), c.re), end = std::sregex_iterator(); it != end; ++it)
            out.push_back(smatchToCaps(*it, s));
    }
    return out;
}

// re_test(str: str, pattern: str) -> num
// 1 if pattern matches anywhere in str, 0 otherwise
Value re_test(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    Compiled c = compilePattern("re_test", args[1].asString());
    requireBacktrackSafe("re_test", c, s);
    return Value(allMatches(c, s, true).empty() ? 0.0 : 1.0);
}

// re_match(str: str, pattern: str) -> arr
// the first match found anywhere in str, as [whole_match, group1, ...], or
// an empty array if there's no match (unmatched optional groups are empty strings)
Value re_match(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    Compiled c = compilePattern("re_match", args[1].asString());
    requireBacktrackSafe("re_match", c, s);
    auto m = allMatches(c, s, true);
    if (m.empty()) return Value(Value::array_type{});
    return capsToArray(s, m[0]);
}

// re_find_all(str: str, pattern: str) -> arr of arr
// every non-overlapping match in str, each shaped like re_match's result
Value re_find_all(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    Compiled c = compilePattern("re_find_all", args[1].asString());
    requireBacktrackSafe("re_find_all", c, s);
    Value::array_type out;
    for (const Caps& m : allMatches(c, s, false)) out.push_back(capsToArray(s, m));
    return Value(std::move(out));
}

// appends `repl` to out, expanding ECMAScript substitutions: $$ -> $, $& -> whole match, $` -> text before
// the match, $' -> text after it, $n / $nn -> group n (a reference to a group that doesn't exist is kept
// literally, as in JavaScript)
static void appendReplacement(std::string& out, const std::string& repl, const std::string& s, const Caps& m) {
    size_t ngroups = m.size() / 2 - 1;
    for (size_t i = 0; i < repl.size(); ++i) {
        char ch = repl[i];
        if (ch != '$' || i + 1 >= repl.size()) { out += ch; continue; }
        char n = repl[i + 1];
        if (n == '$') { out += '$'; ++i; }
        else if (n == '&') { out.append(s, (size_t)m[0], (size_t)(m[1] - m[0])); ++i; }
        else if (n == '`') { out.append(s, 0, (size_t)m[0]); ++i; }
        else if (n == '\'') { out.append(s, (size_t)m[1], std::string::npos); ++i; }
        else if (n >= '0' && n <= '9') {
            size_t g = (size_t)(n - '0');
            size_t used = 1;
            if (i + 2 < repl.size() && repl[i + 2] >= '0' && repl[i + 2] <= '9') {
                size_t g2 = g * 10 + (size_t)(repl[i + 2] - '0');
                if (g2 >= 1 && g2 <= ngroups) { g = g2; used = 2; }
            }
            if (g >= 1 && g <= ngroups) {
                if (m[2 * g] >= 0) out.append(s, (size_t)m[2 * g], (size_t)(m[2 * g + 1] - m[2 * g]));
                i += used;
            } else out += ch;       // no such group: keep the "$n" text
        } else out += ch;
    }
}

// re_replace(str: str, pattern: str, replacement: str) -> str
// replaces every match of pattern with replacement, which may use $1.. $& $` $' $$
Value re_replace(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    Compiled c = compilePattern("re_replace", args[1].asString());
    requireBacktrackSafe("re_replace", c, s);
    const std::string& repl = args[2].asString();
    std::string out;
    size_t last = 0;
    for (const Caps& m : allMatches(c, s, false)) {
        out.append(s, last, (size_t)m[0] - last);
        appendReplacement(out, repl, s, m);
        if (out.size() > (size_t)kMaxScriptAlloc)    // e.g. an empty pattern with a long replacement
            throwError("re_replace", "result would exceed the limit of " + std::to_string(kMaxScriptAlloc) + " bytes");
        last = (size_t)m[1];
    }
    out.append(s, last, std::string::npos);
    return Value(std::move(out));
}

EMBR_PLUGIN {
    interp->bindSig("re_test",     {pStr("string"), pStr("pattern")}, re_test);
    interp->bindSig("re_match",    {pStr("string"), pStr("pattern")}, re_match);
    interp->bindSig("re_find_all", {pStr("string"), pStr("pattern")}, re_find_all);
    interp->bindSig("re_replace",  {pStr("string"), pStr("pattern"), pStr("replacement")}, re_replace);
}
