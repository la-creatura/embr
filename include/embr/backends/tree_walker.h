#ifndef EMBR_BACKENDS_TREE_WALKER_H
#define EMBR_BACKENDS_TREE_WALKER_H

// portable, easy-to-embed backend which walks the AST
// see the vm backend for more performance

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

#ifdef EMBR_WITH_TREE_WALKER

namespace embr {

struct ReturnSignal   { Value value; };
struct BreakSignal    {};
struct ContinueSignal {};

class Runner {
public:
    explicit Runner(Interpreter& interpreter) : interp(interpreter) {}

    // definition files of the script functions currently executing, innermost last (see doInvoke)
    std::vector<const std::string*> fileStack_;

    // runs every top-level statement in order. an uncaught EmbrError stops the rest and goes to the caller,
    // there is no per-statement recovery (same as the VM). a script that wants to keep going after an
    // expected failure says so with `try ... catch e ... end`
    //
    // a `return` at top level (outside any fn) ends the script quietly, like on the VM. without a handler
    // the internal ReturnSignal would reach std::terminate and kill the process
    void run(const std::vector<StmtPtr>& prog) {
        try {
            for (auto& s : prog) exec(s.get());
        } catch (ReturnSignal&) {
        } catch (BreakSignal&) {       // the parser rejects these outside a loop; never let one reach std::terminate
            raiseError("runtime", "'break' outside a loop");
        } catch (ContinueSignal&) {
            raiseError("runtime", "'continue' outside a loop");
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
            for (auto& [k, _] : *sc) {
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
        if (!callee.isCallable()) {
            // dunder fallback: a typed pointer that isn't callable (a table handle, an ffi handle, ...) can still be
            // "called" if its tag has a __tag_call global (see resolveDunder() in core/registry.h). it is called as
            // __tag_call(pointer, ...originalArgs)
            Value dfn;
            if (resolveDunder(interp, callee, "call", dfn)) {
                std::vector<Value> dargs;
                dargs.reserve(args.size() + 1);
                dargs.push_back(callee);
                dargs.insert(dargs.end(), args.begin(), args.end());
                return doInvoke(dfn, dargs, site);
            }
            error("value is not callable (got " + callee.typeName() + ")", site);
        }
        Interpreter::CallDepthGuard depthGuard(interp, site);
        const auto& c = callee.asCallable();

        if (c.isNative()) {
            if (!c.sig.empty()) {
                checkNativeSig(c.name(), c.sig, args, site);
            }
            try {
                return c.native(args);
            } catch (EmbrError& e) {
                if (!e.hasLocation && site.valid()) {
                    // re-raise with call site location attached with raw message, carrying over the
                    // trace frames already collected if this native called back into script code
                    try { raiseError(c.name(), e.message, site, interp.sourceMap()); }
                    catch (EmbrError& located) { located.trace = std::move(e.trace); throw; }
                }
                throw;
            }
        }
        const ScriptFn& fn = c.script;
        // the file this call was made from: the body of whichever script function is running (its
        // definition file), or the current top-level/module file when we're not inside one. kept as a
        // stack of pointers so the cost on the hot path is a push/pop, not a string copy.
        const std::string* callerFile = fileStack_.empty() ? &interp.currentFile() : fileStack_.back();
        fileStack_.push_back(&fn.sourceFile);
        struct FilePop { std::vector<const std::string*>& s; ~FilePop() { s.pop_back(); } } filePop{fileStack_};
        bool   hasVariadic = !fn.params.empty() && fn.params.back().variadic;
        size_t fixedCount  = hasVariadic ? fn.params.size() - 1 : fn.params.size();

        if (args.size() < fixedCount)
            error("'" + fn.name + "' expects " + (hasVariadic ? "at least " : "") +
                  std::to_string(fixedCount) + " args, got " + std::to_string(args.size()), site);
        // typecheck the fixed params
        for (size_t k = 0; k < fixedCount; ++k) {
            const auto& p = fn.params[k];
            if (p.type.isAny()) continue;
            if (!p.type.contains(args[k].tag()))
                error("argument '" + p.name + "' to '" + fn.name + "': expected " +
                      p.type.name() + " but got " + args[k].typeName(), site);
        }
        // typecheck each element collected into the variadic tail, if it carries a type
        if (hasVariadic && !fn.params.back().type.isAny()) {
            const auto& vp = fn.params.back();
            for (size_t k = fixedCount; k < args.size(); ++k)
                if (!vp.type.contains(args[k].tag()))
                    error("argument " + std::to_string(k) + " to '" + fn.name + "' ('..." + vp.name +
                          "'): expected " + vp.type.name() + " but got " + args[k].typeName(), site);
        }

        // push every captured layer first (outermost to innermost, the same live Scope objects the closure
        // closed over, see CaptureFrame in core/value.h), then a fresh param scope on top. params shadow
        // captured names. changes to a captured name inside the body write into the shared layer, so other
        // closures sharing it see them
        if (fn.captured) {
            interp.pushCapture(fn.captured);  // every captured layer
            interp.push();                    // param layer
        } else {
            interp.push();                    // just a normal scope
        }
        // use a manual guard that pops the right number of scopes: one per
        // captured layer (if any), plus the param layer always pushed above.
        struct MultiPop {
            Interpreter& i; size_t n;
            ~MultiPop() { while (n-- > 0) i.pop(); }
        } guard{interp, (fn.captured ? fn.captured->layers.size() : 0) + 1};
        // undeclared assignments inside this call create their variable in this call's scope (see
        // Interpreter::assignFloor); restore the caller's floor however the call ends
        struct FloorRestore {
            Interpreter& i; size_t old;
            ~FloorRestore() { i.swapFuncFloor(old); }
        } floorRestore{interp, interp.swapFuncFloor(interp.topScopeIndex())};

        for (size_t k = 0; k < fixedCount; ++k)
            interp.define(fn.params[k].name, args[k]);
        if (hasVariadic) {
            Value::array_type rest(args.begin() + fixedCount, args.end());
            interp.define(fn.params.back().name, Value(std::move(rest)));
        }
        Value result(0.0);

        try { for (auto& s : *fn.body) exec(s.get()); }
        catch (ReturnSignal& ret) { result = std::move(ret.value); }
        catch (EmbrError& e) {
            // one trace frame per script function the error unwinds through (the VM's handleTry does the same, so both
            // backends report the same call stack): the function's name, and where it was called from (the caller's file and
            // this call's line, 0 if a native called back into script code, which has no script call site)
            e.trace.push_back({fn.name, *callerFile, site.valid() ? site.startLine : 0});
            throw;
        }
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
            const bool ref = isRef(args[i]);
            if (p && p->inout != ref)
                error(ref ? "argument '" + p->name + "' to '" + fname + "' is not an in-out parameter, so it can't take '&'"
                          : "argument '" + p->name + "' to '" + fname + "' is changed in place, so pass it as &name", site);
            if (!p && ref) error("'" + fname + "' can't take '&' for argument " + std::to_string(i + 1), site);
            if (!p || p->type.isAny()) continue;
            const Value& shown = ref ? refTarget(args[i]) : args[i];
            if (!p->type.contains(shown.tag()))
                error("argument '" + p->name + "' to '" + fname + "': expected " +
                      p->type.name() + " but got " + shown.typeName(), site);
        }
    }

    static std::string buildSigString(const std::string& fname, const std::vector<Param>& sig) {
        std::string s = fname + "(";
        for (size_t i = 0; i < sig.size(); ++i) {
            if (i) s += ", ";
            const auto& p = sig[i];
            if (p.variadic) s += "...";
            if (p.inout) s += "&";
            s += p.name;
            if (!p.type.isAny()) s += ": " + p.type.name();
            if (p.optional && !p.variadic) s += "?";
        }
        return s + ")";
    }

    Value eval(Expr* e) {
        switch (e->kind) {
            case Expr::Kind::Number: return Value(static_cast<NumberExpr*>(e)->v);
            case Expr::Kind::Int:    return Value(static_cast<IntExpr*>(e)->v);
            case Expr::Kind::String: return Value(static_cast<StringExpr*>(e)->v);
            case Expr::Kind::Var: {
                auto* v = static_cast<VarExpr*>(e);
                if (!interp.has(v->name)) errorUndefined("variable", v->name, v->range);
                return interp.get(v->name);
            }
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                Value r = eval(u->right.get());
                if (u->op == "-") {
                    if (r.isNumeric()) return r.isInt() ? intNeg(r.asInt()) : Value(-r.asNumber());
                    // dunder fallback: a typed pointer whose tag has a __tag_neg global (see resolveDunder() in core/registry.h)
                    // "!" has no fallback: truthy() already gives every value an answer, unlike unary '-' which can fail in asNumber()
                    Value dfn;
                    if (resolveDunder(interp, r, "neg", dfn)) return doInvoke(dfn, {r}, u->range);
                    error("unsupported operand for unary '-': " + r.typeName(), u->range);
                }
                if (u->op == "!") return Value(!r.truthy());
                error("unknown unary op: " + u->op, u->range);
            }
            case Expr::Kind::Ref: error("'&' only works on an argument of a call", e->range);
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
        } else if (l.isInt() && r.isInt()) {
            int64_t a=l.asInt(), c=r.asInt();
            if (op=="+")  return intAdd(a, c);
            if (op=="-")  return intSub(a, c);
            if (op=="*")  return intMul(a, c);
            if (op=="/")  { if(c==0)   error("division by zero",b->range); return Value((double)a/(double)c); }
            if (op=="%")  { if(c==0)   error("modulo by zero",b->range);   return intMod(a, c); }
            if (op==">")  return Value(a>c);
            if (op=="<")  return Value(a<c);
            if (op==">=") return Value(a>=c);
            if (op=="<=") return Value(a<=c);
        } else if (l.isNumeric() && r.isNumeric()) {
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
        // dunder fallback: if neither built-in path matched and an operand is a typed pointer whose tag has a
        // __tag_<op> global (add/sub/mul/div/mod/gt/lt/gte/lte), call that instead of raising. left operand first,
        // then right. "==" and "!=" are excluded on purpose (see arithDunderOp in core/registry.h)
        if (const char* dop = arithDunderOp(op)) {
            Value dfn;
            if (resolveDunder(interp, l, dop, dfn) || resolveDunder(interp, r, dop, dfn))
                return doInvoke(dfn, {l, r}, b->range);
        }
        // == and != work on every value, callables included (identity; see Value::operator==)
        if (op=="==") return Value(l==r);
        if (op=="!=") return Value(l!=r);
        error("unsupported operator "+op+" between "+l.typeName()+" and "+r.typeName(), b->range);
    }

    // can the key of this index run script code? a key with no calls can't. neither can one that only calls data-only
    // builtins (str, len, ...), as long as each name still means a native function right now
    bool keyIsCallFree(const IndexExpr* idx) {
        if (idx->keyPurity < 0) idx->keyPurity = (int8_t)purityOf(idx->index.get(), &idx->keyCallees);
        if (idx->keyPurity == (int8_t)Purity::CallFree) return true;
        if (idx->keyPurity != (int8_t)Purity::PureCalls) return false;
        for (const auto& n : idx->keyCallees) {
            const Value* f = interp.find(n);
            if (!f || !f->isCallable() || !f->asCallable().isNative()) return false;
        }
        return true;
    }

    Value evalIndex(IndexExpr* idx) {
        // `name[key]` reads the variable where it lives instead of copying all of it first. the key goes first, which
        // only matters if it could change the variable, and a key with no calls can't
        if (idx->object->kind == Expr::Kind::Var && keyIsCallFree(idx)) {
            Value key = eval(idx->index.get());
            if (const Value* c = interp.find(static_cast<VarExpr*>(idx->object.get())->name))
                return indexValue(*c, key, idx);
        }
        Value cont = eval(idx->object.get()), key = eval(idx->index.get());
        return indexValue(cont, key, idx);
    }

    Value indexValue(const Value& cont, const Value& key, IndexExpr* idx) {
        if (cont.isString()) {
            int ii = (int)key.asNumber(); const auto& s = cont.asString();
            if (ii<0||ii>=(int)s.size()) error("string index "+std::to_string(ii)+" out of bounds. make sure the index is valid",idx->range);
            return Value(std::string(1,s[ii]));
        }
        if (cont.isMap()) {
            if (!key.isString()) error("map key must be string. use str() if the index is an expression",idx->range);
            // own slot, then (if key isn't one) the map's own "__proto__"
            // chain, see core/registry.h's lookupMapChain() doc comment.
            Value found;
            if (lookupMapChain(interp, cont, key.asString(), found)) return found;
            error("key not found: "+key.asString(),idx->range);
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

        bool hasRef = false;
        for (auto& a : call->args) {
            if (a->kind == Expr::Kind::Ref) { args.push_back(evalRefPath(static_cast<RefExpr*>(a.get()))); hasRef = true; }
            else args.push_back(eval(a.get()));
        }

        if (hasRef) return callWithRefs(callee, args, call->range);
        return doInvoke(callee, args, call->range);
    }

    // `&name[k1][k2]` as an argument: works out the keys now, finds the variable later (see RefPath in core/value.h)
    Value evalRefPath(RefExpr* r) {
        std::vector<IndexExpr*> chain;
        Expr* cur = r->target.get();
        while (cur->kind == Expr::Kind::Index) { chain.push_back(static_cast<IndexExpr*>(cur)); cur = chain.back()->object.get(); }
        RefPath p;
        p.name = static_cast<VarExpr*>(cur)->name;
        for (size_t i = chain.size(); i-- > 0; ) p.keys.push_back(eval(chain[i]->index.get()));
        return makeRefPath(std::move(p));
    }

    // a call with at least one `&` argument: every argument is in, so the paths can be turned into references
    Value callWithRefs(const Value& callee, std::vector<Value>& args, const SourceRange& site) {
        if (!callee.isCallable() || !callee.asCallable().isNative() || callee.asCallable().sig.empty())
            error("this call can't take '&': only natives with an in-out parameter do", site);
        const auto& c = callee.asCallable();
        for (auto& a : args) {
            if (!isRefPath(a)) continue;
            RefPath& p = refPathOf(a);
            Value* root = interp.findMut(p.name);
            if (!root) errorUndefined("variable", p.name, site);
            std::shared_ptr<RefChain> chain;
            std::string err;
            Value* target = resolveRefPath(root, p, chain, err);
            if (!target) error("&" + p.name + ": " + err, site);
            a = makeRef(target, std::move(chain));
        }
        Value result;
        try { result = doInvoke(callee, args, site); }
        catch (...) { settleRefs(args); throw; }
        settleRefs(args);
        if (isRef(result)) error("'" + c.name() + "' returned a reference. natives must not hand one back", site);
        return result;
    }

    // unpacks src into count Values for destructuring local/multi-assign
    std::vector<Value> unpackForDestructure(const Value& src, size_t count, const SourceRange& r) {
        if (src.isArray()) {
            const auto& arr = src.asArray();
            if (arr.size() < count)
                error("cannot unpack " + std::to_string(count) + " variable" + (count == 1 ? "" : "s") +
                      " from an array of " + std::to_string(arr.size()) + " element" +
                      (arr.size() == 1 ? "" : "s"), r);
            return std::vector<Value>(arr.begin(), arr.begin() + count);
        }
        if (src.isMap()) {
            if (count != 2)
                error("unpacking a map requires exactly 2 targets (keys, values), got " +
                      std::to_string(count), r);
            Value::array_type ks, vs;
            for (const auto& [k, v] : src.asMap()) { ks.push_back(Value(k)); vs.push_back(v); }
            return { Value(std::move(ks)), Value(std::move(vs)) };
        }
        error("cannot unpack " + src.typeName() + " into " + std::to_string(count) +
              " variable" + (count == 1 ? "" : "s"), r);
    }

    // defines one local destructuring target
    void defineTyped(const DestructureTarget& t, const Value& v, const SourceRange& r) {
        TypeSet ts = t.isAuto ? TypeSet(v.tag()) : t.type;
        if (!ts.isAny() && !ts.contains(v.tag()))
            error("cannot assign " + v.typeName() + " to '" + t.name + "' declared as " + ts.name(), r);
        interp.define(t.name, v, ts);
        if (interp.atModuleTopScope()) interp.markModuleLocal(t.name);
    }

    void exec(Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::Local: {
                auto* l = static_cast<LocalStmt*>(s);
                Value rhs = eval(l->value.get());

                if (l->targets.size() == 1 && !l->bracketed) {
                    defineTyped(l->targets[0], rhs, l->range);
                } else {
                    auto vals = unpackForDestructure(rhs, l->targets.size(), l->range);
                    for (size_t i = 0; i < l->targets.size(); ++i)
                        defineTyped(l->targets[i], vals[i], l->range);
                }
                break;
            }
            case Stmt::Kind::MultiAssign: {
                auto* m = static_cast<MultiAssignStmt*>(s);
                Value rhs = eval(m->value.get());
                auto vals = unpackForDestructure(rhs, m->names.size(), m->range);
                for (size_t i = 0; i < m->names.size(); ++i)
                    interp.set(m->names[i], std::move(vals[i]));
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
                    try { for (auto& st : w->body) exec(st.get()); }
                    catch (BreakSignal&)    { break; }
                    catch (ContinueSignal&) { continue; }
                }
                break;
            }
            case Stmt::Kind::For: execFor(static_cast<ForStmt*>(s)); break;
            case Stmt::Kind::Break:    throw BreakSignal{};
            case Stmt::Kind::Continue: throw ContinueSignal{};
            case Stmt::Kind::Try: {
                auto* t = static_cast<TryStmt*>(s);
                try {
                    Interpreter::ScopeGuard guard(interp);
                    for (auto& st : t->tryBlock) exec(st.get());
                } catch (const EmbrError& e) {
                    // BreakSignal/ContinueSignal/ReturnSignal aren't EmbrError
                    Interpreter::ScopeGuard guard(interp);
                    interp.define(t->catchVar, errorToMap(e));
                    for (auto& st : t->catchBlock) exec(st.get());
                }
                break;
            }
            case Stmt::Kind::Import: {
                auto* im = static_cast<ImportStmt*>(s);
                Value pathVal = eval(im->pathExpr.get());
                if (!pathVal.isString())
                    error("import path must evaluate to a string, got " + pathVal.typeName(), im->range);
                execImport(pathVal.asString(), im->range);
                break;
            }
        }
    }

