// lsp.h
// the language server behind `embr lsp`. it talks JSON-RPC over stdin/stdout and, for now, does one
// thing: tell the editor about lex, parse and compile errors while you type.
//
// good to know
//   - the editor sends the whole file on every change (sync mode 1), so there is no incremental state.
//   - embr columns count bytes, the protocol counts UTF-16 units. toUtf16Col() converts.
//   - the json here is just enough for the protocol. the json plugin can't be used: plugins are
//     separate .so files and the cli does not load any.
//   - hover and go-to-definition are not done yet.

#ifndef EMBR_LSP_H
#define EMBR_LSP_H

#include <embr/embr.h>

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace embr_lsp {

// ---- a small json value ---------------------------------------------------------------------

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json& at(const std::string& key) const {
        static const Json none;
        auto it = o.find(key);
        return it == o.end() ? none : it->second;
    }
    bool isObject() const { return kind == Obj; }
    bool isString() const { return kind == Str; }
};

// parses one json value. returns false on any error; depth and size are bounded so a hostile client can't
// overflow the stack.
class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        ws();
        return i_ == t_.size();
    }

private:
    const std::string& t_;
    size_t i_ = 0;

    void ws() { while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_; }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
               out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i_ + 4 > t_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (i_ >= t_.size() || t_[i_] != '"') return false;
        ++i_;
        while (i_ < t_.size()) {
            char c = t_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= t_.size()) return false;
            switch (char e = t_[i_++]) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case '"': case '\\': case '/': out += e; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {   // surrogate pair
                        i_ += 2;
                        unsigned lo;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return false;
        ws();
        if (i_ >= t_.size()) return false;
        char c = t_[i_];
        if (c == '{') {
            ++i_; v.kind = Json::Obj; ws();
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= t_.size() || t_[i_++] != ':') return false;
                if (!value(v.o[k], depth + 1)) return false;
                ws();
                if (i_ >= t_.size()) return false;
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i_; v.kind = Json::Arr; ws();
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
            while (true) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (i_ >= t_.size()) return false;
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.kind = Json::Str; return str(v.s); }
        if (lit("true"))  { v.kind = Json::Bool; v.b = true;  return true; }
        if (lit("false")) { v.kind = Json::Bool; v.b = false; return true; }
        if (lit("null"))  { v.kind = Json::Null; return true; }
        size_t start = i_;
        while (i_ < t_.size() && (std::isdigit((unsigned char)t_[i_]) || t_[i_] == '-' || t_[i_] == '+' ||
                                  t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E')) ++i_;
        if (i_ == start) return false;
        v.kind = Json::Num;
        v.n = std::strtod(t_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

inline std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += (char)c;
        }
    }
    return out + "\"";
}

// ---- the server -----------------------------------------------------------------------------

// returns every error found in a document, see diagnose() in cli.cpp
using DiagnoseFn = std::function<std::vector<embr::EmbrError>(const std::string& text, const std::string& uri)>;

// a byte column (1-based, what embr reports) to the 0-based UTF-16 column the protocol wants
inline int toUtf16Col(const std::string& line, int byteCol) {
    size_t end = byteCol > 0 ? (size_t)byteCol - 1 : 0;
    if (end > line.size()) end = line.size();
    int units = 0;
    for (size_t i = 0; i < end; ++i) {
        unsigned char c = (unsigned char)line[i];
        if ((c & 0xC0) == 0x80) continue;          // continuation byte: part of the previous character
        units += c >= 0xF0 ? 2 : 1;                // a 4-byte character is a surrogate pair in UTF-16
    }
    return units;
}

class Server {
public:
    Server(std::istream& in, std::ostream& out, DiagnoseFn diagnose) : in_(in), out_(out), diagnose_(std::move(diagnose)) {}

    // runs until `exit` or end of input. returns the process exit code the protocol asks for.
    int run() {
        std::string body;
        while (readMessage(body)) {
            Json msg;
            if (!JsonParser(body).parse(msg) || !msg.isObject()) { replyError("null", -32700, "parse error"); continue; }
            handle(msg);
            if (exit_) return shutdown_ ? 0 : 1;
        }
        return 1;      // the client went away without `exit`
    }

private:
    std::istream& in_;
    std::ostream& out_;
    DiagnoseFn diagnose_;
    std::map<std::string, std::string> docs_;   // uri -> text
    bool shutdown_ = false, exit_ = false;

