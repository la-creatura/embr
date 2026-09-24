#ifndef EMBR_BACKENDS_TREE_WALKER_H
#define EMBR_BACKENDS_TREE_WALKER_H

// the original, portable, easy-to-embed executor: walks the AST directly.
// this is what runSource() drives by default. see backends/vm.h (when
// built) for the bytecode alternative aimed at standalone/performance use.

#include "../core/diagnostics.h"
#include "../core/types.h"
#include "../core/value.h"
#include "../core/ast.h"
#include "../core/registry.h"
#include "../core/lexer.h"
#include "../core/parser.h"
#include "../core/platform.h"

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <numeric>

namespace embr {

struct ReturnSignal { Value value; };

class Runner {
public:
    explicit Runner(Interpreter& interpreter) : interp(interpreter) {}

    void run(const std::vector<StmtPtr>& prog) {
        for (auto& s : prog) {
            try { exec(s.get()); }
            catch (const EmbrError& e) { std::cerr << e.what(); }
        }
    }
    void run(const std::vector<StmtPtr>& prog, const SourceMap& src,
             const std::string& filename = "<input>") {
        interp.setSource(src, filename);
        run(prog);
    }

    // public invoke for embrlib
    Value invoke(const Value& callee, const std::vector<Value>& args,
                 const SourceRange& site = {}) {
        return doInvoke(callee, args, site);
    }

    // public entry point for load_module in embrlib
    void importEmbrModule(const std::string& path, const SourceRange& site) {
        execImportEmbrModule(path, site);
    }

    Interpreter& interp;
private:
    [[noreturn]] void error(const std::string& msg, const SourceRange& r = {}) const {
        raiseError("runtime", msg, r, interp.sourceMap());
    }

    static size_t editDistance(
        const std::string& a,
        const std::string& b,
        size_t maxDist = 3)
    {
        const size_t na = a.size();
        const size_t nb = b.size();

        if (na > nb + maxDist || nb > na + maxDist)
            return maxDist + 1;

        std::vector<std::vector<size_t>> dp(na + 1, std::vector<size_t>(nb + 1));

        for (size_t i = 0; i <= na; ++i)
            dp[i][0] = i;

        for (size_t j = 0; j <= nb; ++j)
            dp[0][j] = j;

        for (size_t i = 1; i <= na; ++i) {
            size_t rowMin = SIZE_MAX;

            for (size_t j = 1; j <= nb; ++j) {
                const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;

                size_t v = std::min({
                    dp[i - 1][j] + 1,        // deletion
                    dp[i][j - 1] + 1,        // insertion
                    dp[i - 1][j - 1] + cost  // substitution
                });

                if (i > 1 && j > 1 &&        // trans
                    a[i - 1] == b[j - 2] &&
                    a[i - 2] == b[j - 1])
                {
                    v = std::min(v, dp[i - 2][j - 2] + 1);
                }

                dp[i][j] = v;
                rowMin = std::min(rowMin, v);
            }

            if (rowMin > maxDist)
                return maxDist + 1;
        }

        return dp[na][nb];
    }

    std::string suggest(const std::string& name) const {
        std::string best; size_t bestDist = 3;
        for (auto& sc : interp.allScopes()) {
            for (auto& [k, _] : sc) {
                size_t d = editDistance(name, k, bestDist);
                if (d < bestDist) { bestDist = d; best = k; }
            }
        }
        return best;
    }

    [[noreturn]] void errorUndefined(const std::string& kind,
                                     const std::string& name,
                                     const SourceRange& r) const {
        std::string msg = "undefined " + kind + ": " + name;
        std::string hint = suggest(name);
        if (!hint.empty()) msg += "\n  did you mean '" + hint + "'?";
        raiseError("runtime", msg, r, interp.sourceMap());
    }

