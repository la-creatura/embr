#ifndef EMBR_CORE_DIAGNOSTICS_H
#define EMBR_CORE_DIAGNOSTICS_H

// error types, source-location tracking, and small portability shims
// shared by every later stage (lexer, parser, AST, interpreter).

#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <system_error>
#include <charconv>
#include <cstdlib>

namespace embr {

// forward declaration. SourceRange::fromToken needs a complete Token, which isn't defined until core/token.h
// its body lives there. only the declaration lives here
struct Token;

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

struct SourceRange {
    int startLine = 0, startCol = 0;
    int endLine   = 0, endCol   = 0;

    SourceRange() = default;
    SourceRange(int sl, int sc, int el, int ec)
        : startLine(sl), startCol(sc), endLine(el), endCol(ec) {}

    bool valid() const { return startLine > 0; }

    // defined out-of-line in core/token.h once Token is a complete type
    static SourceRange fromToken(const Token& tok);

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

} // namespace embr

#endif // EMBR_CORE_DIAGNOSTICS_H
