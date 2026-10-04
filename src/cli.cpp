// cli.cpp
// usage:
//   embr                        # start REPL
//   embr file.embr ...          # run one or more script files
//   embr file.embr -- a b       # same; a, b are handed to the script (os_args() in the os plugin)
//   embr --sandbox ...           # restrict imports to plugins that can't touch files/processes/network/threads
//   embr --allow-plugins=a,b ... # restrict imports to exactly these plugins
//   embr --no-plugins ...        # allow no plugin imports at all
//   embr test [--filter=T] [PATH...]  # run script tests (test_* functions, see modules/testing.embr)
//   embr check FILE...           # parse + compile only (no run, no plugins); errors as file:line:col: message
//   embr fmt [--check] [--stdout] [--indent=N] FILE...  # format source files in place (--check: just list files that would change)
//   embr lsp                     # language server on stdin/stdout: shows lex/parse/compile errors in an editor
//   embr --version               # which build directory made this binary, and its backends
//   embr --vm ...                # run on the bytecode VM instead of the tree-walker (needs a build with EMBR_WITH_VM=ON)
//   embr pkg install <name> [version] [--registry <url>]
//   embr pkg list | remove <name> | versions <name>
//                                # package manager (modules/pkg.embr, see its header for the registry protocol)

#include <embr/embr.h>
#include "fmt.h"
#include "lsp.h"

#include <iostream>
#include <memory>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>

