#ifndef EMBR_BACKENDS_VM_H
#define EMBR_BACKENDS_VM_H
// vm backend for embr
// compiles AST to a flat Instruction stream and executes it
//
// usage:
//   embr::Interpreter interp;
//   embr::vm::runSource(src, interp, "<input>");
//
// Compiler is a one-pass AST walker that emits a flat Instruction stream
// each compiled function lives in its own CompiledChunk
// VmRunner owns its own call-frame stack
// variables declared or assigned at true top level are routed through DEFINE_GLOBAL*/LOAD_GLOBAL/STORE_GLOBAL opcodes into the shared Interpreter global
// ScriptFn::compiledChunk (core/value.h) is a type-erased std::shared_ptr<void> holding a shared_ptr<CompiledChunk> when a function was compiled here
// 
// YIELD suspends the runner and the caller may resume it by calling step() repeatedly or run() again after injecting instructions
// inject() prepends raw instructions into a mutable override buffer so plugins can splice in loops, async continuations, etc

#include "../core/diagnostics.h"
#include "../core/types.h"
#include "../core/value.h"
#include "../core/ast.h"
#include "../core/registry.h"
#include "../core/lexer.h"
#include "../core/parser.h"
#include "../core/platform.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <filesystem>

#ifdef EMBR_WITH_VM

namespace embr {
namespace vm {

// instruction set

enum class Op : uint8_t {
    // literals
    PUSH_NUM,       // numVal  = the double
    PUSH_INT,       // intVal  = the int64
    PUSH_STR,       // operand = string-table index
    PUSH_NIL,       // push Value(0.0)

    // stack
    POP,            // discard top

    // variables
    LOAD,           // operand = string-table index  ->  push value
    STORE,          // operand = string-table index  <-  pop value, assign-or-create in nearest scope, type-checked if declared
    DEFINE,         // operand = string-table index  <-  pop value, define untyped in current (innermost) scope
    DEFINE_TYPED,   // operand = string-table index, typeHint = declared type  <-  pop value, type-checked
    DEFINE_AUTO,    // operand = string-table index  <-  pop value, type inferred from the value itself and enforced from then on
    LOAD_GLOBAL,    // operand = string-table index  ->  read from Interpreter global scope
    STORE_GLOBAL,   // operand = string-table index  <-  write to Interpreter global scope
    DEFINE_GLOBAL,       // operand = string-table index  <-  pop value, define untyped in the Interpreter global scope
    DEFINE_GLOBAL_TYPED, // operand = string-table index, typeHint = declared type  <-  pop value, type-checked, defined globally
    DEFINE_GLOBAL_AUTO,  // operand = string-table index  <-  pop value, type inferred and enforced, defined globally

    // arithmetic / string
    ADD, SUB, MUL, DIV, MOD,
    NEG,            // unary minus

    // comparison  (all pop two, push number 0/1)
    EQ, NEQ, LT, GT, LTE, GTE,

    // logic
    NOT,            // unary !

    // control flow
    JUMP,           // operand = absolute instruction index
    JUMP_IF_FALSE,  // operand = absolute instruction index; pops condition

    // jump but leave value on stack
    JUMP_IF_FALSE_PEEK,
    JUMP_IF_TRUE_PEEK,

    // containers
    BUILD_ARRAY,    // operand = element count; pops N, pushes array
    BUILD_MAP,      // operand = entry count;   pops 2N (key,val pairs), pushes map
    INDEX_GET,      // pops container + key, pushes element
    INDEX_SET,      // pops container + key + value, pushes updated container, then STORE follows

    // destructuring
    // pops one array/map, pushes operand values 
    // array: first N elements, error if too few
    // map: operand must be 2, pushes (keys array, values array)
    // pushed in reverse so the first target ends up on TOS, ready for a following DEFINE/DEFINE_TYPED/DEFINE_AUTO/STORE
    UNPACK,

    // for loops
    // ITER_INIT pops the iterable and pushes iterator state onto the current frame's private iterator stack
    // operand = 1 or 2, the number of loop variables
    // ITER_NEXT pushes that many values (2 = key then value, value on TOS) and advances
    // once exhausted jumps to operand without pushing anything
    // ITER_POP discards the iterator state. emitted once at the loop's single exit point, shared by both natural exhaustion and break
    ITER_INIT,
    ITER_NEXT,
    ITER_POP,

    // callables
    MAKE_CLOSURE,   // operand = function-table index; snapshots current locals -> pushes callable
    CALL,           // operand = arg count (or -1 = import sentinel); stack: [callee, arg0..argN]
    RETURN,         // pops return value, unwinds frame

    // async / plugin injection
    YIELD,          // suspend this runner; return control to caller of step()
    NOP,            // no-op; useful as a patch target

    // scope management (name erased only)
    SCOPE_PUSH,
    SCOPE_POP,

    // try/catch
    // operand on TRY_PUSH = ip of the catch block's own SCOPE_PUSH patched like a jump target
    // see VmRunner::handleTry()
    TRY_PUSH,
    TRY_POP,
};

struct Instruction {
    Op      op      = Op::NOP;
    int     operand = 0;               // string-table index, jump target, count, fn index
    double  numVal  = 0.0;             // payload for PUSH_NUM
    int64_t intVal  = 0;               // payload for PUSH_INT
    int     line    = 0;               // for error reporting
    TypeSet typeHint = TypeSet::Any(); // payload for DEFINE_TYPED
};


// compiled representation

// one compiled function body owned by CompiledProgram
// pointed to by closures via shared_ptr so the program can be discarded while closures are still live
struct CompiledChunk {
    std::string              name;
    std::vector<Param>       params;
    TypeSet                  returnType = TypeSet::Any();
    std::vector<Instruction> code;
    std::vector<std::string> strings;   // string table local to this chunk
    SourceRange              definedAt;
    std::string              sourceFile;

    // intern a string, return its index
    int intern(const std::string& s) {
        for (int i = 0; i < (int)strings.size(); ++i)
            if (strings[i] == s) return i;
        strings.push_back(s);
        return (int)strings.size() - 1;
    }

    const std::string& str(int idx) const {
        static const std::string empty;
        if (idx < 0 || idx >= (int)strings.size()) return empty;
        return strings[idx];
    }
};

// full output of a compilation run
struct CompiledProgram {
    // index 0 = top-level chunk
    std::vector<std::shared_ptr<CompiledChunk>> chunks;
    const CompiledChunk& top() const { return *chunks[0]; }
};

class Compiler {
public:
    // the Interpreter is reserved for a LOAD vs LOAD_GLOBAL optimisation
    explicit Compiler(const Interpreter* interp = nullptr) : interp_(interp) {}

    CompiledProgram compile(const std::vector<StmtPtr>& ast,
                            const std::string& filename = "<input>") {
        prog_ = CompiledProgram{};
        // top-level chunk
        auto top = std::make_shared<CompiledChunk>();
        top->name       = "<top>";
        top->sourceFile = filename;
        prog_.chunks.push_back(top);
        cur_ = top.get();
        for (auto& s : ast) compileStmt(s.get());
        cur_ = nullptr;
        return std::move(prog_);
    }

private:
    const Interpreter*    interp_;
    CompiledProgram       prog_;
    CompiledChunk*        cur_  = nullptr;  // current chunk being written

    // compile-time mirror of the SCOPE_PUSH/SCOPE_POP nesting depth emitted so far in the current chunk 
    // so break/continue know how many SCOPE_POPs to backfill before jumping out of nested if/while/for scopes sit between them and the loop they belong to
    int scopeDepth_ = 0;

    // nesting depth of try statements so break/continue know how many TRY_POPs to backfill when jumping out of a try block that sits inside the loop
    int tryDepth_ = 0;

