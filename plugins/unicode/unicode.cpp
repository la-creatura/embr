// unicode.cpp
// small UTF-8-aware companion to str.cpp
//
// usage
//   import "unicode"
//   print(unicode_len("héllo"))        // 5 (codepoints), not str_len's 6 (bytes)
//   print(unicode_ord("é"))            // 233, not str_ord's single leading byte
//   print(unicode_chr(233))            // "é", UTF-8 encoded
//   print(unicode_chars("héllo"))      // ["h", "é", "l", "l", "o"]
//   print(unicode_valid("\xff\xfe"))   // 0
//
// good to know
//   - str_ord/str_chr/str_isalpha/... are byte-based on purpose (see str.cpp: fast, no decoding, matches
//     split/find/replace). this plugin is the opt-in "i mean codepoints" half, kept separate so a script that
//     never touches non-ASCII text pays nothing
//   - isalpha/isdigit/isalnum use the process's C locale (iswalpha etc. on the decoded codepoint), so they know
//     whatever glibc's current locale knows. that's not a full Unicode property table: good enough for "does this
//     look like a word in the user's language", not a Unicode database
//   - programs start in the "C" locale (ASCII-only) until something calls setlocale(), and embr's cli doesn't, so
//     EMBR_PLUGIN below sets LC_CTYPE from the environment (setlocale(LC_CTYPE, "")). that only widens
//     classification, so it's safe to do always. it's process-wide, but every load asks for the same locale

#include <embr/embr.h>
#include <clocale>
#include <cwctype>

using namespace embr;

static Param pStr(std::string n) { return Param::req(std::move(n), TS::Str); }
static Param pNum(std::string n) { return Param::req(std::move(n), TS::Num); }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[unicode:" + fn + "]", msg);
}

struct Decoded {
    uint32_t codepoint;
    size_t   len;   // bytes consumed (1 on invalid, so callers can skip forward)
    bool     valid;
};

// decodes one UTF-8 codepoint starting at s[pos]. pos must be < s.size().
static Decoded decodeOne(const std::string& s, size_t pos) {
    unsigned char c0 = (unsigned char)s[pos];
    if (c0 < 0x80) return {c0, 1, true};

    size_t extra; uint32_t cp;
    if      ((c0 & 0xE0) == 0xC0) { extra = 1; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { extra = 2; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { extra = 3; cp = c0 & 0x07; }
    else return {0, 1, false};

    if (pos + extra >= s.size()) return {0, 1, false};
    for (size_t i = 1; i <= extra; ++i) {
        unsigned char c = (unsigned char)s[pos + i];
        if ((c & 0xC0) != 0x80) return {0, 1, false};
        cp = (cp << 6) | (c & 0x3F);
    }

    // reject overlong encodings and surrogate halves, same as httplib's own is_valid_utf8
    static const uint32_t minForLen[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < minForLen[extra] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return {0, 1, false};

    return {cp, extra + 1, true};
}

static std::string encodeOne(uint32_t cp) {
    std::string out;
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
    return out;
}

Value unicode_ord(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    if (s.empty()) throwError("ord", "empty string");
    Decoded d = decodeOne(s, 0);
    if (!d.valid) throwError("ord", "invalid UTF-8 at byte 0");
    return Value((double)d.codepoint);
}

Value unicode_chr(const std::vector<Value>& args) {
    if (!args[0].isNumeric()) throwError("chr", "argument must be a number");
    int64_t code = numToInt64(args[0], "chr", "argument");
    if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        throwError("chr", "code point out of range (0-0x10FFFF, excluding surrogates)");
    return Value(encodeOne((uint32_t)code));
}

Value unicode_len(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    size_t count = 0, pos = 0;
    while (pos < s.size()) {
        Decoded d = decodeOne(s, pos);
        if (!d.valid) throwError("len", "invalid UTF-8 at byte " + std::to_string(pos));
        ++count;
        pos += d.len;
    }
    return Value((double)count);
}

Value unicode_valid(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    size_t pos = 0;
    while (pos < s.size()) {
        Decoded d = decodeOne(s, pos);
        if (!d.valid) return Value(0.0);
        pos += d.len;
    }
    return Value(1.0);
}

Value unicode_chars(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    std::vector<Value> result;
    size_t pos = 0;
    while (pos < s.size()) {
        Decoded d = decodeOne(s, pos);
        if (!d.valid) throwError("chars", "invalid UTF-8 at byte " + std::to_string(pos));
        result.emplace_back(s.substr(pos, d.len));
        pos += d.len;
    }
    return Value(result);
}

// applies a per-codepoint iswXXX classifier across the whole string;
// true only if the string is non-empty, valid UTF-8, and every codepoint matches
static Value classifyAll(const std::string& fn, const std::vector<Value>& args, int (*classify)(std::wint_t)) {
    const std::string& s = args[0].asString();
    if (s.empty()) return Value(0.0);
    size_t pos = 0;
    while (pos < s.size()) {
        Decoded d = decodeOne(s, pos);
        if (!d.valid) throwError(fn, "invalid UTF-8 at byte " + std::to_string(pos));
        if (!classify((std::wint_t)d.codepoint)) return Value(0.0);
        pos += d.len;
    }
    return Value(1.0);
}

Value unicode_isalpha(const std::vector<Value>& args) { return classifyAll("isalpha", args, std::iswalpha); }
Value unicode_isdigit(const std::vector<Value>& args) { return classifyAll("isdigit", args, std::iswdigit); }
Value unicode_isalnum(const std::vector<Value>& args) { return classifyAll("isalnum", args, std::iswalnum); }

EMBR_PLUGIN {
    std::setlocale(LC_CTYPE, "");

    interp->bindSig("unicode_ord",     {pStr("char")},   unicode_ord);
    interp->bindSig("unicode_chr",     {pNum("code")},   unicode_chr);
    interp->bindSig("unicode_len",     {pStr("string")}, unicode_len);
    interp->bindSig("unicode_valid",   {pStr("string")}, unicode_valid);
    interp->bindSig("unicode_chars",   {pStr("string")}, unicode_chars);
    interp->bindSig("unicode_isalpha", {pStr("string")}, unicode_isalpha);
    interp->bindSig("unicode_isdigit", {pStr("string")}, unicode_isdigit);
    interp->bindSig("unicode_isalnum", {pStr("string")}, unicode_isalnum);
}
