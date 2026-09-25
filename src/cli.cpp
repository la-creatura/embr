// cli.cpp
// usage:
//   embr                 # start REPL
//   embr file.embr ...   # run one or more script files
//   embr --vm ...        # same, using the bytecode VM backend instead of
//                         # the tree-walker (only available if this build
//                         # was configured with EMBR_WITH_VM=ON)

#include <embr/embr.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>

#ifdef _WIN32
#  include <conio.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace embr_debug {

using ChildFn = std::function<void(std::ostream&, const std::string&, bool)>;

static void printExpr(embr::Expr* e, std::ostream& out, const std::string& prefix, bool isLast);
static void printStmt(embr::Stmt* s, std::ostream& out, const std::string& prefix, bool isLast);

static std::string truncate(const std::string& s, size_t maxLen = 60) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "...";
}

static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '"':  out += "\\\""; break;
            default:   out += c;
        }
    }
    return out;
}

static std::string atLine(const embr::SourceRange& r) {
    return r.valid() ? ("  (L" + std::to_string(r.startLine) + ")") : "";
}

static std::string paramsToString(const std::vector<embr::Param>& params) {
    std::string s;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        const auto& p = params[i];
        if (p.variadic) s += "...";
        s += p.name;
        if (!p.type.isAny()) s += ": " + p.type.name();
        if (p.optional && !p.variadic) s += "?";
    }
    return s;
}

static void printNode(std::ostream& out, const std::string& prefix, bool isLast,
                      const std::string& label, const std::vector<ChildFn>& kids) {
    out << prefix << (isLast ? "\\-- " : "|-- ") << label << "\n";
    std::string childPrefix = prefix + (isLast ? "    " : "|   ");
    for (size_t i = 0; i < kids.size(); ++i)
        kids[i](out, childPrefix, i + 1 == kids.size());
}

static ChildFn blockPrinter(const std::vector<embr::StmtPtr>& block, std::string label) {
    return [&block, label](std::ostream& out, const std::string& prefix, bool isLast) {
        std::vector<ChildFn> kids;
        for (auto& s : block)
            kids.push_back([sp = s.get()](std::ostream& o, const std::string& p, bool last) {
                printStmt(sp, o, p, last);
            });
        printNode(out, prefix, isLast,
                 label + " (" + std::to_string(block.size()) + ")", kids);
    };
}