    struct LoopCtx {
        std::vector<int> breakJumps;   // JUMP instrs to patch once the loop's single exit point is known
        int continueTarget;            // instruction index 'continue' jumps to (the loop's re-check point)
        int scopeDepthAtEntry;         // scopeDepth_ when this loop's body started
        int tryDepthAtEntry;           // tryDepth_ when this loop's body started
    };
    std::vector<LoopCtx> loopStack_;

    // emit helpers
    void emit(Op op, int operand = 0, double num = 0.0, int line = 0, TypeSet th = TypeSet::Any()) {
        Instruction i; i.op = op; i.operand = operand; i.numVal = num; i.line = line; i.typeHint = th;
        cur_->code.push_back(i);
    }
    void emitPushInt(int64_t v, int line) {
        Instruction i; i.op = Op::PUSH_INT; i.intVal = v; i.line = line;
        cur_->code.push_back(i);
    }
    void emitScopePush() { emit(Op::SCOPE_PUSH); ++scopeDepth_; }
    void emitScopePop()  { emit(Op::SCOPE_POP);  --scopeDepth_; }

    // returns the index of the emitted instruction for later back-patching
    int emitJump(Op op, int line = 0) {
        cur_->code.push_back({op, -1, 0.0, 0, line});
        return (int)cur_->code.size() - 1;
    }

    void patchJump(int idx) {
        assert(idx >= 0 && idx < (int)cur_->code.size());
        cur_->code[idx].operand = (int)cur_->code.size();
    }

    int here() const { return (int)cur_->code.size(); }

    // true only for code at the true top level
    bool atTopLevel() const {
        return cur_ == prog_.chunks[0].get() && scopeDepth_ == 0;
    }