    void execFor(ForStmt* f) {
        Value coll = eval(f->iterable.get());

        auto runBody = [&]() -> bool { // false = break
            try { for (auto& st : f->body) exec(st.get()); }
            catch (BreakSignal&)    { return false; }
            catch (ContinueSignal&) { /* nothing */ }
            return true;
        };

        if (coll.isArray()) {
            if (!f->keyName.empty())
                error("for-in over an array takes a single loop variable, not 'for k, v in ...'", f->range);
            for (const auto& el : coll.asArray()) {
                Interpreter::ScopeGuard guard(interp);
                interp.define(f->varName, el);
                if (!runBody()) break;
            }
        } else if (coll.isMap()) {
            for (const auto& [k, v] : coll.asMap()) {
                Interpreter::ScopeGuard guard(interp);
                if (!f->keyName.empty()) {
                    interp.define(f->keyName, Value(k));
                    interp.define(f->varName, v);
                } else {
                    interp.define(f->varName, Value(k));
                }
                if (!runBody()) break;
            }
        } else if (coll.isString()) {
            if (!f->keyName.empty())
                error("for-in over a string takes a single loop variable, not 'for k, v in ...'", f->range);
            for (char c : coll.asString()) {
                Interpreter::ScopeGuard guard(interp);
                interp.define(f->varName, Value(std::string(1, c)));
                if (!runBody()) break;
            }
        } else {
            error("cannot iterate over " + coll.typeName() + " with for-in", f->range);
        }
    }