static void printExpr(embr::Expr* e, std::ostream& out, const std::string& prefix, bool isLast) {
    using Kind = embr::Expr::Kind;
    std::vector<ChildFn> kids;
    std::string label;

    auto addExpr = [&](embr::Expr* ep) {
        kids.push_back([ep](std::ostream& o, const std::string& p, bool last) { printExpr(ep, o, p, last); });
    };

    switch (e->kind) {
        case Kind::Number:
            label = "Number " + embr::formatNumber(static_cast<embr::NumberExpr*>(e)->v);
            break;
        case Kind::Int:
            label = "Int " + std::to_string(static_cast<embr::IntExpr*>(e)->v);
            break;
        case Kind::String:
            label = "String \"" + truncate(escape(static_cast<embr::StringExpr*>(e)->v)) + "\"";
            break;
        case Kind::Var:
            label = "Var " + static_cast<embr::VarExpr*>(e)->name;
            break;
        case Kind::Unary: {
            auto* u = static_cast<embr::UnaryExpr*>(e);
            label = "Unary " + u->op;
            addExpr(u->right.get());
            break;
        }
        case Kind::Binary: {
            auto* b = static_cast<embr::BinaryExpr*>(e);
            label = "Binary " + b->op;
            addExpr(b->left.get());
            addExpr(b->right.get());
            break;
        }
        case Kind::And:
        case Kind::Or: {
            auto* lg = static_cast<embr::LogicExpr*>(e);
            label = (e->kind == Kind::And) ? "And" : "Or";
            addExpr(lg->left.get());
            addExpr(lg->right.get());
            break;
        }
        case Kind::Call: {
            auto* c = static_cast<embr::CallExpr*>(e);
            label = "Call (" + std::to_string(c->args.size()) + " args)";
            addExpr(c->callee.get());
            for (auto& a : c->args) addExpr(a.get());
            break;
        }
        case Kind::Array: {
            auto* a = static_cast<embr::ArrayExpr*>(e);
            label = "Array [" + std::to_string(a->elements.size()) + "]";
            for (auto& el : a->elements) addExpr(el.get());
            break;
        }
        case Kind::Map: {
            auto* m = static_cast<embr::MapExpr*>(e);
            label = "Map {" + std::to_string(m->entries.size()) + "}";
            for (auto& [k, v] : m->entries) {
                kids.push_back([k = k.get(), v = v.get()](std::ostream& o, const std::string& p, bool last) {
                    std::vector<ChildFn> pairKids;
                    pairKids.push_back([k](std::ostream& o2, const std::string& p2, bool l2) { printExpr(k, o2, p2, l2); });
                    pairKids.push_back([v](std::ostream& o2, const std::string& p2, bool l2) { printExpr(v, o2, p2, l2); });
                    printNode(o, p, last, "Entry", pairKids);
                });
            }
            break;
        }
        case Kind::Index: {
            auto* idx = static_cast<embr::IndexExpr*>(e);
            label = "Index";
            addExpr(idx->object.get());
            addExpr(idx->index.get());
            break;
        }
        case Kind::Lambda: {
            auto* lam = static_cast<embr::LambdaExpr*>(e);
            label = "Lambda (" + paramsToString(lam->params) + ")";
            if (!lam->returnType.isAny()) label += " -> " + lam->returnType.name();
            for (auto& s : lam->ownedBody)
                kids.push_back([sp = s.get()](std::ostream& o, const std::string& p, bool last) {
                    printStmt(sp, o, p, last);
                });
            break;
        }
    }
    label += atLine(e->range);
    printNode(out, prefix, isLast, label, kids);
}

static void printStmt(embr::Stmt* s, std::ostream& out, const std::string& prefix, bool isLast) {
    using Kind = embr::Stmt::Kind;
    std::vector<ChildFn> kids;
    std::string label;

    auto addExpr = [&](embr::Expr* ep) {
        kids.push_back([ep](std::ostream& o, const std::string& p, bool last) { printExpr(ep, o, p, last); });
    };

    switch (s->kind) {
        case Kind::Assign: {
            auto* a = static_cast<embr::AssignStmt*>(s);
            label = "Assign";
            addExpr(a->target.get());
            addExpr(a->value.get());
            break;
        }
        case Kind::Local: {
            auto* l = static_cast<embr::LocalStmt*>(s);
            label = "Local ";
            if (l->bracketed) label += "[";
            for (size_t i = 0; i < l->targets.size(); ++i) {
                if (i) label += ", ";
                const auto& t = l->targets[i];
                if (t.isAuto)            label += "auto ";
                else if (!t.type.isAny()) label += t.type.name() + " ";
                label += t.name;
            }
            if (l->bracketed) label += "]";
            addExpr(l->value.get());
            break;
        }
        case Kind::MultiAssign: {
            auto* m = static_cast<embr::MultiAssignStmt*>(s);
            label = "MultiAssign ";
            for (size_t i = 0; i < m->names.size(); ++i) {
                if (i) label += ", ";
                label += m->names[i];
            }
            addExpr(m->value.get());
            break;
        }
        case Kind::If: {
            auto* i = static_cast<embr::IfStmt*>(s);
            label = "If";
            addExpr(i->cond.get());
            kids.push_back(blockPrinter(i->thenBlock, "Then"));
            if (!i->elseBlock.empty())
                kids.push_back(blockPrinter(i->elseBlock, "Else"));
            break;
        }
        case Kind::Fn: {
            auto* f = static_cast<embr::FnStmt*>(s);
            label = "Fn " + f->name + "(" + paramsToString(f->params) + ")";
            if (!f->returnType.isAny()) label += " -> " + f->returnType.name();
            kids.push_back(blockPrinter(f->body, "Body"));
            break;
        }
        case Kind::Return: {
            auto* r = static_cast<embr::ReturnStmt*>(s);
            label = "Return";
            addExpr(r->value.get());
            break;
        }
        case Kind::While: {
            auto* w = static_cast<embr::WhileStmt*>(s);
            label = "While";
            addExpr(w->cond.get());
            kids.push_back(blockPrinter(w->body, "Body"));
            break;
        }
        case Kind::For: {
            auto* f = static_cast<embr::ForStmt*>(s);
            label = "For " + (f->keyName.empty() ? f->varName : (f->keyName + ", " + f->varName));
            addExpr(f->iterable.get());
            kids.push_back(blockPrinter(f->body, "Body"));
            break;
        }
        case Kind::Break:
            label = "Break";
            break;
        case Kind::Continue:
            label = "Continue";
            break;
        case Kind::Import:
            label = "Import \"" + static_cast<embr::ImportStmt*>(s)->path + "\"";
            break;
        case Kind::Expr: {
            auto* es = static_cast<embr::ExprStmt*>(s);
            label = "ExprStmt";
            addExpr(es->expr.get());
            break;
        }
        case Kind::Try: {
            auto* t = static_cast<embr::TryStmt*>(s);
            label = "Try";
            kids.push_back(blockPrinter(t->tryBlock, "Try"));
            kids.push_back(blockPrinter(t->catchBlock, "Catch " + t->catchVar));
            break;
        }
    }
    label += atLine(s->range);
    printNode(out, prefix, isLast, label, kids);
}

