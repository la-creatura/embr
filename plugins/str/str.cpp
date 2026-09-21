#include <embr/embr.h>
#include <cstring>
#include <cstdlib>

using namespace embr;

//static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
//static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }
//static Param pMap(std::string n)                      { return Param::req(std::move(n), TS::Map); }
static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
//static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[str:" + fn + "]", msg);
}

Value str_split(const std::vector<Value>& args) {
    const std::string& str = args[0].asString();
    const std::string& sep = args[1].asString();
    std::vector<Value> result;

    if (sep.empty()) {
        for (char c : str) result.emplace_back(std::string(1, c));
    } else {
        size_t start = 0, end;
        while ((end = str.find(sep, start)) != std::string::npos) {
            result.emplace_back(str.substr(start, end - start));
            start = end + sep.size();
        }
        result.emplace_back(str.substr(start));
    }
    return Value(result);
}

Value str_replace(const std::vector<Value>& args) {
    std::string s = args[0].asString();
    const std::string& oldStr = args[1].asString();
    const std::string& newStr = args[2].asString();
    if (oldStr.empty()) throwError("str_replace", "'old' cannot be empty");

    size_t pos = 0;
    while ((pos = s.find(oldStr, pos)) != std::string::npos) {
        s.replace(pos, oldStr.size(), newStr);
        pos += newStr.size();
    }
    return Value(s);
}

Value str_strip(const std::vector<Value>& args) {
    std::string s = args[0].asString();
    std::string chars = " \t\n\r\f\v";
    size_t start = s.find_first_not_of(chars);
    if (start == std::string::npos) return Value(std::string(""));
    size_t end = s.find_last_not_of(chars);
    return Value(s.substr(start, end - start + 1));
}

Value str_lower(const std::vector<Value>& args) {
    std::string s = args[0].asString();
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return Value(s);
}

Value str_upper(const std::vector<Value>& args) {
    std::string s = args[0].asString();
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return Value(s);
}

Value str_find(const std::vector<Value>& args) {
    size_t startPos = 0;
    if (args.size() >= 3) startPos = (size_t)args[2].asNumber();
    size_t pos = args[0].asString().find(args[1].asString(), startPos);
    return Value(pos == std::string::npos ? -1.0 : (double)pos);
}

Value str_startswith(const std::vector<Value>& args) {
    const std::string& s  = args[0].asString();
    const std::string& p  = args[1].asString();
    return Value((s.size() >= p.size() && s.compare(0, p.size(), p) == 0) ? 1.0 : 0.0);
}

Value str_endswith(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    const std::string& f = args[1].asString();
    return Value((s.size() >= f.size() && s.compare(s.size()-f.size(), f.size(), f) == 0) ? 1.0 : 0.0);
}

Value str_isdigit(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    return Value((!s.empty() && std::all_of(s.begin(), s.end(), ::isdigit)) ? 1.0 : 0.0);
}

Value str_isalpha(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    return Value((!s.empty() && std::all_of(s.begin(), s.end(), ::isalpha)) ? 1.0 : 0.0);
}

Value str_isalnum(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    return Value((!s.empty() && std::all_of(s.begin(), s.end(), ::isalnum)) ? 1.0 : 0.0);
}

Value str_count(const std::vector<Value>& args) {
    const std::string& s   = args[0].asString();
    const std::string& sub = args[1].asString();
    if (sub.empty()) return Value(0.0);
    size_t count = 0, pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) { ++count; pos += sub.size(); }
    return Value((double)count);
}

Value str_ord(const std::vector<Value>& args) {
    const std::string& s = args[0].asString();
    if (s.empty()) throwError("ord", "empty string");
    return Value((double)(unsigned char)s[0]);
}

Value str_chr(const std::vector<Value>& args) {
    if (!args[0].isNumber()) throwError("chr", "argument must be a number");
    int code = (int)args[0].asNumber();
    if (code < 0 || code > 255) throwError("chr", "code point out of range (0-255)");
    return Value(std::string(1, (char)code));
}

EMBR_PLUGIN {
    interp->bindSig("str_split",      {pStr("string"), pStr("separator")}, str_split);
    interp->bindSig("str_replace",    {pStr("string"), pStr("old"), pStr("new")}, str_replace);
    interp->bindSig("str_strip",      {pStr("string")}, str_strip);
    interp->bindSig("str_lower",      {pStr("string")}, str_lower);
    interp->bindSig("str_upper",      {pStr("string")}, str_upper);
    interp->bindSig("str_find",       {pStr("string"), pStr("substring"), pNum("start_pos")}, str_find);
    interp->bindSig("str_startswith", {pStr("string"), pStr("prefix")}, str_startswith);
    interp->bindSig("str_endswith",   {pStr("string"), pStr("suffix")}, str_endswith);
    interp->bindSig("str_isdigit",    {pStr("string")}, str_isdigit);
    interp->bindSig("str_isalpha",    {pStr("string")}, str_isalpha);
    interp->bindSig("str_isalnum",    {pStr("string")}, str_isalnum);
    interp->bindSig("str_count",      {pStr("string"), pStr("substring")}, str_count);
    interp->bindSig("str_ord",        {pStr("char")},   str_ord);
    interp->bindSig("str_chr",        {pNum("ord")},    str_chr);
}