    Value doInvoke(const Value& callee, const std::vector<Value>& args,
                   const SourceRange& site) {
        if (!callee.isCallable())
            error("value is not callable (got " + callee.typeName() + ")", site);
        const auto& c = callee.asCallable();

        if (c.isNative()) {
            if (!c.sig.empty()) {
                checkNativeSig(c.name(), c.sig, args, site);
            }
            try {
                return c.native(args);
            } catch (EmbrError& e) {
                if (!e.hasLocation && site.valid()) {
                    raiseError(c.name(), e.what(), site, interp.sourceMap());
                }
                throw;
            }
        }
        const ScriptFn& fn = c.script;
        if (args.size() < fn.params.size())
            error("'" + fn.name + "' expects " + std::to_string(fn.params.size()) +
                  " args, got " + std::to_string(args.size()), site);
        // typecheck
        for (size_t k = 0; k < fn.params.size() && k < args.size(); ++k) {
            const auto& p = fn.params[k];
            if (p.type.isAny()) continue;
            if (!p.type.contains(args[k].tag()))
                error("argument '" + p.name + "' to '" + fn.name + "': expected " +
                      p.type.name() + " but got " + args[k].typeName(), site);
        }

        // push capture frame first (outermost), then a fresh param scope on top
        // params shadow captured names
        // mutations to captured names inside the body update the body's capture scope (not the original frame)
        if (fn.captured) {
            interp.pushCapture(fn.captured);  // capture layer
            interp.push();                    // param layer
        } else {
            interp.push();                    // just a normal scope
        }
        // use a manual guard that pops the right number of scopes
        struct MultiPop {
            Interpreter& i; int n;
            ~MultiPop() { while (n-- > 0) i.pop(); }
        } guard{interp, fn.captured ? 2 : 1};

        for (size_t k = 0; k < fn.params.size(); ++k)
            interp.define(fn.params[k].name, args[k]);
        Value result(0.0);

        try { for (auto& s : *fn.body) exec(s.get()); }
        catch (ReturnSignal& ret) { result = std::move(ret.value); }
        // enforce return type
        if (!fn.returnType.isAny() && !fn.returnType.contains(result.tag()))
            error("'" + fn.name + "' declared return type " +
                  fn.returnType.name() + " but returned " + result.typeName(), site);
        return result;
    }

    // validates args against a native sig
    void checkNativeSig(const std::string& fname,
                        const std::vector<Param>& sig,
                        const std::vector<Value>& args,
                        const SourceRange& site) {
        // count required params and find variadic slot
        size_t minArgs = 0;
        bool   hasVariadic = false;
        for (const auto& p : sig) {
            if (p.variadic) { hasVariadic = true; break; }
            if (!p.optional) ++minArgs;
        }
        size_t maxArgs = hasVariadic ? SIZE_MAX : sig.size();

        if (args.size() < minArgs) {
            // build person-readable sig
            std::string sig_str = buildSigString(fname, sig);
            error("too few arguments to '" + fname + "': expected " +
                  std::to_string(minArgs) + " but got " +
                  std::to_string(args.size()) + "\n  signature: " + sig_str, site);
        }
        if (args.size() > maxArgs) {
            std::string sig_str = buildSigString(fname, sig);
            error("too many arguments to '" + fname + "': expected at most " +
                  std::to_string(maxArgs) + " but got " +
                  std::to_string(args.size()) + "\n  signature: " + sig_str, site);
        }

        // typecheck pos args
        for (size_t i = 0; i < args.size(); ++i) {
            const Param* p = (i < sig.size() && !sig[i].variadic) ? &sig[i] : nullptr;
            if (!p)
                for (auto it = sig.rbegin(); it != sig.rend(); ++it)
                    if (it->variadic) { p = &*it; break; }
            if (!p || p->type.isAny()) continue;
            if (!p->type.contains(args[i].tag()))
                error("argument '" + p->name + "' to '" + fname + "': expected " +
                      p->type.name() + " but got " + args[i].typeName(), site);
        }
    }