static void printProgram(const std::vector<embr::StmtPtr>& prog, std::ostream& out) {
    out << "Program (" << prog.size() << " statement" << (prog.size() == 1 ? "" : "s") << ")\n";
    for (size_t i = 0; i < prog.size(); ++i)
        printStmt(prog[i].get(), out, "", i + 1 == prog.size());
}

static void printAstFromSource(const std::string& src) {
    embr::Lexer lex(src);
    auto tokens = lex.tokenize();
    embr::SourceMap sm = lex.sourceMap();
    embr::Parser parser(std::move(tokens), sm);
    auto prog = parser.parse();
    printProgram(prog, std::cout);
}

} // namespace embr_debug

// set once in main() from the --vm flag, before runFile()/repl() run anything
static bool g_useVM = false;

// picks the execution backend selected by --vm (or the tree-walker, the
// default) -- the only branch point cli.cpp needs, since embrlib and every
// other plugin already go through embr::invoke() (core/invoke.h) for their
// own backend-agnostic calls into script code.
//
// shared across every call within the process (multiple file arguments,
// and/or the REPL) via a function-local static VmRunner, so top-level state
// (variables, functions) persists the same way it does on the tree-walker
// path, where it simply lives in the long-lived Interpreter::scopes_[0].
// safe because cli.cpp only ever constructs one Interpreter per process.
static void runSourceDispatch(const std::string& src, embr::Interpreter& interp,
                              const std::string& filename) {
    if (g_useVM) {
#ifdef EMBR_WITH_VM
        static embr::vm::VmRunner vmRunner(interp);
        try {
            auto prog = embr::vm::compileSource(src, interp, filename);
            vmRunner.run(prog);
        } catch (const embr::EmbrError& e) {
            std::cerr << e.what();
        }
#else
        std::cerr << "embr: --vm requested but this build was not configured with EMBR_WITH_VM\n";
        std::exit(1);
#endif
    } else {
#ifdef EMBR_WITH_TREE_WALKER
        embr::runSource(src, interp, filename);
#else
        std::cerr << "embr: the tree-walker backend is not compiled into this build; pass --vm\n";
        std::exit(1);
#endif
    }
}

static void runFile(const std::string& path, embr::Interpreter& interp) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Error: cannot open: " << path << "\n"; return; }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    interp.scriptDir = fs::absolute(path).parent_path().string();
    runSourceDispatch(src, interp, path);
}

