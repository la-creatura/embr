// csv.cpp
// CSV parse/stringify for embr (RFC 4180 with a few documented leniencies).
//
// usage
//   import "csv"
//   rows = csv_parse("a,b\n1,\"x, y\"\n")      # [["a","b"],["1","x, y"]]
//   recs = csv_parse_dicts("n,v\nA,1\nB,2\n")  # [{"n":"A","v":"1"}, {"n":"B","v":"2"}]
//   print(csv_stringify([["a","b"],[1,"x,y"]]))
//
// rules
//   - fields are always strings on parse (no type guessing).
//   - a quoted field may contain the delimiter, newlines, and "" (an escaped quote).
//   - line endings: \n, \r\n and a bare \r all end a record. a final record
//     needs no terminator, and a trailing terminator does not add an empty record.
//   - completely blank lines are skipped (not a one-empty-field record).
//   - a leading UTF-8 BOM is ignored.
//   - malformed input raises with "line L, column C": an unterminated quote, a
//     quote in the middle of an unquoted field, or text after a closing quote.
//   - the optional delimiter is a single byte other than '"', '\r', '\n'.
//   - no recursion anywhere, so hostile input can't overflow the stack.
//
// csv_stringify quotes only when needed (delimiter, quote, CR/LF, leading/trailing
// space, or a record that is one empty cell), ends every record with "\n", and
// accepts str and number cells; any other cell type raises.

#include <embr/embr.h>
#include "../vec/vec.h"

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

[[noreturn]] static void fail(const std::string& fn, const std::string& msg) {
    raiseError("[csv:" + fn + "]", msg);
}

static char delimArg(const std::vector<Value>& args, size_t idx, const std::string& fn) {
    if (args.size() <= idx) return ',';
    const std::string& d = args[idx].asString();
    if (d.size() != 1 || d[0] == '"' || d[0] == '\r' || d[0] == '\n')
        fail(fn, "delimiter must be a single character other than '\"', CR or LF");
    return d[0];
}

static std::vector<std::vector<std::string>> parseCsv(const std::string& fn, const std::string& s, char delim) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    size_t i = 0;
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;

    size_t line = 1, lineStart = i;
    auto where = [&](size_t pos) {
        return "line " + std::to_string(line) + ", column " + std::to_string(pos - lineStart + 1);
    };

    bool inQuotes = false, afterQuote = false;
    bool fieldStart = true;    // nothing consumed yet for the current field
    bool rowStarted = false;   // anything consumed on the current record (blank-line detection)
    std::string quoteOpenedAt;

    auto endField = [&]() { row.push_back(std::move(field)); field.clear();
                            fieldStart = true; afterQuote = false; };
    auto endRow   = [&]() { endField(); rows.push_back(std::move(row)); row.clear(); rowStarted = false; };

    while (i < s.size()) {
        char c = s[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') { field += '"'; i += 2; continue; }
                inQuotes = false; afterQuote = true; ++i; continue;
            }
            if (c == '\n') { ++line; lineStart = i + 1; }
            field += c; ++i; continue;
        }
        if (c == '\r' || c == '\n') {
            size_t len = (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ? 2 : 1;
            if (rowStarted) endRow();          // else: blank line, skipped
            i += len; ++line; lineStart = i; continue;
        }
        if (afterQuote && c != delim)
            fail(fn, "unexpected character after closing quote at " + where(i));
        if (c == delim) { endField(); rowStarted = true; ++i; continue; }
        if (c == '"') {
            if (!fieldStart) fail(fn, "unexpected quote inside an unquoted field at " + where(i));
            quoteOpenedAt = where(i);
            inQuotes = true; fieldStart = false; rowStarted = true; ++i; continue;
        }
        field += c; fieldStart = false; rowStarted = true; ++i;
    }
    if (inQuotes) fail(fn, "unterminated quoted field opened at " + quoteOpenedAt);
    if (rowStarted) endRow();
    return rows;
}