#ifdef _WIN32
#  include <conio.h>
#  include <io.h>
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
        case Kind::Ref: {
            label = "Ref";
            addExpr(static_cast<embr::RefExpr*>(e)->target.get());
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
            label = "Import";
            addExpr(static_cast<embr::ImportStmt*>(s)->pathExpr.get());
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

#ifndef EMBR_BUILD_NAME
#define EMBR_BUILD_NAME "unknown"
#endif

// bin/ is shared by every CMake preset, so a binary missing a backend is very
// often just "the last preset to build overwrote it" rather than a real bug
[[maybe_unused]] static const char* kSharedBinHint =
    "note: this binary was built by build/" EMBR_BUILD_NAME ". bin/<platform>/ is shared by all\n"
    "presets, so building another preset (e.g. linux-debug) afterwards overwrites it. If you expected\n"
    "a different backend, rebuild that preset (e.g. `cmake --build build/linux-vm-debug`).\n";

static std::string backendSummary() {
    std::string s;
#ifdef EMBR_WITH_TREE_WALKER
    s += "tree-walker";
#endif
#ifdef EMBR_WITH_VM
    s += s.empty() ? "vm" : ", vm";
#endif
    return s.empty() ? "none" : s;
}

// set once in main() from the --vm flag, before runFile()/repl() run anything
static bool g_useVM = false;

// the plugins `--sandbox` allows: everything that cannot touch the outside world (files, processes,
// network, foreign code, threads). embrlib's own file natives are turned off separately (fileIo).
static const char* const kSandboxPlugins[] = {
    "embrlib", "str", "unicode", "json", "csv", "encoding", "regex", "embrmath", "table", "vec", "time", "path",
    "random", "embrmeta", "embrtypes", "gc", "coroutine",
};
static embr::PluginPolicy g_policy;
#ifdef EMBR_WITH_VM
// the VM backend's persistent top-level runner: top-level variables live in it across run() calls (REPL,
// several files). main() resets it before the interpreter is destroyed so its frames don't outlive
// (and leak through) the scopes they reference.
static std::unique_ptr<embr::vm::VmRunner> g_vmRunner;
#endif

// runs src on the backend picked by --vm (tree-walker by default). the only place cli.cpp has to choose, since
// embrlib and every other plugin already go through embr::invoke() (core/invoke.h)
//
// the VM runner is shared across every call in the process (several file arguments, the REPL) as a static, so
// top-level variables and functions persist, like they do on the tree-walker (where they live in
// Interpreter::scopes_[0]). that's safe because cli.cpp makes only one Interpreter per process
static void runSourceDispatch(const std::string& src, embr::Interpreter& interp,
                              const std::string& filename) {
    if (g_useVM) {
#ifdef EMBR_WITH_VM
        // kept at file scope (see g_vmRunner) so main() can release it BEFORE the interpreter is destroyed
        if (!g_vmRunner) g_vmRunner = std::make_unique<embr::vm::VmRunner>(interp);
        auto& vmRunner = *g_vmRunner;
        try {
            auto prog = embr::vm::compileSource(src, interp, filename);
            vmRunner.run(prog);
        } catch (const embr::EmbrError& e) {
            std::cerr << e.what() << embr::formatTrace(e);
        }
#else
        std::cerr << "embr: --vm requested but this binary has no VM backend (compiled backends: "
                  << backendSummary() << ")\n" << kSharedBinHint;
        std::exit(1);
#endif
    } else {
#ifdef EMBR_WITH_TREE_WALKER
        // a lexer error (bad escape, unterminated string, out-of-range number) is thrown while
        // tokenizing, before embr::runSource's own try/catch around execution, so it must be handled
        // here or it escapes main() and std::terminate kills the process (the VM branch above already
        // catches the same errors around compileSource)
        try {
            embr::runSource(src, interp, filename);
        } catch (const embr::EmbrError& e) {
            std::cerr << e.what() << embr::formatTrace(e);
        }
#else
        std::cerr << "embr: the tree-walker backend is not compiled into this binary (compiled backends: "
                  << backendSummary() << "); pass --vm\n" << kSharedBinHint;
        std::exit(1);
#endif
    }
}

// escapes s for embedding inside a double-quoted embr string literal. only used to hand the CLI's own argv
// (untrusted process arguments, not script source) to the pkg.embr module as a real array literal (see
// runPkgCommand below), instead of splicing argv text into code
static std::string escapeEmbrStringLiteral(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// exit code from pkg_main(), set by the __cli_set_exit binding below.
// static storage (not a captured local) so the no-capture lambda passed to
// bindSig can reference it directly, the same way it could reference any
// other file-scope state.
static int g_pkgExitCode = 0;

// `embr pkg <args...>`: runs modules/pkg.embr's pkg_main(argv) with the remaining arguments (after "pkg", minus
// --vm) as an embr array of strings. it's a tiny generated script, not a C++ dispatcher, so the package manager's
// command handling lives in one place (pkg_main), like modules/ffi.embr owns ffi_cdef's parsing
static int runPkgCommand(const std::vector<std::string>& pkgArgs, embr::Interpreter& interp) {
    interp.bindSig("__cli_set_exit", {embr::Param::req("code", embr::TS::Num)},
    [](const std::vector<embr::Value>& args) -> embr::Value {
        g_pkgExitCode = (int)args[0].asNumber();
        return embr::Value(0.0);
    });

    std::string src = "import \"pkg.embr\"\n__cli_set_exit(pkg_main([";
    for (size_t i = 0; i < pkgArgs.size(); ++i) {
        if (i) src += ", ";
        src += "\"" + escapeEmbrStringLiteral(pkgArgs[i]) + "\"";
    }
    src += "]))\n";

    runSourceDispatch(src, interp, "<pkg>");
    return g_pkgExitCode;
}

static void runFile(const std::string& path, embr::Interpreter& interp) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Error: cannot open: " << path << "\n"; return; }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    interp.scriptDir = fs::absolute(path).parent_path().string();
    runSourceDispatch(src, interp, path);
}

// every lex, parse and (on a VM build) compile error in src, as data. used by `embr check` and `embr lsp`
// nothing is run and no plugin is loaded (imports aren't resolved)
//
// `embr check FILE...` prints each error as `file:line:col: message` (plain text, for editors and CI). the parser
// recovers after an error, so several can be reported per file. returns the number of files with errors
static std::vector<embr::EmbrError> diagnose(const std::string& src, const std::string& path) {
    std::vector<embr::EmbrError> errors;
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(tokens), sm);
        parser.collectErrors(&errors);
        auto prog = parser.parse();
#ifdef EMBR_WITH_VM
        if (errors.empty()) {          // compile-time errors (e.g. a stray break) only matter once it parses
            embr::Interpreter interp;
            embr::vm::compileSource(src, interp, path);
        }
#else
        (void)path;
#endif
    } catch (const embr::EmbrError& e) {
        errors.push_back(e);          // lexer errors stop tokenizing, so there is exactly one
    }
    return errors;
}

static int checkFiles(const std::vector<std::string>& paths) {
    int bad = 0;
    for (const auto& path : paths) {
        std::ifstream f(path);
        if (!f) { std::cerr << path << ": error: cannot open file\n"; ++bad; continue; }
        std::string src((std::istreambuf_iterator<char>(f)), {});

        auto loc = [&](const embr::EmbrError& e) {
            std::string out = path;
            if (e.hasLocation) out += ":" + std::to_string(e.range.startLine) + ":" + std::to_string(e.range.startCol);
            return out + ": " + e.context + (e.context.empty() ? "" : ": ") + e.message;
        };
        auto errors = diagnose(src, path);
        for (const auto& e : errors) std::cerr << loc(e) << "\n";
        if (!errors.empty()) ++bad;
    }
    return bad;
}