// nesting depth
//   +1 for each block-opener keyword (if, fn, while)
//   -1 for end
static int blockDelta(const std::string& line) {
    static const std::unordered_map<std::string,int> delta = {
        {"if",1},{"fn",1},{"while",1},{"for",1},
        {"elif",0},{"else",0},
        {"end",-1},
    };
    std::istringstream ss(line);
    std::string word; int d = 0;
    while (ss >> word) {
        auto it = delta.find(word);
        if (it != delta.end()) d += it->second;
    }
    return d;
}

static int totalDepth(const std::vector<std::string>& lines) {
    int d = 0;
    for (auto& l : lines) d += blockDelta(l);
    return d;
}

// ---------------------------------------------------------------------------
// raw terminal input
//
// on POSIX this puts the tty into non-canonical, no-echo mode for the
// duration of a RawMode object and restores it on destruction. on Windows
// _getch() already reads unbuffered/un-echoed input so RawMode is a no-op.
// arrow keys arrive as ESC '[' 'A'/'B'/'C'/'D' on POSIX and as a 0/224
// prefix byte followed by 72/80/75/77 on Windows.
// ---------------------------------------------------------------------------
namespace term {

#ifdef _WIN32
struct RawMode { RawMode() {} ~RawMode() {} };
inline int rawByte() { return _getch(); }
#else
struct RawMode {
    termios orig{};
    bool    ok = false;
    RawMode() {
        if (tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= ~(ICANON | ECHO | ISIG);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) ok = true;
        }
    }
    ~RawMode() { if (ok) tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig); }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;
};
inline int rawByte() {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    return (unsigned char)c;
}
#endif


enum class Key { Char, Enter, Backspace, Up, Down, Left, Right, Eof, Interrupt, Other };

struct KeyEvent { Key key; char ch = 0; };

inline KeyEvent readKey() {
#ifdef _WIN32
    int c = rawByte();
    if (c == EOF)  return {Key::Eof};
    if (c == 3)    return {Key::Interrupt};
    if (c == '\r' || c == '\n') return {Key::Enter};
    if (c == 8)    return {Key::Backspace};
    if (c == 0 || c == 224) {
        int c2 = rawByte();
        switch (c2) {
            case 72: return {Key::Up};
            case 80: return {Key::Down};
            case 75: return {Key::Left};
            case 77: return {Key::Right};
        }
        return {Key::Other};
    }
    return {Key::Char, (char)c};
#else
    int c = rawByte();
    if (c < 0)  return {Key::Eof};
    if (c == 3) return {Key::Interrupt};   // Ctrl-C
    if (c == 4) return {Key::Eof};         // Ctrl-D
    if (c == '\r' || c == '\n') return {Key::Enter};
    if (c == 127 || c == 8) return {Key::Backspace};
    if (c == 27) {
        int c1 = rawByte();
        if (c1 == '[') {
            int c2 = rawByte();
            switch (c2) {
                case 'A': return {Key::Up};
                case 'B': return {Key::Down};
                case 'C': return {Key::Right};
                case 'D': return {Key::Left};
            }
        }
        return {Key::Other};
    }
    return {Key::Char, (char)c};
#endif
}

} // namespace term

// ---------------------------------------------------------------------------
// single-line editor: history navigation with up/down, left/right cursor
// movement, backspace. behaves like a plain unix shell prompt.
// ---------------------------------------------------------------------------
struct LineResult { std::string text; bool eof = false; bool interrupted = false; };