    void execAssign(AssignStmt* a) {
        if (a->target->kind == Expr::Kind::Var) {
            auto* ve = static_cast<VarExpr*>(a->target.get());
            if (a->value->kind == Expr::Kind::Call && appendInPlace(ve->name, static_cast<CallExpr*>(a->value.get()))) return;
            interp.set(ve->name, eval(a->value.get()));
            return;
        }
        if (a->target->kind == Expr::Kind::Index) {
            Value v = eval(a->value.get());
            if (assignInPlace(static_cast<IndexExpr*>(a->target.get()), v)) return;
            writeBack(a->target.get(), std::move(v), a->range);
            return;
        }
        error("invalid assignment target", a->range);
    }

    // `x = push(x, v)` when push is embrlib's and x is an array: appends to x where it lives instead of building a
    // whole new array. false means it didn't apply and nothing was evaluated that matters (v has no calls), so the
    // ordinary assignment runs
    bool appendInPlace(const std::string& name, CallExpr* c) {
        if (c->callee->kind != Expr::Kind::Var || static_cast<VarExpr*>(c->callee.get())->name != "push" ||
            c->args.size() != 2 || c->args[0]->kind != Expr::Kind::Var ||
            static_cast<VarExpr*>(c->args[0].get())->name != name ||
            purityOf(c->args[1].get()) != Purity::CallFree)
            return false;
        const Value* fn = interp.find("push");
        Value* target = interp.findMut(name);
        if (!fn || !fn->isCallable() || !fn->asCallable().isNative() || fn->asCallable().name() != "push" ||
            !target || !target->isArray())
            return false;
        target->arrayAppend(eval(c->args[1].get()));
        return true;
    }

