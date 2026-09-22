// cli.cpp
// usage:
//   embr                 # start REPL
//   embr file.embr ...   # run one or more script files

#define EMBR_NO_MAIN
// #include <embr/embr.h>
#include "embr_vm.cpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// embr_debug :: lex + parse a string and print the resulting AST as a
// box-drawn tree, e.g.:
//
//   Program (2 statements)
//   ├── Local x  (L1)
//   │   └── Number 5
//   └── ExprStmt  (L2)
//       └── Call print (1 args)  (L2)
//           └── Binary +
//               ├── Var x
//               └── Number 1
//
// bound into the interpreter as ast(src) / ast_file(path) for REPL use.
// ---------------------------------------------------------------------
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

// draws one node ("label") at prefix/isLast, then recurses into kids
// one level deeper. this is the only place that emits box-drawing chars.
static void printNode(std::ostream& out, const std::string& prefix, bool isLast,
                      const std::string& label, const std::vector<ChildFn>& kids) {
    out << prefix << (isLast ? "\\-- " : "|-- ") << label << "\n";
    std::string childPrefix = prefix + (isLast ? "    " : "|   ");
    for (size_t i = 0; i < kids.size(); ++i)
        kids[i](out, childPrefix, i + 1 == kids.size());
}

// wraps a statement block (then/else/body) as a single labeled child node
// so `Then (2)` / `Else (1)` / `Body (3)` show up as their own branch.
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
                // group each entry under a synthetic "key: value" pair node
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
            label = "Local " + l->name;
            addExpr(l->value.get());
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
        case Kind::Import:
            label = "Import \"" + static_cast<embr::ImportStmt*>(s)->path + "\"";
            break;
        case Kind::Expr: {
            auto* es = static_cast<embr::ExprStmt*>(s);
            label = "ExprStmt";
            addExpr(es->expr.get());
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

// lex + parse `src` (parser recovers from and reports syntax errors itself,
// same as normal script execution) and print the tree to stdout
static void printAstFromSource(const std::string& src) {
    embr::Lexer lex(src);
    auto tokens = lex.tokenize();
    embr::SourceMap sm = lex.sourceMap();
    embr::Parser parser(std::move(tokens), sm);
    auto prog = parser.parse();
    printProgram(prog, std::cout);
}

} // namespace embr_debug

static void runFile(const std::string& path, embr::Interpreter& interp) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Error: cannot open: " << path << "\n"; return; }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    interp.scriptDir = fs::absolute(path).parent_path().string();
    embr::vm::runSource(src, interp, path);
}

// nesting depth
//   +1 for each block-opener keyword (if, fn, while)
//   -1 for end
static int blockDelta(const std::string& line) {
    // tokenise just enough to count depth tokens
    // we only need to handle the keywords
    static const std::unordered_map<std::string,int> delta = {
        {"if",1},{"fn",1},{"while",1},
        {"else",0},
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

static void repl(embr::Interpreter& interp) {
    std::cout << "embr REPL  (type 'exit' to quit)\n";
    std::string line, accumulated;
    int depth = 0;

    while (true) {
        std::cout << (depth > 0 ? "... " : ">>> ") << std::flush;
        if (!std::getline(std::cin, line)) break;

        if (depth == 0 && line == "exit") break;
        if (depth == 0 && line.empty())   continue;

        accumulated += line + "\n";
        depth += blockDelta(line);

        // if in block, keep accumulating
        if (depth > 0) continue;
        depth = 0; // clamp

        try {
            embr::vm::runSource(accumulated, interp, "<repl>");
        } catch (const embr::EmbrError& e) {
            std::cerr << e.what();
        } catch (const std::exception& e) {
            std::cerr << "[error] " << e.what() << "\n";
        }
        accumulated.clear();
    }
}

//#include "../plugins/embrlib/embrlib.cpp"

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
    //registerEmbrLib(interp);
#ifdef _WIN32
    embr::enableAnsi();
#endif
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            runFile(argv[i], interp);
    } else {
        repl(interp);
    }
    return 0;
}