static LineResult readLineSingle(std::vector<std::string>& history, const std::string& prompt) {
    term::RawMode raw;

    std::string buf;
    size_t      cursor = 0;
    int         histIdx = (int)history.size(); // one past the end = "editing a new line"
    std::string saved;                          // buffer stashed while browsing history

    auto redraw = [&]() {
        std::cout << "\r\x1b[2K" << prompt << buf;
        size_t back = buf.size() - cursor;
        if (back > 0) std::cout << "\x1b[" << back << "D";
        std::cout << std::flush;
    };

    redraw();

    while (true) {
        term::KeyEvent ev = term::readKey();
        switch (ev.key) {
            case term::Key::Enter:
                std::cout << "\n";
                return { buf, false, false };
            case term::Key::Eof:
                std::cout << "\n";
                return { "", true, false };
            case term::Key::Interrupt:
                std::cout << "^C\n";
                return { "", false, true };
            case term::Key::Backspace:
                if (cursor > 0) { buf.erase(cursor - 1, 1); --cursor; redraw(); }
                break;
            case term::Key::Left:
                if (cursor > 0) { --cursor; redraw(); }
                break;
            case term::Key::Right:
                if (cursor < buf.size()) { ++cursor; redraw(); }
                break;
            case term::Key::Up:
                if (histIdx > 0) {
                    if (histIdx == (int)history.size()) saved = buf;
                    --histIdx;
                    buf = history[histIdx];
                    cursor = buf.size();
                    redraw();
                }
                break;
            case term::Key::Down:
                if (histIdx < (int)history.size()) {
                    ++histIdx;
                    buf = (histIdx == (int)history.size()) ? saved : history[histIdx];
                    cursor = buf.size();
                    redraw();
                }
                break;
            case term::Key::Char:
                buf.insert(buf.begin() + (long)cursor, ev.ch);
                ++cursor;
                redraw();
                break;
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// multi-line editor: used once a block opener (if/fn/while) has been typed.
// up/down move the cursor vertically between lines, left/right move within
// and across lines, like a small text editor. Enter normally splits the
// current line the way a text editor would. Pressing down while on the
// last line creates a fresh temporary blank line below the cursor; pressing
// Enter on an empty last line (with balanced block depth) submits the
// whole buffer for execution instead of inserting another line.
// ---------------------------------------------------------------------------
struct MultiLineResult { std::optional<std::string> source; bool eof = false; };

static std::string promptFor(size_t row) { return row == 0 ? ">>> " : "... "; }

static MultiLineResult runMultiLineEditor(std::vector<std::string> lines, int startRow) {
    term::RawMode raw;

    int row = startRow;
    int col = (int)lines[row].size();
    int screenLines = (int)lines.size();

    auto redraw = [&]() {
        if (screenLines > 1) std::cout << "\x1b[" << (screenLines - 1) << "A";
        std::cout << "\r\x1b[0J";
        for (size_t i = 0; i < lines.size(); ++i) {
            std::cout << promptFor(i) << lines[i];
            if (i + 1 < lines.size()) std::cout << "\n";
        }
        screenLines = (int)lines.size();

        int upMove = (int)lines.size() - 1 - row;
        if (upMove > 0) std::cout << "\x1b[" << upMove << "A";
        std::cout << "\r";
        int rightMove = 4 /* prompt width */ + col;
        if (rightMove > 0) std::cout << "\x1b[" << rightMove << "C";
        std::cout << std::flush;
    };

    redraw();

    while (true) {
        term::KeyEvent ev = term::readKey();
        switch (ev.key) {
            case term::Key::Eof:
                std::cout << "\n";
                return { std::nullopt, true };

            case term::Key::Interrupt:
                std::cout << "\n^C\n";
                return { std::nullopt, false };

            case term::Key::Left:
                if (col > 0) {
                    --col;
                } else if (row > 0) {
                    --row;
                    col = (int)lines[row].size();
                }
                redraw();
                break;

            case term::Key::Right:
                if (col < (int)lines[row].size()) {
                    ++col;
                } else if (row + 1 < (int)lines.size()) {
                    ++row;
                    col = 0;
                }
                redraw();
                break;

            case term::Key::Up:
                if (row > 0) {
                    --row;
                    col = std::min(col, (int)lines[row].size());
                    redraw();
                }
                break;

            case term::Key::Down:
                if (row + 1 < (int)lines.size()) {
                    ++row;
                    col = std::min(col, (int)lines[row].size());
                } else {
                    // at the last line: open a temporary blank line so
                    // Enter can be used there to submit the block
                    lines.push_back("");
                    ++row;
                    col = 0;
                }
                redraw();
                break;

            case term::Key::Backspace:
                if (col > 0) {
                    lines[row].erase((size_t)col - 1, 1);
                    --col;
                } else if (row > 0) {
                    int prevLen = (int)lines[row - 1].size();
                    lines[row - 1] += lines[row];
                    lines.erase(lines.begin() + row);
                    --row;
                    col = prevLen;
                }
                redraw();
                break;

            case term::Key::Enter: {
                bool isLastLine  = (row + 1 == (int)lines.size());
                bool lineIsEmpty = lines[row].empty();

                if (isLastLine && lineIsEmpty && totalDepth(lines) <= 0) {
                    std::cout << "\n";
                    // drop the trailing blank line(s) used purely for
                    // navigation before joining into source
                    while (!lines.empty() && lines.back().empty())
                        lines.pop_back();
                    std::string src;
                    for (auto& l : lines) src += l + "\n";
                    return { src, false };
                }

                // normal editor behaviour: split the line at the cursor
                std::string tail = lines[row].substr((size_t)col);
                lines[row].resize((size_t)col);
                lines.insert(lines.begin() + row + 1, tail);
                ++row;
                col = 0;
                redraw();
                break;
            }

            case term::Key::Char:
                lines[row].insert(lines[row].begin() + col, ev.ch);
                ++col;
                redraw();
                break;

            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// REPL
// ---------------------------------------------------------------------------
static void repl(embr::Interpreter& interp) {
    std::cout << "embr REPL  (type 'exit' to quit)\n";
    std::vector<std::string> history;

    while (true) {
        LineResult first = readLineSingle(history, ">>> ");
        if (first.eof) break;
        if (first.interrupted) continue;

        std::string line = first.text;
        if (line.empty()) continue;
        if (line == "exit") break;

        if (!history.empty() && history.back() == line) {
            // don't spam duplicate consecutive entries
        } else {
            history.push_back(line);
        }

        int depth = blockDelta(line);

        std::string source;
        if (depth <= 0) {
            // single-line mode: run immediately
            source = line + "\n";
        } else {
            // multi-line mode: hand off to the block editor
            std::vector<std::string> lines = { line, "" };
            MultiLineResult mres = runMultiLineEditor(std::move(lines), 1);
            if (mres.eof) break;
            if (!mres.source) continue; // cancelled (Ctrl-C)
            source = *mres.source;
            history.push_back(source); // recall the whole block later, too
        }

        try {
            runSourceDispatch(source, interp, "<repl>");
        } catch (const embr::EmbrError& e) {
            std::cerr << e.what();
        } catch (const std::exception& e) {
            std::cerr << "[error] " << e.what() << "\n";
        }
    }
}

int main(int argc, char** argv) {
    embr::Interpreter interp;
    interp.bindSig("ast", {embr::Param::req("val", embr::TS::Str)},
    [](const std::vector<embr::Value>& args) -> embr::Value {
        embr_debug::printAstFromSource(args[0].asString());
        return embr::Value(0.0);
    });
    interp.bindSig("lex", {embr::Param::req("val", embr::TS::Str)},
    [](const std::vector<embr::Value>& args) -> embr::Value {
        embr::Lexer lex(args[0].asString());
        std::vector<embr::Token> tokens = lex.tokenize();
        for (size_t i = 0; i < tokens.size(); ++i)
            std::cout << "   " << tokens[i].text;
        return embr::Value(0.0);
    });
#ifdef _WIN32
    embr::enableAnsi();
#endif
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--vm") g_useVM = true;
        else               files.push_back(std::move(arg));
    }
    if (!files.empty()) {
        for (const auto& f : files) runFile(f, interp);
    } else {
        repl(interp);
    }
    return 0;
}