// `embr fmt [--check] [--stdout] [--indent=N] FILE...`: formats each file in place (see fmt.h). --check writes nothing and
// exits 1 if any file would change; --stdout prints the result instead of writing it. exit status 2 if a file
// can't be formatted (it doesn't lex, or can't be read).
static int runFmtCommand(const std::vector<std::string>& args) {
    bool check = false, toStdout = false;
    int indent = 4;
    std::vector<std::string> paths;
    for (const auto& a : args) {
        if (a.rfind("--indent=", 0) == 0) {
            indent = std::atoi(a.c_str() + 9);
            if (indent < 1 || indent > 16) { std::cerr << "embr fmt: --indent must be 1 to 16\n"; return 2; }
        }
        else if (a == "--check") check = true;
        else if (a == "--stdout") toStdout = true;
        else paths.push_back(a);
    }
    if (paths.empty()) { std::cerr << "usage: embr fmt [--check] [--stdout] [--indent=N] FILE...\n"; return 2; }
    int status = 0;
    for (const auto& path : paths) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { std::cerr << path << ": error: cannot open file\n"; status = 2; continue; }
        std::string src((std::istreambuf_iterator<char>(f)), {});
        std::string out;
        try {
            out = embr_fmt::formatSource(src, indent);
        } catch (const embr::EmbrError& e) {
            std::cerr << path << ": error: " << e.context << ": " << e.message << "\n";
            status = 2; continue;
        } catch (const embr_fmt::FormatError& e) {
            std::cerr << path << ": error: " << e.what() << " (file left untouched)\n";
            status = 2; continue;
        }
        if (toStdout) { std::cout << out; continue; }
        if (out == src) continue;
        if (check) { std::cout << path << "\n"; if (status == 0) status = 1; continue; }
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o || !(o << out)) { std::cerr << path << ": error: cannot write file\n"; status = 2; }
    }
    return status;
}

// `embr test [--filter=TEXT] [PATH...]`: runs script tests. PATH is a file or a directory (searched recursively for
// test_*.embr / *_test.embr, default "."). each file gets a fresh interpreter (on the backend chosen with --vm), and
// every top-level function named test_* runs on its own inside a try/catch, in name order, so one failure doesn't
// stop the rest. an optional setup() runs before each test and teardown() after it (also after a failure). a file
// that fails to parse, or whose top level raises, counts as one failed test. prints PASS/FAIL per test (with the
// error and call trace) and a summary. exit status: 0 all passed, 1 any failure, 2 nothing to run / usage.
// assertions are in modules/testing.embr
struct TestTotals { int passed = 0, failed = 0; };