    // `name[k1]..[kn] = v` changed where the variable lives, when no key can run script code. false means it didn't apply
    // (a key with a call, an undefined name, an index that isn't there, a map key found only through __proto__), and
    // nothing has changed, so writeBack() runs and reports whatever is wrong
    bool assignInPlace(IndexExpr* idx, const Value& v) {
        std::vector<IndexExpr*> chain;
        Expr* cur = idx;
        while (cur->kind == Expr::Kind::Index) {
            auto* ix = static_cast<IndexExpr*>(cur);
            if (!keyIsCallFree(ix)) return false;
            chain.push_back(ix);
            cur = ix->object.get();
        }
        if (cur->kind != Expr::Kind::Var) return false;
        RefPath path;
        path.name = static_cast<VarExpr*>(cur)->name;
        for (size_t i = chain.size(); i-- > 1; ) path.keys.push_back(eval(chain[i]->index.get()));   // all but the last
        Value last = eval(chain[0]->index.get());
        Value* root = interp.findMut(path.name);
        if (!root) return false;
        std::shared_ptr<RefChain> rc;
        std::string err;
        Value* cont = resolveRefPath(root, path, rc, err);
        if (!cont) return false;
        size_t above = rc ? rc->above.size() : 0;
        if (cont->isArray()) {
            int ii = (int)last.asNumber();
            if (ii < 0 || ii >= (int)cont->asArray().size()) return false;
            Value::checkElementDepth(v, above);
            cont->arraySet((size_t)ii, v);
        } else if (cont->isMap()) {
            if (!last.isString()) return false;
            Value::checkElementDepth(v, above);
            cont->mapSet(last.asString(), v);
        } else if (cont->isString()) {
            int ii = (int)last.asNumber();
            if (!v.isString() || ii < 0 || ii >= (int)cont->asString().size()) return false;
            cont->stringSetAt((size_t)ii, v.asString());
            return true;
        } else {
            return false;
        }
        if (rc) settleChain(*rc, cont->nestDepth);
        return true;
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

        auto cands = fileCandidates(rawPath, interp.scriptDir, ".embr", "modules");
        fs::path resolved = findExistingCandidate(cands);
        if (resolved.empty())
            error("cannot find module \"" + rawPath + "\"\n  tried:" + describeCandidates(cands), r);

        const std::string canonStr = canonicalOrSelf(resolved);

        // deduplication
        if (interp.loadedModules_.count(canonStr)) return;
        interp.loadedModules_.insert(canonStr);

        auto src = tryReadFile(resolved);
        if (!src) error("cannot open module file: " + resolved.string(), r);

        // save interpreter context fields that runSource overwrites
        std::string savedDir  = interp.scriptDir;
        SourceMap   savedMap  = interp.sourceMap();
        std::string savedFile = interp.currentFile();

        interp.scriptDir = resolved.parent_path().string();

        // push a dedicated module scope
        interp.pushModuleScope();

        // parse & run inside the module scope
        try {
            Lexer     lex(*src);
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

        // pop module scope and export its non-local bindings into the
        // importer's current scope (import statement semantics, see
        // exportModuleScope's own comment for why this differs from load_module())
        auto res = interp.popModuleScope();

        // restore interpreter context
        interp.scriptDir = savedDir;
        interp.setSource(savedMap, savedFile);

        exportModuleScope(interp, std::move(res), /*mutateCallerScope=*/true);
    }

    // import a native plugin (.so / .dll / .dylib)
    void execImportPlugin(const std::string& rawPath, const SourceRange& r) {
        // locating, policy-checking (sandbox allow-list) and registering a plugin all live in
        // importNativePlugin() (module.h), shared with the VM so neither backend can bypass the policy
        std::string err = importNativePlugin(interp, rawPath);
        if (!err.empty()) error(err, r);
    }
};

// compile and run in one call. an uncaught EmbrError stops the rest of src and is printed once here, same as
// vm::runSource, so a caller doesn't need to know which backend it got
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
    try {
        runner.run(*stable, sm, filename);
    } catch (const EmbrError& e) {
        std::cerr << e.what() << formatTrace(e);
    }
}

} // namespace embr

#endif // EMBR_WITH_TREE_WALKER
#endif // EMBR_BACKENDS_TREE_WALKER_H