    static std::string buildSigString(const std::string& fname, const std::vector<Param>& sig) {
        std::string s = fname + "(";
        for (size_t i = 0; i < sig.size(); ++i) {
            if (i) s += ", ";
            const auto& p = sig[i];
            if (p.variadic) s += "...";
            s += p.name;
            if (!p.type.isAny()) s += ": " + p.type.name();
            if (p.optional && !p.variadic) s += "?";
        }
        return s + ")";
    }

    Value eval(Expr* e) {
        switch (e->kind) {
            case Expr::Kind::Number: return Value(static_cast<NumberExpr*>(e)->v);
            case Expr::Kind::String: return Value(static_cast<StringExpr*>(e)->v);
            case Expr::Kind::Var: {
                auto* v = static_cast<VarExpr*>(e);
                if (!interp.has(v->name)) errorUndefined("variable", v->name, v->range);
                return interp.get(v->name);
            }
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                Value r = eval(u->right.get());
                if (u->op == "-") return Value(-r.asNumber());
                if (u->op == "!") return Value(!r.truthy());
                error("unknown unary op: " + u->op, u->range);
            }
            case Expr::Kind::Binary: return evalBinary(static_cast<BinaryExpr*>(e));
            case Expr::Kind::Call:   return evalCall(static_cast<CallExpr*>(e));
            case Expr::Kind::Array: {
                auto* a = static_cast<ArrayExpr*>(e);
                Value::array_type vals; vals.reserve(a->elements.size());
                for (auto& el : a->elements) vals.push_back(eval(el.get()));
                return Value(std::move(vals));
            }
            case Expr::Kind::Map: {
                auto* m = static_cast<MapExpr*>(e);
                Value::map_type res;
                for (auto& [kx,vx] : m->entries) {
                    Value k = eval(kx.get());
                    if (!k.isString()) error("map keys must be strings. use str() if the index is an expression", kx->range);
                    res[k.asString()] = eval(vx.get());
                }
                return Value(std::move(res));
            }
            case Expr::Kind::And: {
                auto* log = static_cast<LogicExpr*>(e);
                Value l = eval(log->left.get());
                if (!l.truthy()) return l;
                return eval(log->right.get());
            }
            case Expr::Kind::Or: {
                auto* log = static_cast<LogicExpr*>(e);
                Value l = eval(log->left.get());
                if (l.truthy()) return l;
                return eval(log->right.get());
            }
            case Expr::Kind::Lambda: {
                auto* lam = static_cast<LambdaExpr*>(e);
                ScriptFn sf;
                sf.name       = "<lambda>";
                sf.params     = lam->params;
                sf.returnType = lam->returnType;
                sf.sourceFile = interp.currentFile();
                sf.body       = &lam->ownedBody;  // stable: lam is in ownedPrograms_
                sf.captured   = interp.captureLocals();
                return Value::makeScript(std::move(sf));
            }
            case Expr::Kind::Index: return evalIndex(static_cast<IndexExpr*>(e));
        }
        error("unhandled expression kind", e->range);
    }

    Value evalBinary(BinaryExpr* b) {
        Value l = eval(b->left.get()), r = eval(b->right.get());
        const auto& op = b->op;
        if (l.isString()||r.isString()) {
            std::string a=l.formatAsString(), c=r.formatAsString();
            if (op=="+")  return Value(a+c);
        } else if (l.isNumber()&&r.isNumber()) {
            double a=l.asNumber(), c=r.asNumber();
            if (op=="+")  return Value(a+c);
            if (op=="-")  return Value(a-c);
            if (op=="*")  return Value(a*c);
            if (op=="/")  { if(c==0.0) error("division by zero",b->range); return Value(a/c); }
            if (op=="%")  { if(c==0.0) error("modulo by zero",b->range);   return Value(std::fmod(a,c)); }
            if (op==">")  return Value(a>c);
            if (op=="<")  return Value(a<c);
            if (op==">=") return Value(a>=c);
            if (op=="<=") return Value(a<=c);
        }
        if (!(l.isCallable()||r.isCallable())) {
            if (op=="==") return Value(l==r);
            if (op=="!=") return Value(l!=r);
        }
        error("unsupported operator "+op+" between "+l.typeName()+" and "+r.typeName(), b->range);
    }