    static constexpr size_t kMaxMessage = 64u * 1024 * 1024;

    bool readMessage(std::string& body) {
        long length = -1;
        std::string line;
        while (std::getline(in_, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                if (length < 0) continue;
                if ((size_t)length > kMaxMessage) return false;
                body.assign((size_t)length, '\0');
                in_.read(&body[0], length);
                return in_.gcount() == length;
            }
            if (line.rfind("Content-Length:", 0) == 0) length = std::atol(line.c_str() + 15);
        }
        return false;
    }

    void send(const std::string& json) {
        out_ << "Content-Length: " << json.size() << "\r\n\r\n" << json << std::flush;
    }
    void reply(const std::string& id, const std::string& result) {
        send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
    }
    void replyError(const std::string& id, int code, const std::string& message) {
        send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) +
             ",\"message\":" + quote(message) + "}}");
    }
    // an id is a number or a string; echo it back in the same form
    static std::string idText(const Json& id) {
        if (id.kind == Json::Num) { char b[32]; std::snprintf(b, sizeof b, "%.0f", id.n); return b; }
        if (id.kind == Json::Str) return quote(id.s);
        return "null";
    }

    static std::string lineOf(const std::string& text, int line) {   // 1-based
        size_t start = 0;
        for (int l = 1; l < line; ++l) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) return "";
            start = nl + 1;
        }
        size_t end = text.find('\n', start);
        std::string out = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return out;
    }

    void publish(const std::string& uri) {
        std::string list;
        auto it = docs_.find(uri);
        if (it != docs_.end()) {
            const std::string& text = it->second;
            for (const embr::EmbrError& e : diagnose_(text, uri)) {
                int sl = 0, sc = 0, el = 0, ec = 0;
                if (e.hasLocation && e.range.valid()) {
                    sl = e.range.startLine - 1; el = (e.range.endLine > 0 ? e.range.endLine : e.range.startLine) - 1;
                    sc = toUtf16Col(lineOf(text, e.range.startLine), e.range.startCol);
                    ec = toUtf16Col(lineOf(text, el + 1), e.range.endCol > 0 ? e.range.endCol : e.range.startCol + 1);
                    if (el == sl && ec <= sc) ec = sc + 1;     // an empty range is invisible in most editors
                }
                if (!list.empty()) list += ",";
                list += "{\"range\":{\"start\":{\"line\":" + std::to_string(sl) + ",\"character\":" + std::to_string(sc) +
                        "},\"end\":{\"line\":" + std::to_string(el) + ",\"character\":" + std::to_string(ec) +
                        "}},\"severity\":1,\"source\":\"embr\",\"message\":" +
                        quote((e.context.empty() ? "" : e.context + ": ") + e.message) + "}";
            }
        }
        send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" + quote(uri) +
             ",\"diagnostics\":[" + list + "]}}");
    }

    void handle(const Json& msg) {
        const std::string method = msg.at("method").s;
        const Json& params = msg.at("params");
        bool isRequest = msg.o.count("id") > 0;
        std::string id = idText(msg.at("id"));

        if (method == "initialize") {
            reply(id, "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1}},"
                      "\"serverInfo\":{\"name\":\"embr\"}}");
        } else if (method == "shutdown") {
            shutdown_ = true;
            reply(id, "null");
        } else if (method == "exit") {
            exit_ = true;
        } else if (method == "textDocument/didOpen") {
            const Json& d = params.at("textDocument");
            if (d.at("uri").isString() && d.at("text").isString()) {
                docs_[d.at("uri").s] = d.at("text").s;
                publish(d.at("uri").s);
            }
        } else if (method == "textDocument/didChange") {
            const std::string& uri = params.at("textDocument").at("uri").s;
            const Json& changes = params.at("contentChanges");
            if (!uri.empty() && !changes.a.empty() && changes.a.back().at("text").isString()) {
                docs_[uri] = changes.a.back().at("text").s;      // full sync: the last change is the whole file
                publish(uri);
            }
        } else if (method == "textDocument/didClose") {
            const std::string& uri = params.at("textDocument").at("uri").s;
            docs_.erase(uri);
            publish(uri);                                          // an erased document publishes an empty list
        } else if (isRequest) {
            replyError(id, -32601, "method not found: " + method);
        }
        // any other notification ($/cancelRequest, initialized, ...) is ignored
    }
};

} // namespace embr_lsp

#endif // EMBR_LSP_H