static void runTestFile(const fs::path& file, const std::string& filter, TestTotals& tot) {
    std::cout << file.string() << "\n";
    auto failFile = [&](const std::string& why) {
        std::cout << "  FAIL (while loading)\n";
        std::istringstream ls(why);
        for (std::string line; std::getline(ls, line); ) std::cout << "        " << line << "\n";
        ++tot.failed;
    };
    std::ifstream f(file);
    if (!f) { failFile("cannot open file"); return; }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    embr::Interpreter interp;
    interp.pluginPolicy = g_policy;
    interp.scriptDir = fs::absolute(file).parent_path().string();

#ifdef EMBR_WITH_VM
    std::unique_ptr<embr::vm::CompiledProgram> compiled;   // must outlive the runner that executes it
    std::unique_ptr<embr::vm::VmRunner>        vmRunner;
#endif
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        std::vector<embr::EmbrError> perr;
        embr::Parser parser(std::move(tokens), sm);
        parser.collectErrors(&perr);
        auto prog = parser.parse();
        if (!perr.empty()) {
            std::string why;
            for (const auto& e : perr) why += e.message + (e.hasLocation ? " (line " + std::to_string(e.range.startLine) + ")" : "") + "\n";
            failFile(why); return;
        }
        if (g_useVM) {
#ifdef EMBR_WITH_VM
            compiled = std::make_unique<embr::vm::CompiledProgram>(embr::vm::compileSource(src, interp, file.string()));
            vmRunner = std::make_unique<embr::vm::VmRunner>(interp);
            vmRunner->run(*compiled);
#else
            failFile("this build has no VM backend"); return;
#endif
        } else {
#ifdef EMBR_WITH_TREE_WALKER
            const auto* stable = interp.storeProgram(std::move(prog));
            embr::Runner runner(interp);
            runner.run(*stable, sm, file.string());
#else
            failFile("this build has no tree-walker backend; pass --vm"); return;
#endif
        }
    } catch (const embr::EmbrError& e) {
        failFile(e.message + embr::formatTrace(e)); return;
    }

    std::vector<std::string> names;
    for (const auto& [name, val] : interp.globals())
        if (name.rfind("test_", 0) == 0 && val.isCallable() &&
            (filter.empty() || name.find(filter) != std::string::npos))
            names.push_back(name);
    std::sort(names.begin(), names.end());
    if (names.empty()) { std::cout << "  (no test_* functions" << (filter.empty() ? "" : " matching the filter") << ")\n"; return; }

    auto call = [&](const std::string& fn) { embr::invoke(interp, interp.getGlobal(fn), {}); };
    const bool hasSetup = interp.hasGlobal("setup") && interp.getGlobal("setup").isCallable();
    const bool hasTeardown = interp.hasGlobal("teardown") && interp.getGlobal("teardown").isCallable();
    for (const auto& name : names) {
        bool ok = true;
        std::string msg, trace;
        try {
            if (hasSetup) call("setup");
            call(name);
        } catch (const embr::EmbrError& e) { ok = false; msg = e.message; trace = embr::formatTrace(e); }
        if (hasTeardown) {
            try { call("teardown"); }
            catch (const embr::EmbrError& e) { if (ok) { ok = false; msg = "teardown failed: " + e.message; trace = embr::formatTrace(e); } }
        }
        if (ok) { std::cout << "  PASS " << name << "\n"; ++tot.passed; continue; }
        std::cout << "  FAIL " << name << "\n";
        std::istringstream ms(msg + "\n" + trace);
        for (std::string line; std::getline(ms, line); ) if (!line.empty()) std::cout << "        " << line << "\n";
        ++tot.failed;
    }
}

static int runTestCommand(const std::vector<std::string>& args) {
    std::string filter;
    std::vector<fs::path> roots;
    for (const auto& a : args) {
        if (a.rfind("--filter=", 0) == 0) filter = a.substr(9);
        else roots.emplace_back(a);
    }
    if (roots.empty()) roots.emplace_back(".");

    std::vector<fs::path> files;
    for (const auto& r : roots) {
        std::error_code ec;
        if (fs::is_directory(r, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(r, ec)) {
                if (!e.is_regular_file()) continue;
                std::string n = e.path().filename().string();
                auto endsWith = [&](const std::string& suffix) {
                    return n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                bool isTest = endsWith(".embr") && (n.rfind("test_", 0) == 0 || endsWith("_test.embr"));
                if (isTest) files.push_back(e.path());
            }
        } else if (fs::is_regular_file(r, ec)) files.push_back(r);
        else { std::cerr << "embr test: no such file or directory: " << r.string() << "\n"; return 2; }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::cerr << "embr test: no test files found (test_*.embr or *_test.embr)\n"; return 2; }

    TestTotals tot;
    for (const auto& f : files) runTestFile(f, filter, tot);
    std::cout << "\n" << tot.passed << " passed, " << tot.failed << " failed\n";
    return tot.failed ? 1 : 0;
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
// on POSIX, RawMode puts the tty in non-canonical, no-echo mode while it exists and restores it when destroyed
// on Windows _getch() already reads unbuffered and unechoed, so RawMode does nothing
// arrow keys arrive as ESC '[' 'A'/'B'/'C'/'D' on POSIX, and as a 0/224 prefix byte then 72/80/75/77 on Windows
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
// multi-line editor: used once a block opener (if/fn/while) has been typed
// up/down move between lines, left/right move within and across lines, like a small text editor. Enter splits
// the current line. down on the last line makes a temporary blank line below. Enter on an empty last line
// (with balanced block depth) runs the whole buffer
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
// true if src lexes and parses with no errors. the REPL uses it to ask "is this line an expression?"
static bool parsesClean(const std::string& src) {
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(tokens), sm);
        std::vector<embr::EmbrError> errors;
        parser.collectErrors(&errors);
        parser.parse();
        return errors.empty();
    } catch (const embr::EmbrError&) {
        return false;
    }
}

static bool stdinIsTty() {
#ifdef _WIN32
    return _isatty(0) != 0;
#else
    return isatty(0) != 0;
#endif
}

// a call like print(x) returns 0 because every native needs some value; echoing that 0 would be noise
static bool looksLikeCall(const std::string& line) {
    return !line.empty() && line.back() == ')' &&
           (std::isalpha((unsigned char)line[0]) || line[0] == '_');
}