    Value evalIndex(IndexExpr* idx) {
        Value cont = eval(idx->object.get()), key = eval(idx->index.get());
        if (cont.isString()) {
            int ii = (int)key.asNumber(); const auto& s = cont.asString();
            if (ii<0||ii>=(int)s.size()) error("string index "+std::to_string(ii)+" out of bounds. make sure the index is valid",idx->range);
            return Value(std::string(1,s[ii]));
        }
        if (cont.isMap()) {
            if (!key.isString()) error("map key must be string. use str() if the index is an expression",idx->range);
            const auto& m = cont.asMap(); auto it = m.find(key.asString());
            if (it==m.end()) error("key not found: "+key.asString(),idx->range);
            return it->second;
        }
        if (cont.isArray()) {
            int ii=(int)key.asNumber(); const auto& a=cont.asArray();
            if (ii<0||ii>=(int)a.size()) error("array index "+std::to_string(ii)+" out of bounds. make sure the index is valid",idx->range);
            return a[ii];
        }
        error("cannot index "+cont.typeName(),idx->range);
    }

    Value evalCall(CallExpr* call) {
        Value callee = eval(call->callee.get());

        std::vector<Value> args;
        args.reserve(call->args.size());

        for (auto& a : call->args)
            args.push_back(eval(a.get()));

        return doInvoke(callee, args, call->range);
    }