static std::string cellText(const Value& v, const std::string& fn, size_t r, size_t c) {
    if (v.isString()) return v.asString();
    if (v.isNumeric()) return v.formatAsString();
    fail(fn, "cell [" + std::to_string(r) + "][" + std::to_string(c) + "] must be a string or number, got " + v.typeName());
}

EMBR_PLUGIN {
    // csv_parse(text: str, delimiter?: str) -> arr of arr of str
    interp->bindSig("csv_parse", {pStr("text"), pOpt("delimiter", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        char d = delimArg(args, 1, "csv_parse");
        Value::array_type out;
        for (auto& r : parseCsv("csv_parse", args[0].asString(), d)) {
            Value::array_type cells;
            cells.reserve(r.size());
            for (auto& f : r) cells.push_back(Value(std::move(f)));
            out.push_back(Value(std::move(cells)));
        }
        return Value(std::move(out));
    });

    // csv_parse_dicts(text: str, delimiter?: str) -> arr of map
    // the first record is the header. every later record must have exactly as
    // many fields as the header (raises with the record number otherwise);
    // duplicate header names raise.
    interp->bindSig("csv_parse_dicts", {pStr("text"), pOpt("delimiter", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        char d = delimArg(args, 1, "csv_parse_dicts");
        auto rows = parseCsv("csv_parse_dicts", args[0].asString(), d);
        Value::array_type out;
        if (rows.empty()) return Value(std::move(out));
        const auto& header = rows[0];
        for (size_t i = 0; i < header.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (header[i] == header[j]) fail("csv_parse_dicts", "duplicate header name '" + header[i] + "'");
        for (size_t r = 1; r < rows.size(); ++r) {
            if (rows[r].size() != header.size())
                fail("csv_parse_dicts", "record " + std::to_string(r) + " has " +
                     std::to_string(rows[r].size()) + " fields, header has " + std::to_string(header.size()));
            Value::map_type m;
            for (size_t c = 0; c < header.size(); ++c) m[header[c]] = Value(rows[r][c]);
            out.push_back(Value(std::move(m)));
        }
        return Value(std::move(out));
    });

    // csv_stringify(rows: arr of arr, delimiter?: str) -> str
    interp->bindSig("csv_stringify", {Param::req("rows", TS::Arr|TS::Ptr), pOpt("delimiter", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        char d = delimArg(args, 1, "csv_stringify");
        std::string out;
        // rows can be a vec too, read where it is
        embrvec::Vec* rowsVec = args[0].isPointer() ? embrvec::vecOf(args[0]) : nullptr;
        if (args[0].isPointer() && !rowsVec)
            fail("csv_stringify", "rows must be an array or a vec, got ptr with tag '" + args[0].asPointer().type + "'");
        const auto& rows = rowsVec ? rowsVec->items : args[0].asArray();
        for (size_t r = 0; r < rows.size(); ++r) {
            // a row can be an array or a vec, a vec is read where it is. keep the tag in sync with vec.h
            embrvec::Vec* vp = embrvec::vecOf(rows[r]);
            if (!vp && !rows[r].isArray())
                fail("csv_stringify", "row " + std::to_string(r) + " must be an array, got " + rows[r].typeName());
            const auto& cells = vp ? vp->items : rows[r].asArray();
            for (size_t c = 0; c < cells.size(); ++c) {
                if (c) out += d;
                std::string t = cellText(cells[c], "csv_stringify", r, c);
                bool quote = t.find_first_of(std::string("\"\r\n") + d) != std::string::npos ||
                             (!t.empty() && (t.front() == ' ' || t.back() == ' ')) ||
                             (t.empty() && cells.size() == 1);   // a bare blank line would be skipped on parse
                if (quote) {
                    out += '"';
                    for (char ch : t) { if (ch == '"') out += '"'; out += ch; }
                    out += '"';
                } else out += t;
            }
            out += '\n';
        }
        return Value(std::move(out));
    });
}