    // expression compilation
    void compileExpr(Expr* e) {
        switch (e->kind) {

        case Expr::Kind::Number:
            emit(Op::PUSH_NUM, 0, static_cast<NumberExpr*>(e)->v, e->range.startLine);
            break;

        case Expr::Kind::Int:
            emitPushInt(static_cast<IntExpr*>(e)->v, e->range.startLine);
            break;

        case Expr::Kind::String: {
            auto* se = static_cast<StringExpr*>(e);
            emit(Op::PUSH_STR, cur_->intern(se->v), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Var: {
            auto* ve = static_cast<VarExpr*>(e);
            emitLoad(ve->name, e->range.startLine);
            break;
        }

        case Expr::Kind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            compileExpr(u->right.get());
            if (u->op == "-") emit(Op::NEG, 0, 0.0, e->range.startLine);
            else if (u->op == "!") emit(Op::NOT, 0, 0.0, e->range.startLine);
            else raiseError("compiler", "unknown unary op: " + u->op, e->range);
            break;
        }

        case Expr::Kind::Binary:
            compileBinary(static_cast<BinaryExpr*>(e));
            break;

        // if left is falsy push left and skip right
        case Expr::Kind::And: {
            auto* log = static_cast<LogicExpr*>(e);
            compileExpr(log->left.get());
            int skipRight = emitJump(Op::JUMP_IF_FALSE_PEEK, e->range.startLine);
            emit(Op::POP);                       // discard left; we want right's value
            compileExpr(log->right.get());
            patchJump(skipRight);
            break;
        }

        // if left is truthy push left and skip right
        case Expr::Kind::Or: {
            auto* log = static_cast<LogicExpr*>(e);
            compileExpr(log->left.get());
            int skipRight = emitJump(Op::JUMP_IF_TRUE_PEEK, e->range.startLine);
            emit(Op::POP);
            compileExpr(log->right.get());
            patchJump(skipRight);
            break;
        }

        case Expr::Kind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            compileExpr(c->callee.get());
            for (auto& arg : c->args) compileExpr(arg.get());
            emit(Op::CALL, (int)c->args.size(), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Array: {
            auto* a = static_cast<ArrayExpr*>(e);
            for (auto& el : a->elements) compileExpr(el.get());
            emit(Op::BUILD_ARRAY, (int)a->elements.size(), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Map: {
            auto* m = static_cast<MapExpr*>(e);
            for (auto& [k, v] : m->entries) {
                compileExpr(k.get());
                compileExpr(v.get());
            }
            emit(Op::BUILD_MAP, (int)m->entries.size(), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Index: {
            auto* idx = static_cast<IndexExpr*>(e);
            compileExpr(idx->object.get());
            compileExpr(idx->index.get());
            emit(Op::INDEX_GET, 0, 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Lambda: {
            auto* lam = static_cast<LambdaExpr*>(e);
            int fnIdx = compileFunction(
                "<lambda>", lam->params, lam->returnType,
                lam->ownedBody, e->range, cur_->sourceFile);
            emit(Op::MAKE_CLOSURE, fnIdx, 0.0, e->range.startLine);
            break;
        }

        default:
            raiseError("compiler", "unhandled expression kind", e->range);
        }
    }

    void compileBinary(BinaryExpr* b) {
        compileExpr(b->left.get());
        compileExpr(b->right.get());
        int ln = b->range.startLine;
        const auto& op = b->op;
        if      (op == "+")  emit(Op::ADD,  0, 0.0, ln);
        else if (op == "-")  emit(Op::SUB,  0, 0.0, ln);
        else if (op == "*")  emit(Op::MUL,  0, 0.0, ln);
        else if (op == "/")  emit(Op::DIV,  0, 0.0, ln);
        else if (op == "%")  emit(Op::MOD,  0, 0.0, ln);
        else if (op == "==") emit(Op::EQ,   0, 0.0, ln);
        else if (op == "!=") emit(Op::NEQ,  0, 0.0, ln);
        else if (op == "<")  emit(Op::LT,   0, 0.0, ln);
        else if (op == ">")  emit(Op::GT,   0, 0.0, ln);
        else if (op == "<=") emit(Op::LTE,  0, 0.0, ln);
        else if (op == ">=") emit(Op::GTE,  0, 0.0, ln);
        else raiseError("compiler", "unknown binary op: " + op, b->range);
    }

    // statement compilation
    void compileStmt(Stmt* s) {
        switch (s->kind) {

        case Stmt::Kind::Expr: {
            compileExpr(static_cast<ExprStmt*>(s)->expr.get());
            emit(Op::POP, 0, 0.0, s->range.startLine); // discard result
            break;
        }

        case Stmt::Kind::Local: {
            auto* l = static_cast<LocalStmt*>(s);
            int line = s->range.startLine;
            compileExpr(l->value.get());
            bool destructure = l->targets.size() > 1 || l->bracketed;
            if (!destructure) {
                emitDefineTarget(l->targets[0], line);
            } else {
                emit(Op::UNPACK, (int)l->targets.size(), 0.0, line);
                for (auto& t : l->targets) emitDefineTarget(t, line);
            }
            break;
        }

        case Stmt::Kind::MultiAssign: {
            auto* m = static_cast<MultiAssignStmt*>(s);
            int line = s->range.startLine;
            compileExpr(m->value.get());
            emit(Op::UNPACK, (int)m->names.size(), 0.0, line);
            for (auto& nm : m->names) emitStore(nm, line);
            break;
        }

        case Stmt::Kind::Assign:
            compileAssign(static_cast<AssignStmt*>(s));
            break;

        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            compileExpr(i->cond.get());
            int toElse = emitJump(Op::JUMP_IF_FALSE, s->range.startLine);

            emitScopePush();
            for (auto& st : i->thenBlock) compileStmt(st.get());
            emitScopePop();

            if (!i->elseBlock.empty()) {
                int toEnd = emitJump(Op::JUMP, s->range.startLine);
                patchJump(toElse);
                emitScopePush();
                for (auto& st : i->elseBlock) compileStmt(st.get());
                emitScopePop();
                patchJump(toEnd);
            } else {
                patchJump(toElse);
            }
            break;
        }

        case Stmt::Kind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            int line = s->range.startLine;
            int loopTop = here();
            compileExpr(w->cond.get());
            int toEnd = emitJump(Op::JUMP_IF_FALSE, line);

            loopStack_.push_back({{}, loopTop, scopeDepth_, tryDepth_});
            emitScopePush();
            for (auto& st : w->body) compileStmt(st.get());
            emitScopePop();
            emit(Op::JUMP, loopTop, 0.0, line);

            LoopCtx ctx = std::move(loopStack_.back());
            loopStack_.pop_back();
            patchJump(toEnd);
            for (int idx : ctx.breakJumps) patchJump(idx);
            break;
        }

        case Stmt::Kind::For: {
            auto* f = static_cast<ForStmt*>(s);
            int line = s->range.startLine;
            compileExpr(f->iterable.get());
            int varCount = f->keyName.empty() ? 1 : 2;
            emit(Op::ITER_INIT, varCount, 0.0, line);

            int loopTop = here();
            int toEnd = emitJump(Op::ITER_NEXT, line);

            loopStack_.push_back({{}, loopTop, scopeDepth_, tryDepth_});
            emitScopePush();
            if (varCount == 2) {
                emit(Op::DEFINE, cur_->intern(f->varName), 0.0, line); // pops value (TOS)
                emit(Op::DEFINE, cur_->intern(f->keyName),  0.0, line); // pops key
            } else {
                emit(Op::DEFINE, cur_->intern(f->varName), 0.0, line);
            }
            for (auto& st : f->body) compileStmt(st.get());
            emitScopePop();
            emit(Op::JUMP, loopTop, 0.0, line);

            LoopCtx ctx = std::move(loopStack_.back());
            loopStack_.pop_back();
            patchJump(toEnd);
            for (int idx : ctx.breakJumps) patchJump(idx);
            emit(Op::ITER_POP, 0, 0.0, line);
            break;
        }

        case Stmt::Kind::Break: {
            // the parser rejects break/continue outside a loop, so loopStack_ is guaranteed non-empty
            auto& ctx = loopStack_.back();
            for (int i = 0; i < scopeDepth_ - ctx.scopeDepthAtEntry; ++i) emit(Op::SCOPE_POP);
            for (int i = 0; i < tryDepth_ - ctx.tryDepthAtEntry; ++i) emit(Op::TRY_POP);
            ctx.breakJumps.push_back(emitJump(Op::JUMP, s->range.startLine));
            break;
        }

        case Stmt::Kind::Continue: {
            auto& ctx = loopStack_.back();
            for (int i = 0; i < scopeDepth_ - ctx.scopeDepthAtEntry; ++i) emit(Op::SCOPE_POP);
            for (int i = 0; i < tryDepth_ - ctx.tryDepthAtEntry; ++i) emit(Op::TRY_POP);
            emit(Op::JUMP, ctx.continueTarget, 0.0, s->range.startLine);
            break;
        }

        case Stmt::Kind::Try: {
            auto* t = static_cast<TryStmt*>(s);
            int line = s->range.startLine;
            int tryPush = emitJump(Op::TRY_PUSH, line);
            ++tryDepth_;
            emitScopePush();
            for (auto& st : t->tryBlock) compileStmt(st.get());
            emitScopePop();
            --tryDepth_;
            emit(Op::TRY_POP, 0, 0.0, line);
            int toEnd = emitJump(Op::JUMP, line);

            patchJump(tryPush); // catch block starts here
            emitScopePush();
            emit(Op::DEFINE, cur_->intern(t->catchVar), 0.0, line); // pops the error value handleTry() pushed
            for (auto& st : t->catchBlock) compileStmt(st.get());
            emitScopePop();
            patchJump(toEnd);
            break;
        }

        case Stmt::Kind::Fn: {
            auto* f = static_cast<FnStmt*>(s);
            int fnIdx = compileFunction(
                f->name, f->params, f->returnType,
                f->body, f->range, cur_->sourceFile);
            emit(Op::MAKE_CLOSURE, fnIdx, 0.0, s->range.startLine);
            // functions defined at statement level go into the current scope so later code in the same chunk can call them
            emit(atTopLevel() ? Op::DEFINE_GLOBAL : Op::DEFINE, cur_->intern(f->name), 0.0, s->range.startLine);
            break;
        }

        case Stmt::Kind::Return: {
            auto* r = static_cast<ReturnStmt*>(s);
            compileExpr(r->value.get());
            emit(Op::RETURN, 0, 0.0, s->range.startLine);
            break;
        }

        case Stmt::Kind::Import: {
            // import is a side-effect that mutates the Interpreter (loads a plugin)
            // emit a special call to the built-in __import__ helper which the VM dispatches to execImport
            // the string operand is the path
            auto* im = static_cast<ImportStmt*>(s);
            emit(Op::PUSH_STR, cur_->intern(im->path), 0.0, s->range.startLine);
            // operand -1 = sentinel so the VM knows this CALL is an import
            emit(Op::CALL, -1, 0.0, s->range.startLine);
            break;
        }

        }
    }

    // emits whichever DEFINE variant a destructuring/local target calls for, routing to the *_GLOBAL variant at true top level
    void emitDefineTarget(const DestructureTarget& t, int line) {
        bool g = atTopLevel();
        if (t.isAuto)              emit(g ? Op::DEFINE_GLOBAL_AUTO  : Op::DEFINE_AUTO,  cur_->intern(t.name), 0.0, line);
        else if (!t.type.isAny())  emit(g ? Op::DEFINE_GLOBAL_TYPED : Op::DEFINE_TYPED, cur_->intern(t.name), 0.0, line, t.type);
        else                       emit(g ? Op::DEFINE_GLOBAL       : Op::DEFINE,       cur_->intern(t.name), 0.0, line);
    }

    // compile an assignment, including chained index write-backs
    void compileAssign(AssignStmt* a) {
        if (a->target->kind == Expr::Kind::Var) {
            compileExpr(a->value.get());
            auto* ve = static_cast<VarExpr*>(a->target.get());
            emitStore(ve->name, a->range.startLine);
            return;
        }
        if (a->target->kind == Expr::Kind::Index) {
            compileIndexAssign(static_cast<IndexExpr*>(a->target.get()),
                               a->value.get(), a->range.startLine);
            return;
        }
        raiseError("compiler", "invalid assignment target", a->target->range);
    }

    // eval container, eval key, eval value, INDEX_SET, then write the updated container back to wherever a lives
    // recurse nested
    void compileIndexAssign(IndexExpr* idx, Expr* rhs, int line) {
        compileExpr(idx->object.get());  // container
        compileExpr(idx->index.get());   // key
        compileExpr(rhs);                // new value
        emit(Op::INDEX_SET, 0, 0.0, line);
        // stack now has the updated container; write it back
        writeBackTarget(idx->object.get(), line);
    }

    // after INDEX_SET leaves the updated container on the stack store it back into the appropriate target
    void writeBackTarget(Expr* target, int line) {
        if (target->kind == Expr::Kind::Var) {
            auto* ve = static_cast<VarExpr*>(target);
            emitStore(ve->name, line);
        } else if (target->kind == Expr::Kind::Index) {
            auto* idx = static_cast<IndexExpr*>(target);
            std::string tmp = "__wb_tmp_" + std::to_string(here());
            emit(Op::DEFINE, cur_->intern(tmp), 0.0, line);  // pop+store the updated sub-container
            compileExpr(idx->object.get());                  // outer container
            compileExpr(idx->index.get());                   // outer key
            emit(Op::LOAD, cur_->intern(tmp), 0.0, line);    // reload updated sub-container as the value
            emit(Op::INDEX_SET, 0, 0.0, line);
            writeBackTarget(idx->object.get(), line);
        } else {
            raiseError("compiler", "cannot assign to temporary", {});
        }
    }

    // function/lambda helper

    // compile a function body into a new chunk, return its index in prog_.chunks
    int compileFunction(
        const std::string&        name,
        const std::vector<Param>& params,
        TypeSet                   returnType,
        const std::vector<StmtPtr>& body,
        const SourceRange&        range,
        const std::string&        sourceFile)
    {
        auto chunk = std::make_shared<CompiledChunk>();
        chunk->name       = name;
        chunk->params     = params;
        chunk->returnType = returnType;
        chunk->definedAt  = range;
        chunk->sourceFile = sourceFile;

        int idx = (int)prog_.chunks.size();
        prog_.chunks.push_back(chunk);

        // switch compilation target to the new chunk
        // break/continue can't cross a fn boundary, so start this chunk with an empty loop context too
        CompiledChunk* savedChunk      = cur_;
        int            savedScopeDepth = scopeDepth_;
        int            savedTryDepth   = tryDepth_;
        auto           savedLoopStack  = std::move(loopStack_);
        cur_        = chunk.get();
        scopeDepth_ = 0;
        tryDepth_   = 0;
        loopStack_.clear();

        for (auto& s : body) compileStmt(s.get());

        // implicit return 0 at end of function if no explicit return reached
        emit(Op::PUSH_NIL);
        emit(Op::RETURN);

        cur_        = savedChunk;
        scopeDepth_ = savedScopeDepth;
        tryDepth_   = savedTryDepth;
        loopStack_  = std::move(savedLoopStack);
        return idx;
    }

    // variable load/store helpers

    // LOAD/STORE for everything except true top level
    void emitLoad(const std::string& name, int line) {
        emit(atTopLevel() ? Op::LOAD_GLOBAL : Op::LOAD, cur_->intern(name), 0.0, line);
    }

    void emitStore(const std::string& name, int line) {
        emit(atTopLevel() ? Op::STORE_GLOBAL : Op::STORE, cur_->intern(name), 0.0, line);
    }
};


// one iteration in progress for a for loop
// lives on the owning CallFrame's iterStack (not the value stack) so a native callback re-entering the VM mid-loop can't corrupt it
struct IterState {
    int               varCount = 1;   // 1 or 2 loop variables per step
    Value::array_type keys;           // populated only for varCount == 2 (map iteration)
    Value::array_type items;          // values to iterate: array elements, 1-char strings, or map keys
    size_t            idx = 0;
};

// one activation record on the call stack
struct CallFrame {
    std::shared_ptr<CompiledChunk>           chunk;       // the function being executed
    int                                      ip = 0;      // instruction pointer into chunk->code
    int                                      stackBase;   // index of this frame's first local on the value stack
    std::unordered_map<std::string, Value>   locals;      // this frame's scope layer
    std::unordered_map<std::string, TypeSet> localTypes;  // set by DEFINE_TYPED/DEFINE_AUTO; consulted by STORE
    std::shared_ptr<CaptureFrame>            captured;    // closure env may be null
    TypeSet                                  returnType = TypeSet::Any();
    std::string                              fnName;

    // per-frame so a nested call mid-loop-body/mid-iteration can't corrupt the caller's own bookkeeping
    int                                   scopeDepth = 0;
    std::vector<std::vector<std::string>> scopeNames;  // names defined at each SCOPE_PUSH depth
    std::vector<IterState>                iterStack;

    // one entry per currently-open try block in this frame. LIFO
    struct TryHandler {
        int    catchIp;             // ip of the catch block's own SCOPE_PUSH
        int    scopeDepthAtEntry;   // this frame's scopeDepth when TRY_PUSH ran
        size_t stackBase;           // VmRunner::stack_ size when TRY_PUSH ran
    };
    std::vector<TryHandler> tryHandlers;

    // set only on the persistent frame VmRunner::run() creates for its very first call
    // doReturn() special-cases it to survive running off the end of its chunk so locals defined by one run() call are still there for the next
    bool isPersistentTop = false;
};

class VmRunner {
public:
    enum class State { Ready, Running, Suspended, Done, Error };

    explicit VmRunner(Interpreter& interp) : interp_(interp) {}

    // run a compiled program to completion or until YIELD
    // calling this again on the same VmRunner after a prior call completed reuses the persistent top-level frame so its locals survive
    Value run(const CompiledProgram& prog) {
        prog_ = &prog;
        if (frames_.empty()) {
            CallFrame frame{};
            frame.chunk          = prog.chunks[0];
            frame.stackBase      = 0;
            frame.fnName         = "<top>";
            frame.isPersistentTop = true;
            frames_.push_back(std::move(frame));
        } else {
            while (frames_.size() > 1) { frames_.pop_back(); interp_.exitCall(); }
            stack_.clear();
            frames_[0].chunk = prog.chunks[0];
            frames_[0].ip    = 0;
            frames_[0].scopeDepth = 0;
            frames_[0].scopeNames.clear();
            frames_[0].iterStack.clear();
        }
        state_ = State::Running;
        return dispatch();
    }

    // execute a single instruction returns false when Done/Error/Suspended
    bool step() {
        if (state_ != State::Running) return false;
        if (frames_.empty()) { state_ = State::Done; return false; }
        execOne();
        return state_ == State::Running;
    }

    // resume after a YIELD
    Value resume() {
        if (state_ == State::Suspended) state_ = State::Running;
        return dispatch();
    }

    // invoke a callable from C++. used by embr::invoke() and by native functions that wanna operate on runner's frame stack directly
    Value invoke(const Value& callee, const std::vector<Value>& args,
                 const SourceRange& site = {}) {
        if (!callee.isCallable())
            vmError("value is not callable (got " + callee.typeName() + ")", site.startLine);

        const auto& c = callee.asCallable();
        if (c.isNative()) {
            checkNativeSig(c.name(), c.sig, args, site);
            try {
                return c.native(args);
            } catch (EmbrError& e) {
                if (!e.hasLocation && site.valid())
                    raiseError(c.name(), e.message, site, interp_.sourceMap());
                throw;
            }
        }

        // script call via compiled chunk
        const ScriptFn& sf = c.script;
        if (sf.compiledChunk) {
            auto chunk = std::static_pointer_cast<CompiledChunk>(sf.compiledChunk);
            return invokeChunk(chunk, sf.captured, args, site, sf.returnType, sf.name);
        }

#ifdef EMBR_WITH_TREE_WALKER
        // the callee was created by the tree-walking backend (no compiled chunk); fall back to it
        Runner treeRunner(interp_);
        return treeRunner.invoke(callee, args, site);
#else
        vmError("cannot invoke '" + sf.name + "': it was not compiled by the VM, and the "
                "tree-walker backend is not compiled into this build", site.startLine);
#endif
    }

    // inject instructions to be executed before the current instruction pointer resumes
    // useful for plugins that want to prepend a loop or async continuation into the running stream
    void inject(std::vector<Instruction> code) {
        if (!injBuf_) {
            // copy remaining instructions from the current chunk into a mutable buffer
            auto& cur = frames_.back();
            auto remaining = std::vector<Instruction>(
                cur.chunk->code.begin() + cur.ip,
                cur.chunk->code.end());
            injBuf_ = std::make_unique<std::vector<Instruction>>(std::move(remaining));
            cur.ip = 0;
        }
        // prepend new code before the buffered remainder
        injBuf_->insert(injBuf_->begin(), code.begin(), code.end());
        frames_.back().ip = 0;
    }

    State           state()   const { return state_; }
    Interpreter&    interp()        { return interp_; }

private:
    Interpreter&                          interp_;
    const CompiledProgram*                prog_   = nullptr;
    std::vector<CallFrame>                frames_;
    std::vector<Value>                    stack_;
    State                                 state_  = State::Ready;
    // optional mutable override buffer (used by inject())
    std::unique_ptr<std::vector<Instruction>> injBuf_;

    // stack helpers
    void   push(Value v)  { stack_.push_back(std::move(v)); }
    Value  pop()          { assert(!stack_.empty()); Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
    Value& peek()         { assert(!stack_.empty()); return stack_.back(); }

    // frame/scope helpers

    // look up a name
    // innermost frame locals > captured frame > outer call frames > Interpreter global scope
    Value resolveLoad(const std::string& name, int line) {
        // walk frames from innermost outward
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            auto f = it->locals.find(name);
            if (f != it->locals.end()) return f->second;
            if (it->captured) {
                auto cf = it->captured->find(name);
                if (cf != it->captured->end()) return cf->second;
            }
        }
        // fall through to Interpreter global (read-only)
        if (interp_.has(name)) return interp_.get(name);
        // not found suggest closest match
        std::string hint = suggest(name);
        std::string msg  = "undefined variable: " + name;
        if (!hint.empty()) msg += "\n  did you mean '" + hint + "'?";
        vmError(msg, line);
    }

    // store to nearest scope that already contains this name, else create in the innermost frame's locals (untyped)
    void resolveStore(const std::string& name, Value v, int line) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if (it->locals.count(name)) {
                auto tyIt = it->localTypes.find(name);
                if (tyIt != it->localTypes.end() && !tyIt->second.isAny() && !tyIt->second.contains(v.tag()))
                    vmError("cannot assign " + v.typeName() + " to '" + name + "' declared as " +
                            tyIt->second.name(), line);
                it->locals[name] = std::move(v);
                return;
            }
            if (it->captured && it->captured->count(name)) {
                (*it->captured)[name] = std::move(v); return;
            }
        }
        // not found in any frame
        // if it's an existing Interpreter global update it there instead of shadowing it with a new local
        if (interp_.has(name)) { interp_.set(name, std::move(v)); return; }
        // truly unseen name: create in innermost frame
        frames_.back().locals[name] = std::move(v);
    }

    // record that name was DEFINE'd while at least one SCOPE_PUSH is active in this frame so the matching SCOPE_POP knows to erase it
    // a name defined at frame scope depth isn't tracked here and correctly survives to the end of the frame
    void trackScopedDefine(CallFrame& f, const std::string& name) {
        if (!f.scopeNames.empty()) f.scopeNames.back().push_back(name);
    }

    // define (untyped) in the innermost frame. clears any stale type constraint from an earlier declaration of the same name
    void resolveDef(const std::string& name, Value v) {
        auto& f = frames_.back();
        f.locals[name] = std::move(v);
        f.localTypes.erase(name);
        trackScopedDefine(f, name);
    }

    void resolveDefTyped(const std::string& name, Value v, TypeSet ts, int line) {
        if (!ts.isAny() && !ts.contains(v.tag()))
            vmError("cannot assign " + v.typeName() + " to '" + name + "' declared as " + ts.name(), line);
        auto& f = frames_.back();
        f.locals[name] = std::move(v);
        if (!ts.isAny()) f.localTypes[name] = ts;
        else              f.localTypes.erase(name);
        trackScopedDefine(f, name);
    }

    void resolveDefAuto(const std::string& name, Value v) {
        auto& f = frames_.back();
        TypeSet ts(v.tag());
        f.locals[name] = std::move(v);
        f.localTypes[name] = ts;
        trackScopedDefine(f, name);
    }

    // snapshot all locals across the current call stack into a CaptureFrame.
    //
    // top-level names are never stored in frames_[0].locals in the first place so they're naturally absent from this snapshot
    // and a closure over one instead falls through resolveLoad()/resolveStore() to the live Interpreter scope.

    // x = 10
    // fn f()
    //   return x
    // end
    // x = 20
    // print(f())

    // prints 20 on both backends
    std::shared_ptr<CaptureFrame> snapshotLocals() const {
        auto cap = std::make_shared<CaptureFrame>();
        for (auto& f : frames_) {
            // a frame's own captured env must be included too, not just its direct locals
            // otherwise a closure nested two levels deep loses visibility into a variable the middle frame only has by way of its own capture
            // fn outer(a) fn inner(b) return fn(c) return a+b+c end end return inner end
            // when the innermost lambda is made, inner's frame has b in .locals but a only in .captured
            if (f.captured)
                for (auto& [k, v] : *f.captured) (*cap)[k] = v;
            for (auto& [k, v] : f.locals)
                (*cap)[k] = v;
        }
        return cap;
    }

    // dispatch loop

    Value dispatch() {
        while (state_ == State::Running && !frames_.empty()) {
            try {
                execOne();
            } catch (const EmbrError& e) {
                if (!handleTry(e, 0)) throw;
            }
        }
        if (state_ == State::Done || frames_.empty()) {
            state_ = State::Done;
            return stack_.empty() ? Value(0.0) : pop();
        }
        return Value(0.0); // suspended
    }

    // search frames_[floor..] from innermost outward for the nearest open
    // try handler and, if found, unwind execution state to it and redirect ip to its catch block
    // floor keeps a nested dispatch loop from reaching past its own frame into a caller's try
    // a handler below the floor is left for that caller's own loop to find once this exception propagates out of the C++ call that owns it
    // returns false and propagates if no handler in range is open
    bool handleTry(const EmbrError& e, int floor) {
        for (int fi = (int)frames_.size() - 1; fi >= floor; --fi) {
            if (frames_[fi].tryHandlers.empty()) continue;
            auto h = frames_[fi].tryHandlers.back();
            frames_[fi].tryHandlers.pop_back();

            // discard any frames pushed by calls that were still in progress when the error was raised
            while ((int)frames_.size() - 1 > fi) { frames_.pop_back(); interp_.exitCall(); }

            auto& f = frames_[fi];
            // unwind scopes the try block itself opened
            while (f.scopeDepth > h.scopeDepthAtEntry) {
                if (!f.scopeNames.empty()) {
                    for (auto& nm : f.scopeNames.back()) { f.locals.erase(nm); f.localTypes.erase(nm); }
                    f.scopeNames.pop_back();
                }
                f.scopeDepth--;
            }
            while (stack_.size() > h.stackBase) stack_.pop_back();

            push(Value(e.message)); // consumed by the catch block's own DEFINE
            f.ip = h.catchIp;
            injBuf_.reset();
            state_ = State::Running;
            return true;
        }
        return false;
    }

    // get the current instruction, advancing ip
    const Instruction& fetch() {
        auto& f = frames_.back();
        if (injBuf_ && f.ip < (int)injBuf_->size())
            return (*injBuf_)[f.ip++];
        // if exhausted the injection buffer, switch back to the chunk
        if (injBuf_ && f.ip >= (int)injBuf_->size()) {
            injBuf_.reset();
            f.ip = 0;
        }
        assert(f.ip < (int)f.chunk->code.size());
        return f.chunk->code[f.ip++];
    }

    bool atEnd() const {
        if (frames_.empty()) return true;
        const auto& f = frames_.back();
        if (injBuf_) return f.ip >= (int)injBuf_->size();
        return f.ip >= (int)f.chunk->code.size();
    }

    const std::string& chunkStr(int idx) const {
        return frames_.back().chunk->str(idx);
    }

    void execOne() {
        if (atEnd()) {
            // implicit return from a frame that ran off the end
            doReturn(Value(0.0));
            return;
        }

        const Instruction instr = fetch(); // copy, not reference (inject may reallocate)
        const int line = instr.line;

        switch (instr.op) {

        // literals
        case Op::PUSH_NUM:   push(Value(instr.numVal)); break;
        case Op::PUSH_INT:   push(Value(instr.intVal)); break;
        case Op::PUSH_STR:   push(Value(chunkStr(instr.operand))); break;
        case Op::PUSH_NIL:   push(Value(0.0)); break;

        case Op::POP: pop(); break;
        case Op::NOP: break;

        // variables
        case Op::LOAD:
            push(resolveLoad(chunkStr(instr.operand), line));
            break;

        case Op::STORE:
            resolveStore(chunkStr(instr.operand), pop(), line);
            break;

        case Op::DEFINE:
            resolveDef(chunkStr(instr.operand), pop());
            break;

        case Op::DEFINE_TYPED:
            resolveDefTyped(chunkStr(instr.operand), pop(), instr.typeHint, line);
            break;

        case Op::DEFINE_AUTO:
            resolveDefAuto(chunkStr(instr.operand), pop());
            break;

        case Op::LOAD_GLOBAL:
            if (!interp_.has(chunkStr(instr.operand)))
                vmError("undefined global: " + chunkStr(instr.operand), line);
            push(interp_.get(chunkStr(instr.operand)));
            break;

        case Op::STORE_GLOBAL:
            interp_.set(chunkStr(instr.operand), pop());
            break;

        case Op::DEFINE_GLOBAL:
            interp_.define(chunkStr(instr.operand), pop());
            break;

        case Op::DEFINE_GLOBAL_TYPED: {
            Value v = pop();
            const std::string& nm = chunkStr(instr.operand);
            if (!instr.typeHint.isAny() && !instr.typeHint.contains(v.tag()))
                vmError("cannot assign " + v.typeName() + " to '" + nm + "' declared as " +
                        instr.typeHint.name(), line);
            interp_.define(nm, std::move(v), instr.typeHint);
            break;
        }

        case Op::DEFINE_GLOBAL_AUTO: {
            Value v = pop();
            TypeSet ts(v.tag());
            interp_.define(chunkStr(instr.operand), std::move(v), ts);
            break;
        }

        // arithmetic
        // Int stays exact for +,-,*,%; mixed/float widens to double. division widens to float
        case Op::ADD: {
            Value r = pop(), l = pop();
            if (l.isString() || r.isString())
                push(Value(l.formatAsString() + r.formatAsString()));
            else if (l.isInt() && r.isInt())
                push(Value(l.asInt() + r.asInt()));
            else if (l.isNumeric() && r.isNumeric())
                push(Value(l.asNumber() + r.asNumber()));
            else
                vmError("unsupported operands for '+': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::SUB: {
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt()) push(Value(l.asInt() - r.asInt()));
            else                        push(Value(l.asNumber() - r.asNumber()));
            break;
        }
        case Op::MUL: {
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt()) push(Value(l.asInt() * r.asInt()));
            else                        push(Value(l.asNumber() * r.asNumber()));
            break;
        }
        case Op::DIV: {
            Value r = pop(), l = pop();
            double rd = r.asNumber();
            if (rd == 0.0) vmError("division by zero", line);
            push(Value(l.asNumber() / rd));
            break;
        }
        case Op::MOD: {
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt()) {
                int64_t rv = r.asInt();
                if (rv == 0) vmError("modulo by zero", line);
                push(Value(l.asInt() % rv));
            } else {
                double rd = r.asNumber();
                if (rd == 0.0) vmError("modulo by zero", line);
                push(Value(std::fmod(l.asNumber(), rd)));
            }
            break;
        }
        case Op::NEG: { Value v = pop(); push(v.isInt() ? Value(-v.asInt()) : Value(-v.asNumber())); break; }
        case Op::NOT: { Value v = pop(); push(Value(!v.truthy())); break; }

        // comparison
        case Op::EQ:  { Value r=pop(),l=pop(); push(Value(l==r)); break; }
        case Op::NEQ: { Value r=pop(),l=pop(); push(Value(l!=r)); break; }
        case Op::LT:  { Value r=pop(),l=pop(); push(Value(l.isInt()&&r.isInt() ? (l.asInt()<r.asInt())  : (l.asNumber()<r.asNumber())));  break; }
        case Op::GT:  { Value r=pop(),l=pop(); push(Value(l.isInt()&&r.isInt() ? (l.asInt()>r.asInt())  : (l.asNumber()>r.asNumber())));  break; }
        case Op::LTE: { Value r=pop(),l=pop(); push(Value(l.isInt()&&r.isInt() ? (l.asInt()<=r.asInt()) : (l.asNumber()<=r.asNumber()))); break; }
        case Op::GTE: { Value r=pop(),l=pop(); push(Value(l.isInt()&&r.isInt() ? (l.asInt()>=r.asInt()) : (l.asNumber()>=r.asNumber()))); break; }

        // control flow
        case Op::JUMP:
            frames_.back().ip = instr.operand;
            if (injBuf_) injBuf_.reset(); // jump clears injection buffer
            break;

        case Op::JUMP_IF_FALSE:
            if (!pop().truthy()) {
                frames_.back().ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        case Op::JUMP_IF_FALSE_PEEK:
            if (!peek().truthy()) {
                frames_.back().ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        case Op::JUMP_IF_TRUE_PEEK:
            if (peek().truthy()) {
                frames_.back().ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        // scope
        case Op::SCOPE_PUSH: {
            auto& f = frames_.back();
            f.scopeDepth++;
            f.scopeNames.emplace_back();
            break;
        }

        case Op::SCOPE_POP: {
            auto& f = frames_.back();
            if (f.scopeDepth > 0 && !f.scopeNames.empty()) {
                for (auto& name : f.scopeNames.back()) {
                    f.locals.erase(name);
                    f.localTypes.erase(name);
                }
                f.scopeNames.pop_back();
                f.scopeDepth--;
            }
            break;
        }

        // try/catch
        case Op::TRY_PUSH: {
            auto& f = frames_.back();
            f.tryHandlers.push_back({instr.operand, f.scopeDepth, stack_.size()});
            break;
        }

        case Op::TRY_POP:
            frames_.back().tryHandlers.pop_back();
            break;

        // destructuring
        case Op::UNPACK: {
            int count = instr.operand;
            Value src = pop();
            std::vector<Value> vals;
            if (src.isArray()) {
                const auto& arr = src.asArray();
                if ((int)arr.size() < count)
                    vmError("cannot unpack " + std::to_string(count) + " variable" + (count == 1 ? "" : "s") +
                            " from an array of " + std::to_string(arr.size()) + " element" +
                            (arr.size() == 1 ? "" : "s"), line);
                vals.assign(arr.begin(), arr.begin() + count);
            } else if (src.isMap()) {
                if (count != 2)
                    vmError("unpacking a map requires exactly 2 targets (keys, values), got " +
                            std::to_string(count), line);
                Value::array_type ks, vs;
                for (const auto& [k, v] : src.asMap()) { ks.push_back(Value(k)); vs.push_back(v); }
                vals = { Value(std::move(ks)), Value(std::move(vs)) };
            } else {
                vmError("cannot unpack " + src.typeName() + " into " + std::to_string(count) +
                         " variable" + (count == 1 ? "" : "s"), line);
            }
            for (int i = count - 1; i >= 0; --i) push(vals[i]);
            break;
        }

        // for
        case Op::ITER_INIT: {
            Value src = pop();
            IterState st;
            st.varCount = instr.operand;
            if (src.isArray()) {
                if (st.varCount == 2)
                    vmError("for loop over an array takes a single loop variable, not 'for k, v in ...'", line);
                st.items = src.asArray();
            } else if (src.isString()) {
                if (st.varCount == 2)
                    vmError("for loop over a string takes a single loop variable, not 'for k, v in ...'", line);
                for (char c : src.asString()) st.items.push_back(Value(std::string(1, c)));
            } else if (src.isMap()) {
                for (const auto& [k, v] : src.asMap()) {
                    if (st.varCount == 2) { st.keys.push_back(Value(k)); st.items.push_back(v); }
                    else                    st.items.push_back(Value(k));
                }
            } else {
                vmError("cannot iterate over " + src.typeName() + " with for loop", line);
            }
            frames_.back().iterStack.push_back(std::move(st));
            break;
        }

        case Op::ITER_NEXT: {
            auto& st = frames_.back().iterStack.back();
            if (st.idx >= st.items.size()) {
                frames_.back().ip = instr.operand;
                break;
            }
            if (st.varCount == 2) {
                push(st.keys[st.idx]);   // key (below)
                push(st.items[st.idx]);  // value (TOS)
            } else {
                push(st.items[st.idx]);
            }
            ++st.idx;
            break;
        }

        case Op::ITER_POP:
            frames_.back().iterStack.pop_back();
            break;

        // collections
        case Op::BUILD_ARRAY: {
            int n = instr.operand;
            Value::array_type arr(n);
            for (int i = n - 1; i >= 0; --i) arr[i] = pop();
            push(Value(std::move(arr)));
            break;
        }

        case Op::BUILD_MAP: {
            int n = instr.operand;
            Value::map_type m;
            // pairs were pushed in order: key0, val0, key1, val1, ...
            std::vector<std::pair<Value,Value>> pairs(n);
            for (int i = n - 1; i >= 0; --i) {
                pairs[i].second = pop(); // val
                pairs[i].first  = pop(); // key
            }
            for (auto& [k, v] : pairs) {
                if (!k.isString())
                    vmError("map keys must be strings", line);
                m[k.asString()] = std::move(v);
            }
            push(Value(std::move(m)));
            break;
        }

        case Op::INDEX_GET: {
            Value key  = pop();
            Value cont = pop();
            if (cont.isString()) {
                int ii = (int)key.asNumber();
                const auto& s = cont.asString();
                if (ii < 0 || ii >= (int)s.size())
                    vmError("string index " + std::to_string(ii) + " out of bounds", line);
                push(Value(std::string(1, s[ii])));
            } else if (cont.isMap()) {
                if (!key.isString())
                    vmError("map key must be string", line);
                const auto& m = cont.asMap();
                auto it = m.find(key.asString());
                if (it == m.end())
                    vmError("key not found: " + key.asString(), line);
                push(it->second);
            } else if (cont.isArray()) {
                int ii = (int)key.asNumber();
                const auto& a = cont.asArray();
                if (ii < 0 || ii >= (int)a.size())
                    vmError("array index " + std::to_string(ii) + " out of bounds", line);
                push(a[ii]);
            } else {
                vmError("cannot index " + cont.typeName(), line);
            }
            break;
        }

        case Op::INDEX_SET: {
            // stack: [container, key, new_value] (new_value on top)
            Value newVal = pop();
            Value key    = pop();
            Value cont   = pop();
            if (cont.isArray()) {
                int ii = (int)key.asNumber();
                auto arr = cont.asArray();
                if (ii < 0 || ii >= (int)arr.size())
                    vmError("array index " + std::to_string(ii) + " out of bounds", line);
                arr[ii] = std::move(newVal);
                push(Value(std::move(arr)));
            } else if (cont.isMap()) {
                if (!key.isString())
                    vmError("map key must be string", line);
                auto m = cont.asMap();
                m[key.asString()] = std::move(newVal);
                push(Value(std::move(m)));
            } else if (cont.isString()) {
                int ii = (int)key.asNumber();
                auto str = cont.asString();
                if (ii < 0 || ii >= (int)str.size())
                    vmError("string index " + std::to_string(ii) + " out of bounds", line);
                const std::string& sv = newVal.asString();
                str.replace(ii, sv.size(), sv);
                push(Value(std::move(str)));
            } else {
                vmError("cannot index-assign on " + cont.typeName(), line);
            }
            break;
        }

        // functions
        case Op::MAKE_CLOSURE: {
            int fnIdx = instr.operand;
            if (fnIdx < 0 || fnIdx >= (int)prog_->chunks.size())
                vmError("invalid function index " + std::to_string(fnIdx), line);
            auto chunk = prog_->chunks[fnIdx];
            ScriptFn sf;
            sf.name          = chunk->name;
            sf.params        = chunk->params;
            sf.returnType    = chunk->returnType;
            sf.definedAt     = chunk->definedAt;
            sf.sourceFile    = chunk->sourceFile;
            sf.captured      = snapshotLocals();
            sf.compiledChunk = chunk;
            push(Value::makeScript(std::move(sf)));
            break;
        }

        case Op::CALL: {
            int argc = instr.operand;

            // import sentinel
            if (argc == -1) {
                Value pathVal = pop();
                execImport(pathVal.asString(), line);
                push(Value(0.0));
                break;
            }

            // normal call stack is [callee, arg0, arg1, ..., argN]
            // args are on TOS, callee below them
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();
            Value callee = pop();

            if (!callee.isCallable())
                vmError("value is not callable (got " + callee.typeName() + ")", line);

            const auto& c = callee.asCallable();
            if (c.isNative()) {
                checkNativeSig(c.name(), c.sig, args, {});
                try {
                    push(c.native(args));
                } catch (EmbrError& e) {
                    vmError(e.message, line);
                }
                break;
            }

            // script call
            const ScriptFn& sf = c.script;
            if (sf.compiledChunk) {
                // push a new call frame and continue the dispatch loop
                auto chunk = std::static_pointer_cast<CompiledChunk>(sf.compiledChunk);
                pushFrame(chunk, sf.captured, args, sf.returnType, sf.name);
            } else {
#ifdef EMBR_WITH_TREE_WALKER
                // callee was compiled by the tree-walker so fall back to it
                Runner treeRunner(interp_);
                push(treeRunner.invoke(callee, args));
#else
                vmError("cannot invoke '" + sf.name + "': it was not compiled by the VM, and the "
                        "tree-walker backend is not compiled into this build", line);
#endif
            }
            break;
        }

        case Op::RETURN: {
            Value result = pop();
            doReturn(std::move(result));
            break;
        }

        // async
        case Op::YIELD:
            state_ = State::Suspended;
            break;

        } // end switch
    }

    // frame management

    void pushFrame(
        const std::shared_ptr<CompiledChunk>& chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        const std::vector<Value>&             args,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        bool   hasVariadic = !chunk->params.empty() && chunk->params.back().variadic;
        size_t fixedCount  = hasVariadic ? chunk->params.size() - 1 : chunk->params.size();

        // typecheck fixed params
        for (size_t i = 0; i < fixedCount && i < args.size(); ++i) {
            const auto& p = chunk->params[i];
            if (!p.type.isAny() && !p.type.contains(args[i].tag()))
                vmError("argument '" + p.name + "' to '" + fnName + "': expected " +
                        p.type.name() + " but got " + args[i].typeName(), 0);
        }
        if (args.size() < fixedCount)
            vmError("'" + fnName + "' expects " + (hasVariadic ? "at least " : "") +
                    std::to_string(fixedCount) + " args, got " + std::to_string(args.size()), 0);
        // typecheck the variadic tail, if it declares a type
        if (hasVariadic && !chunk->params.back().type.isAny()) {
            const auto& vp = chunk->params.back();
            for (size_t i = fixedCount; i < args.size(); ++i)
                if (!vp.type.contains(args[i].tag()))
                    vmError("argument " + std::to_string(i) + " to '" + fnName + "' ('..." + vp.name +
                            "'): expected " + vp.type.name() + " but got " + args[i].typeName(), 0);
        }

        CallFrame frame{};
        frame.chunk      = chunk;
        frame.stackBase  = (int)stack_.size();
        frame.captured   = captured;
        frame.returnType = returnType;
        frame.fnName     = fnName;

        for (size_t i = 0; i < fixedCount; ++i)
            frame.locals[chunk->params[i].name] = args[i];
        if (hasVariadic) {
            Value::array_type rest(args.begin() + fixedCount, args.end());
            frame.locals[chunk->params.back().name] = Value(std::move(rest));
        }

        // guards against unbounded recursion
        // only entered once the frame is actually about to be pushed
        // paired with the exitCall() in doReturn() where this exact frame is popped again
        // this frame's C++ lifetime outlives pushFrame() itself for a plain in-VM CALL
        // so a scope-based guard here wouldn't cover the frame's real lifetime
        interp_.enterCall();
        frames_.push_back(std::move(frame));
        // clear injection buffer when entering a new frame
        injBuf_.reset();
    }

    void doReturn(Value result) {
        if (frames_.empty()) { state_ = State::Done; push(std::move(result)); return; }

        if (frames_.size() == 1 && frames_[0].isPersistentTop) {
            // the persistent top-level frame
            // don't pop it, so its locals survive for a later run() call
            // discard anything the frame left on the stack and stop
            while ((int)stack_.size() > frames_[0].stackBase) stack_.pop_back();
            state_ = State::Done;
            push(std::move(result));
            return;
        }

        CallFrame leaving = std::move(frames_.back());
        frames_.pop_back();
        interp_.exitCall();

        // typecheck return value
        if (!leaving.returnType.isAny() && !leaving.returnType.contains(result.tag()))
            vmError("'" + leaving.fnName + "' declared return type " +
                    leaving.returnType.name() + " but returned " + result.typeName(), 0);

        // discard any values the returning frame left on the stack above its base
        while ((int)stack_.size() > leaving.stackBase) stack_.pop_back();

        if (frames_.empty()) state_ = State::Done;
        push(std::move(result)); // caller receives return value
    }

    // import
    void execImport(const std::string& rawPath, int line) {
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
            if (!interp_.scriptDir.empty())
                cands.push_back((fs::path(interp_.scriptDir) / withE).lexically_normal());
            cands.push_back((fs::current_path() / withE).lexically_normal());
        } else {
            fs::path name = withExt(p);
            if (!interp_.scriptDir.empty()) {
                fs::path base(interp_.scriptDir);
                cands.push_back(base / name);
                cands.push_back(base / "plugins" / name);
            }
            cands.push_back(fs::current_path() / name);
            cands.push_back(fs::current_path() / "plugins" / name);
        }

        PluginHandle handle = nullptr;
        std::string  loadedFrom;
        for (const auto& c : cands) {
            handle = pluginOpen(c.string().c_str());
            if (handle) { loadedFrom = c.string(); break; }
        }
        if (!handle) {
            std::string tried;
            for (const auto& c : cands) tried += "\n    " + c.string();
            vmError("cannot load plugin \"" + rawPath + "\": " + pluginError() +
                    "\n  tried:" + tried, line);
        }
        using RegFn = void(*)(Interpreter*);
        auto reg = reinterpret_cast<RegFn>(pluginSym(handle, "embr_register"));
        if (!reg) {
            pluginClose(handle);
            vmError("'embr_register' not found in \"" + loadedFrom + "\"", line);
        }
        reg(&interp_);
        interp_.pluginHandles_.push_back(handle);
        std::cout << "[runtime] loaded plugin: " << loadedFrom << "\n";
    }

    // invoke helper for the public invoke() method
    Value invokeChunk(
        const std::shared_ptr<CompiledChunk>& chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        const std::vector<Value>&             args,
        const SourceRange&                    /*site*/,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        // push new frame and run dispatch loop until the frame returns
        // record the depth to detect when the frame returns
        int targetDepth = (int)frames_.size();
        pushFrame(chunk, captured, args, returnType, fnName);

        State savedState = state_;
        state_ = State::Running;
        while ((int)frames_.size() > targetDepth && state_ == State::Running) {
            try {
                execOne();
            } catch (const EmbrError& e) {
                // a handler at or below targetDepth belongs to whatever outer call pushed frames up to targetDepth not to this invocation
                // leave it for that caller's own loop and let the exception propagate out of this call instead
                if (!handleTry(e, targetDepth)) throw;
            }
        }

        Value result = stack_.empty() ? Value(0.0) : pop();
        state_ = savedState;
        return result;
    }

    // native sig validation
    void checkNativeSig(const std::string& fname,
                        const std::vector<Param>& sig,
                        const std::vector<Value>& args,
                        const SourceRange& site) {
        if (sig.empty()) return;
        size_t minArgs = 0;
        bool   hasVariadic = false;
        for (const auto& p : sig) {
            if (p.variadic) { hasVariadic = true; break; }
            if (!p.optional) ++minArgs;
        }
        size_t maxArgs = hasVariadic ? SIZE_MAX : sig.size();
        if (args.size() < minArgs)
            vmError("too few arguments to '" + fname + "': expected " +
                    std::to_string(minArgs) + " but got " +
                    std::to_string(args.size()), site.startLine);
        if (args.size() > maxArgs)
            vmError("too many arguments to '" + fname + "': expected at most " +
                    std::to_string(maxArgs) + " but got " +
                    std::to_string(args.size()), site.startLine);
        for (size_t i = 0; i < args.size(); ++i) {
            const Param* p = (i < sig.size() && !sig[i].variadic) ? &sig[i] : nullptr;
            if (!p) for (auto it = sig.rbegin(); it != sig.rend(); ++it)
                if (it->variadic) { p = &*it; break; }
            if (!p || p->type.isAny()) continue;
            if (!p->type.contains(args[i].tag()))
                vmError("argument '" + p->name + "' to '" + fname + "': expected " +
                        p->type.name() + " but got " + args[i].typeName(), site.startLine);
        }
    }

    // error helper
    [[noreturn]] void vmError(const std::string& msg, int line) const {
        std::ostringstream out;
        out << "[vm] " << msg;
        if (line > 0) out << " at line " << line;
        out << "\n";
        if (line > 0) {
            const std::string& ln = interp_.sourceMap().get(line);
            if (!ln.empty()) out << "  " << ln << "\n";
        }
        throw EmbrError(out.str(), line > 0, {line, 0, line, 0}, msg);
    }

    static size_t editDist(const std::string& a, const std::string& b, size_t cap = 3) {
        if (a.size() > b.size() + cap || b.size() > a.size() + cap) return cap + 1;
        const size_t na = a.size(), nb = b.size();
        std::vector<std::vector<size_t>> dp(na+1, std::vector<size_t>(nb+1));
        for (size_t i = 0; i <= na; ++i) dp[i][0] = i;
        for (size_t j = 0; j <= nb; ++j) dp[0][j] = j;
        for (size_t i = 1; i <= na; ++i) {
            size_t rowMin = SIZE_MAX;
            for (size_t j = 1; j <= nb; ++j) {
                size_t cost = (a[i-1] == b[j-1]) ? 0 : 1;
                size_t v = std::min({dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+cost});
                if (i>1&&j>1&&a[i-1]==b[j-2]&&a[i-2]==b[j-1]) v=std::min(v,dp[i-2][j-2]+1);
                dp[i][j] = v; rowMin = std::min(rowMin, v);
            }
            if (rowMin > cap) return cap + 1;
        }
        return dp[na][nb];
    }

    // typo suggestions
    std::string suggest(const std::string& name) const {
        std::string best; size_t bestD = 3;
        for (auto& f : frames_) {
            for (auto& [k, _] : f.locals) {
                size_t d = editDist(name, k, bestD);
                if (d < bestD) { bestD = d; best = k; }
            }
        }
        for (auto& [k, _] : interp_.globals()) {
            size_t d = editDist(name, k, bestD);
            if (d < bestD) { bestD = d; best = k; }
        }
        return best;
    }
};


// public API

// compile a source string returning the program
inline CompiledProgram compileSource(const std::string& src,
                                     Interpreter& interp,
                                     const std::string& filename = "<input>") {
    Lexer lex(src);
    auto tokens = lex.tokenize();
    SourceMap sm = lex.sourceMap();
    interp.setSource(sm, filename);
    Parser parser(std::move(tokens), sm);
    auto ast = parser.parse();
    Compiler compiler(&interp);
    auto prog = compiler.compile(ast, filename);
    // keep the AST alive for as long as the Interpreter
    // a closure's ScriptFn::body still points into it even though the VM executes via compiledChunk, not body
    interp.storeProgram(std::move(ast));
    return prog;
}

// compile and run in one call
inline void runSource(const std::string& src,
                      Interpreter& interp,
                      const std::string& filename = "<input>") {
    auto prog = compileSource(src, interp, filename);
    VmRunner runner(interp);
    try {
        runner.run(prog);
    } catch (const EmbrError& e) {
        std::cerr << e.what();
    }
}

} // namespace vm
} // namespace embr

#endif // EMBR_WITH_VM
#endif // EMBR_BACKENDS_VM_H