static void repl(embr::Interpreter& interp) {
    const bool tty = stdinIsTty();
    if (tty) std::cout << "embr REPL  (type 'exit' to quit)\n";
    std::vector<std::string> history;

    // prints the value of a bare expression line, see the wrapping in the loop below
    bool echoZero = true;
    interp.bindSig("__repl_show", {embr::Param::req("val")},
    [&echoZero](const std::vector<embr::Value>& args) -> embr::Value {
        const embr::Value& v = args[0];
        if (!(v.isNumber() && v.asNumber() == 0.0 && !echoZero))
            std::cout << embr::valueRepr(v) << "\n";
        return embr::Value(0.0);
    });

    while (true) {
        LineResult first;
        if (tty) {
            first = readLineSingle(history, ">>> ");
        } else {
            // piped input: no raw mode, no prompt, no keystroke echo
            if (!std::getline(std::cin, first.text)) first.eof = true;
        }
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
            // a bare expression prints its value: wrap it in __repl_show(...) when that still parses
            // (statements like `x = 1` or `if ...` don't, so they run as written)
            std::string wrapped = "__repl_show(" + line + ")\n";
            if (parsesClean(wrapped)) {
                echoZero = !looksLikeCall(line);
                source = wrapped;
            }
        } else if (!tty) {
            // piped input: keep reading lines until the block closes
            source = line + "\n";
            std::string more;
            while (depth > 0 && std::getline(std::cin, more)) {
                source += more + "\n";
                depth += blockDelta(more);
            }
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
    // print() writes to a buffered stdout, which a crash or a kill throws away. EMBR_UNBUFFERED=1 flushes every
    // write, so the output of a script that hangs or crashes shows how far it got
    if (const char* ub = std::getenv("EMBR_UNBUFFERED"); ub && *ub == '1') std::cout << std::unitbuf;
    embr::Interpreter interp;
#ifdef EMBR_WITH_VM
    // declared after interp, so it is destroyed BEFORE it on every return path (locals die in reverse order)
    struct VmRunnerRelease { ~VmRunnerRelease() { g_vmRunner.reset(); } } vmRunnerRelease;
#endif
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
        if (arg == "--" && !files.empty() && files[0] != "pkg") {
            for (++i; i < argc; ++i) interp.scriptArgs.push_back(argv[i]);
            break;
        }
        if (arg == "--sandbox") {
            g_policy.restricted = true; g_policy.fileIo = false;
            for (const char* n : kSandboxPlugins) g_policy.allowed.insert(n);
            continue;
        }
        if (arg == "--no-plugins") { g_policy.restricted = true; g_policy.fileIo = false; g_policy.allowed.clear(); continue; }
        if (arg.rfind("--allow-plugins=", 0) == 0) {
            // an exact allow-list; does not add the sandbox set and keeps embrlib's file natives ON
            // unless --sandbox/--no-plugins also appears (combine them for both)
            if (!g_policy.restricted) g_policy.fileIo = true;
            g_policy.restricted = true;
            g_policy.allowed.clear();
            std::string list = arg.substr(std::string("--allow-plugins=").size()), cur;
            for (char ch : list + ",") {
                if (ch == ',') { if (!cur.empty()) g_policy.allowed.insert(cur); cur.clear(); }
                else cur += ch;
            }
            continue;
        }
        if (arg == "--version") {
            std::cout << "embr " EMBR_VERSION " (build/" EMBR_BUILD_NAME "; backends: " << backendSummary() << ")\n";
            return 0;
        }
        if (arg == "--vm") g_useVM = true;
        else               files.push_back(std::move(arg));
    }
    interp.pluginPolicy = g_policy;
    if (!files.empty() && files[0] == "test") {
        return runTestCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "check") {
        if (files.size() < 2) { std::cerr << "usage: embr check FILE...\n"; return 2; }
        return checkFiles(std::vector<std::string>(files.begin() + 1, files.end())) ? 1 : 0;
    }
    if (!files.empty() && files[0] == "lsp") {
        embr_lsp::Server server(std::cin, std::cout, diagnose);
        return server.run();
    }
    if (!files.empty() && files[0] == "fmt") {
        return runFmtCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "pkg") {
        std::vector<std::string> pkgArgs(files.begin() + 1, files.end());
        return runPkgCommand(pkgArgs, interp);
    }
    if (!files.empty()) {
        for (const auto& f : files) runFile(f, interp);
    } else {
        repl(interp);
    }
    return 0;
}