    void exec(Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::Local: {
                auto* l = static_cast<LocalStmt*>(s);
                interp.define(l->name, eval(l->value.get()));
                // track as module-local so it is excluded from exports
                if (interp.atModuleTopScope())
                    interp.markModuleLocal(l->name);

                break;
            }
            case Stmt::Kind::Assign: execAssign(static_cast<AssignStmt*>(s)); break;
            case Stmt::Kind::If: {
                auto* i = static_cast<IfStmt*>(s);
                Interpreter::ScopeGuard guard(interp);
                auto& blk = eval(i->cond.get()).truthy() ? i->thenBlock : i->elseBlock;
                for (auto& st : blk) exec(st.get());
                break;
            }
            case Stmt::Kind::Fn: {
                auto* f = static_cast<FnStmt*>(s);
                ScriptFn sf;
                sf.name       = f->name;
                sf.params     = f->params;
                sf.returnType = f->returnType;
                sf.definedAt  = f->range;
                sf.sourceFile = interp.currentFile();
                sf.body       = &f->body;
                sf.captured   = interp.captureLocals();
                interp.defineScriptFn(std::move(sf));
                break;
            }
            case Stmt::Kind::Return:
                throw ReturnSignal{eval(static_cast<ReturnStmt*>(s)->value.get())};
            case Stmt::Kind::Expr:
                eval(static_cast<ExprStmt*>(s)->expr.get());
                break;
            case Stmt::Kind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                while (eval(w->cond.get()).truthy()) {
                    Interpreter::ScopeGuard guard(interp);
                    for (auto& st : w->body) exec(st.get());
                }
                break;
            }
            case Stmt::Kind::Import: {
                auto* im = static_cast<ImportStmt*>(s);
                execImport(im->path, im->range);
                break;
            }
        }
    }

    void execAssign(AssignStmt* a) {
        if (a->target->kind == Expr::Kind::Var) {
            interp.set(static_cast<VarExpr*>(a->target.get())->name,
                        eval(a->value.get()));
            return;
        }
        if (a->target->kind == Expr::Kind::Index) {
            writeBack(a->target.get(), eval(a->value.get()), a->range);
            return;
        }
        error("invalid assignment target", a->range);
    }

    void writeBack(Expr* target, Value v, const SourceRange& r) {
        if (target->kind == Expr::Kind::Var) {
            interp.set(static_cast<VarExpr*>(target)->name, std::move(v));
            return;
        }
        if (target->kind == Expr::Kind::Index) {
            auto* idx = static_cast<IndexExpr*>(target);
            Value cont = eval(idx->object.get());
            Value key  = eval(idx->index.get());
            if (cont.isString()) {
                int ii = (int)key.asNumber();
                auto str = cont.asString();
                if (ii < 0 || ii >= (int)str.size())
                    error("string index out of bounds. make sure the index is valid", r);
                auto val = v.asString();
                str.replace(ii, val.size(), val);
                writeBack(idx->object.get(), Value(std::move(str)), r);  // recurse
                return;
            }
            if (cont.isArray()) {
                int ii = (int)key.asNumber();
                auto arr = cont.asArray();
                if (ii < 0 || ii >= (int)arr.size())
                    error("array index out of bounds. make sure the index is valid", r);
                arr[ii] = std::move(v);
                writeBack(idx->object.get(), Value(std::move(arr)), r);  // recurse
                return;
            }
            if (cont.isMap()) {
                if (!key.isString()) error("map key must be string. use str() if the index is an expression", r);
                auto m = cont.asMap();
                m[key.asString()] = std::move(v);
                writeBack(idx->object.get(), Value(std::move(m)), r);    // recurse
                return;
            }
            error("index assignment on " + cont.typeName(), r);
        }
        error("cannot assign to temporary", r);
    }

    void execImport(const std::string& rawPath, const SourceRange& r) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);

        const bool isEmbrModule = (p.extension() == ".embr");

        if (isEmbrModule) {
            execImportEmbrModule(rawPath, r);
        } else {
            execImportPlugin(rawPath, r);
        }
    }

    // import an embr script as a module
    //
    // after the module runs, its non-local top level bindings are exported into the importing scope and the module's scope is deleted
    // multiple imports of the same path are no-ops
    void execImportEmbrModule(const std::string& rawPath, const SourceRange& r) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);

        auto withEmbr = [](fs::path base) -> fs::path {
            if (base.extension() == ".embr") return base;
            return fs::path(base.string() + ".embr");
        };

        std::vector<fs::path> cands;
        if (p.is_absolute()) {
            cands.push_back(withEmbr(p));
        } else if (p.has_parent_path()) {
            fs::path withE = withEmbr(p);
            if (!interp.scriptDir.empty())
                cands.push_back((fs::path(interp.scriptDir) / withE).lexically_normal());
            cands.push_back((fs::current_path() / withE).lexically_normal());
        } else {
            fs::path name = withEmbr(p);
            if (!interp.scriptDir.empty()) {
                fs::path base(interp.scriptDir);
                cands.push_back(base / name);
                cands.push_back(base / "modules" / name);
            }
            cands.push_back(fs::current_path() / name);
            cands.push_back(fs::current_path() / "modules" / name);
        }

        fs::path resolved;
        for (const auto& c : cands) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) { resolved = c; break; }
        }
        if (resolved.empty()) {
            std::string tried;
            for (const auto& c : cands) tried += "\n    " + c.string();
            error("cannot find module \"" + rawPath + "\"\n  tried:" + tried, r);
        }

        std::error_code ec;
        fs::path canon = fs::canonical(resolved, ec);
        if (ec) canon = resolved;
        const std::string canonStr = canon.string();

        // deduplication
        if (interp.loadedModules_.count(canonStr)) return;
        interp.loadedModules_.insert(canonStr);

        // read source
        std::ifstream f(resolved);
        if (!f) error("cannot open module file: " + resolved.string(), r);
        std::string src((std::istreambuf_iterator<char>(f)), {});

        // save interpreter context fields that runSource overwrites
        std::string savedDir  = interp.scriptDir;
        SourceMap   savedMap  = interp.sourceMap();
        std::string savedFile = interp.currentFile();

        interp.scriptDir = resolved.parent_path().string();

        // push a dedicated module scope
        interp.pushModuleScope();

        // parse & run inside the module scope
        try {
            Lexer     lex(src);
            auto      tokens = lex.tokenize();
            SourceMap sm     = lex.sourceMap();
            Parser    parser(std::move(tokens), sm);
            auto      prog   = parser.parse();
            const std::vector<StmtPtr>* stable = interp.storeProgram(std::move(prog));
            run(*stable, sm, resolved.string());
        } catch (...) {
            // pop module scope even on error, then restore context and rethrow
            interp.popModuleScope();  // discard result on error
            interp.scriptDir = savedDir;
            interp.setSource(savedMap, savedFile);
            throw;
        }

        // pop module scope and collect the exported bindings
        auto [moduleScope, moduleLocals] = interp.popModuleScope();

        // restore interpreter context
        interp.scriptDir = savedDir;
        interp.setSource(savedMap, savedFile);

        // export: copy non-local bindings into the importer's current scope
        for (auto& [name, val] : moduleScope) {
            if (!moduleLocals.count(name))
                interp.define(name, std::move(val));
        }
    }

    // import a native plugin (.so / .dll / .dylib)
    void execImportPlugin(const std::string& rawPath, const SourceRange& r) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);

        auto withExt = [&](fs::path base) -> fs::path {
            if (base.extension() == PLUGIN_EXT) return base;
            return fs::path(base.string() + PLUGIN_EXT);
        };

        std::vector<fs::path> cands;

        if (p.is_absolute()) {
            cands.push_back(withExt(p));

        } else if (p.has_parent_path()) {
            fs::path withE = withExt(p);
            if (!interp.scriptDir.empty())
                cands.push_back((fs::path(interp.scriptDir) / withE).lexically_normal());
            cands.push_back((fs::current_path() / withE).lexically_normal());

        } else {
            fs::path name = withExt(p);  // just the filename
            if (!interp.scriptDir.empty()) {
                fs::path base(interp.scriptDir);
                cands.push_back(base / name);                   // next to the script
                cands.push_back(base / "plugins" / name);       // plugins/ subdir
            }
            cands.push_back(fs::current_path() / name);         // cwd
            cands.push_back(fs::current_path() / "plugins" / name); // cwd/plugins/
        }

        PluginHandle handle = nullptr;
        std::string  loadedFrom;
        for (const auto& c : cands) {
            handle = pluginOpen(c.string().c_str());
            if (handle) { loadedFrom = c.string(); break; }
        }

        if (!handle) {
            // list of tried paths for the error message
            std::string tried;
            for (const auto& c : cands) tried += "\n    " + c.string();
            error("cannot load plugin \"" + rawPath + "\": " + pluginError() +
                  "\n  tried:" + tried, r);
        }

        using RegFn = void(*)(Interpreter*);
        auto reg = reinterpret_cast<RegFn>(pluginSym(handle, "embr_register"));
        if (!reg) {
            pluginClose(handle);
            error("'embr_register' not found in \"" + loadedFrom + "\"", r);
        }
        try {
            reg(&interp);
            interp.pluginHandles_.push_back(handle);
        }
        catch (...) {
            pluginClose(handle);
            throw;
        }
        std::cout << "[runtime] loaded plugin: " << loadedFrom << "\n";
    }
};

inline void runSource(const std::string& src, Interpreter& interp,
                      const std::string& filename = "<input>") {
    Lexer lex(src);
    auto tokens = lex.tokenize();
    SourceMap sm = lex.sourceMap();
    Parser parser(std::move(tokens), sm);
    auto prog = parser.parse();

    // transfer ownership before running
    const std::vector<StmtPtr>* stable = interp.storeProgram(std::move(prog));

    Runner runner(interp);
    runner.run(*stable, sm, filename);
}

} // namespace embr

#endif // EMBR_BACKENDS_TREE_WALKER_H
