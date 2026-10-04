#ifndef EMBR_BACKENDS_VM_H
#define EMBR_BACKENDS_VM_H
// vm backend for embr
// compiles the AST to a flat list of instructions and runs them
//
// usage:
//   embr::Interpreter interp;
//   embr::vm::runSource(src, interp, "<input>");
//
// how it fits together:
//   Compiler   walks the AST once and emits instructions. each function body becomes its own CompiledChunk
//   VmRunner   runs the instructions and keeps its own stack of call frames
//   variables at true top level go through the *_GLOBAL opcodes into the Interpreter's global scope
//   a function compiled here keeps its chunk in ScriptFn::compiledChunk (type-erased, see core/value.h)
//
// YIELD pauses the runner. call step() or run() again to continue, optionally after inject()ing instructions
// inject() lets a plugin splice instructions into the running stream (loops, async continuations, ...)
// see the wiki, architecture page, for the big picture

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
#include <unordered_set>
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
    // operand = string-table index. emitted right after a DEFINE_GLOBAL* for a top-level `local`,
    // so a module importer leaves that name out of its exports. the tree-walker does this directly with
    // Interpreter::markModuleLocal, the VM needs an opcode because it compiles once and may run many times
    // no stack effect
    MARK_MODULE_LOCAL,

    // compiled from `x = x + simple` (see compileAssign). operand = name, slot = local slot or -1, intVal = ip to jump to.
    // if x holds a string, appends the popped value to it in place and jumps. otherwise changes nothing and falls through
    // to the ordinary load/add/store code. saves copying the whole string every time round a loop
    APPEND_VAR,

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
    // compiled from `name[simple]`. operand = name, slot = local slot or -1. pops only the key and reads the variable in
    // place, so indexing a big array doesn't copy it first. only used when the key can't run user code (see compileIndexGet)
    // with kStash set, a STASH_VAR for the same name ran first and its result sits under the key
    INDEX_GET_VAR,
    INDEX_SET,      // pops container + key + value, pushes updated container, then STORE follows
    // compiled from `name[key] = value`. operand = name, slot = local slot or -1, intVal = ip after the STORE that follows.
    // pops value and key (and the stash, with kStash). if the variable can be changed where it lives, changes it there and
    // jumps over the STORE. otherwise pushes the updated copy and falls through to the STORE
    INDEX_SET_VAR,
    // pushes what a later INDEX_SET_VAR / APPEND_ARR needs of the variable `name` before the key / value code runs. if that
    // code can't change the variable (it is a local of this frame, or kCallFree, or kPureCalls with every callee still native)
    // this is a placeholder and the variable is read when it is needed. otherwise it is a copy, so the key / value code
    // can't change what gets indexed. intVal = index into CompiledChunk::pureSets for kPureCalls
    STASH_VAR,
    // compiled from `name = push(name, x)`. stack: [push, stash, x]. when push is embrlib's and the variable is an array,
    // appends in place, pops all three and jumps to intVal. otherwise leaves the stack alone so CALL + STORE run as usual
    APPEND_ARR,
    // compiled from `&name[k1]..[kn]` in a call argument. operand = name, slot = local slot or -1, intVal = n.
    // pops the n keys, pushes a reference path (value.h). the CALL that follows (flags kHasRef) turns it into a reference
    REF_PATH,

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
    // local slot picked at compile time for LOAD/STORE/DEFINE/DEFINE_TYPED/DEFINE_AUTO
    // (CompiledChunk::internLocal). -1 for every other opcode and for the *_GLOBAL variants
    int     slot    = -1;
    int     flags   = 0;                       // kStash / kCallFree, for the variable-in-place ops above
};

// Instruction::flags
constexpr int kStash    = 1;   // a STASH_VAR result sits under the operands
constexpr int kCallFree = 2;   // the code between STASH_VAR and the op that uses it can't run script code
constexpr int kHasRef   = 8;   // CALL: some argument is a REF_PATH
constexpr int kPureCalls = 4;  // ... as long as the builtins it calls still are builtins, see CompiledChunk::pureSets


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

    // every function chunk from the same compile() call, this one included. MAKE_CLOSURE looks its function up here,
    // not in the program running right now: a closure exported from a module can run long after that module's program is gone
    // a chunk holds a pointer to itself on purpose, so compiled chunks are never freed (the tree-walker never frees its AST either)
    std::shared_ptr<std::vector<std::shared_ptr<CompiledChunk>>> siblings;

    // every name this chunk defines or assigns locally (not through a *_GLOBAL opcode) gets a slot number the first time
    // the compiler sees it. a frame keeps its locals in a flat array indexed by that number, so LOAD/STORE don't hash
    // the name at runtime. see CallFrame::slots and CallFrame::locals
    std::unordered_map<std::string, int> localSlots;
    int numLocalSlots = 0;

    // for STASH_VAR with kPureCalls: the string-table indexes of the builtins (str, len, ...) the key / value code calls.
    // they are only safe to ignore while each one still resolves to a native function, which STASH_VAR checks when it runs
    std::vector<std::vector<int>> pureSets;

    int internLocal(const std::string& name) {
        auto it = localSlots.find(name);
        if (it != localSlots.end()) return it->second;
        int slot = numLocalSlots++;
        localSlots.emplace(name, slot);
        return slot;
    }

    // -1 if `name` was never compiled as a local of this chunk (e.g. it only
    // ever appears as a global/captured read)
    int slotOf(const std::string& name) const {
        auto it = localSlots.find(name);
        return it != localSlots.end() ? it->second : -1;
    }

    // intern a string, return its index
    int intern(const std::string& s) {
        for (int i = 0; i < (int)strings.size(); ++i)
            if (strings[i] == s) return i;
        strings.push_back(s);
        return (int)strings.size() - 1;
    }

    // LOAD_GLOBAL / STORE_GLOBAL remember where a global lives, so a loop doesn't hash its name on every pass.
    // `epoch` is the Interpreter::scopeEpoch() the pointer was found under; once it differs, look the name up again
    struct GlobalCache {
        Value*   ptr   = nullptr;
        uint64_t epoch = 0;
        bool     typed = false;   // the variable has a declared type, so a store has to go through Interpreter::set()
    };
    mutable std::vector<GlobalCache> globalCache;   // one per string-table entry, filled in as it is used
    GlobalCache& globalCacheFor(int idx) const {
        if (globalCache.size() != strings.size()) globalCache.resize(strings.size());
        return globalCache[idx];
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

        // point every chunk's siblings at the same shared list before any of them can escape as a Value
        // (MAKE_CLOSURE needs it, see CompiledChunk::siblings)
        auto siblings = std::make_shared<std::vector<std::shared_ptr<CompiledChunk>>>(prog_.chunks);
        for (auto& c : prog_.chunks) c->siblings = siblings;

        return std::move(prog_);
    }

private:
    [[maybe_unused]] const Interpreter* interp_;  // kept for the LOAD vs LOAD_GLOBAL idea above, nothing reads it yet
    CompiledProgram       prog_;
    CompiledChunk*        cur_  = nullptr;  // current chunk being written

    // compile-time mirror of the SCOPE_PUSH/SCOPE_POP nesting depth emitted so far in the current chunk, so
    // break/continue know how many SCOPE_POPs to backfill before jumping out of the scopes between them and their loop
    int scopeDepth_ = 0;

    // nesting depth of try statements, so break/continue know how many TRY_POPs to backfill when jumping out of
    // a try block that sits inside the loop
    int tryDepth_ = 0;

    struct LoopCtx {
        std::vector<int> breakJumps;   // JUMP instrs to patch once the loop's single exit point is known
        int continueTarget;            // instruction index 'continue' jumps to (the loop's re-check point)
        int scopeDepthAtEntry;         // scopeDepth_ when this loop's body started
        int tryDepthAtEntry;           // tryDepth_ when this loop's body started
    };
    std::vector<LoopCtx> loopStack_;

    // emit helpers
    void emit(Op op, int operand = 0, double num = 0.0, int line = 0, TypeSet th = TypeSet::Any(), int slot = -1) {
        Instruction i; i.op = op; i.operand = operand; i.numVal = num; i.line = line; i.typeHint = th; i.slot = slot;
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
            bool hasRef = false;
            for (auto& arg : c->args) {
                if (arg->kind == Expr::Kind::Ref) { compileRefArg(static_cast<RefExpr*>(arg.get())); hasRef = true; }
                else compileExpr(arg.get());
            }
            emit(Op::CALL, (int)c->args.size(), 0.0, e->range.startLine);
            if (hasRef) cur_->code.back().flags = kHasRef;
            break;
        }

        case Expr::Kind::Ref:
            raiseError("compiler", "'&' only works on an argument of a call", e->range);

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
            if (idx->object->kind == Expr::Kind::Var) {
                // the variable is read after the key is worked out, which only matters if the key could change it.
                // a key with no calls can't. one with calls gets a STASH_VAR first, see there
                const std::string& nm = static_cast<VarExpr*>(idx->object.get())->name;
                std::vector<std::string> callees;
                Purity p = purityOf(idx->index.get(), &callees);
                if (p != Purity::CallFree) emitStash(nm, e->range.startLine, p, callees);
                compileExpr(idx->index.get());
                emitVarOp(Op::INDEX_GET_VAR, nm, e->range.startLine, p == Purity::CallFree ? 0 : kStash);
                break;
            }
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

    // emits one of the variable-in-place ops (STASH_VAR, INDEX_GET_VAR, INDEX_SET_VAR, APPEND_ARR) for `name`.
    // returns its index so a jump target can be patched into it
    int emitVarOp(Op op, const std::string& name, int line, int flags) {
        emit(op, cur_->intern(name), 0.0, line, TypeSet::Any(), atTopLevel() ? -1 : cur_->internLocal(name));
        cur_->code.back().flags = flags;
        return (int)cur_->code.size() - 1;
    }
    // STASH_VAR for code of the given purity (see purityOf). `callees` are the pure builtins it calls
    void emitStash(const std::string& name, int line, Purity p, const std::vector<std::string>& callees) {
        int flags = p == Purity::CallFree ? kCallFree : p == Purity::PureCalls ? kPureCalls : 0;
        int at = emitVarOp(Op::STASH_VAR, name, line, flags);
        if (p == Purity::PureCalls) {
            std::vector<int> set;
            for (auto& c : callees) set.push_back(cur_->intern(c));
            cur_->code[at].intVal = (int)cur_->pureSets.size();
            cur_->pureSets.push_back(std::move(set));
        }
    }

    // `&name`, `&name[k1][k2]`: pushes the keys in order, then REF_PATH turns them into a reference path (see value.h)
    void compileRefArg(RefExpr* r) {
        std::vector<IndexExpr*> chain;
        Expr* cur = r->target.get();
        while (cur->kind == Expr::Kind::Index) { chain.push_back(static_cast<IndexExpr*>(cur)); cur = chain.back()->object.get(); }
        const std::string& nm = static_cast<VarExpr*>(cur)->name;
        for (size_t i = chain.size(); i-- > 0; ) compileExpr(chain[i]->index.get());
        int at = emitVarOp(Op::REF_PATH, nm, r->range.startLine, 0);
        cur_->code[at].intVal = (int64_t)chain.size();
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
                emit(Op::DEFINE, cur_->intern(f->varName), 0.0, line, TypeSet::Any(), cur_->internLocal(f->varName)); // pops value (TOS)
                emit(Op::DEFINE, cur_->intern(f->keyName),  0.0, line, TypeSet::Any(), cur_->internLocal(f->keyName)); // pops key
            } else {
                emit(Op::DEFINE, cur_->intern(f->varName), 0.0, line, TypeSet::Any(), cur_->internLocal(f->varName));
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
            // the parser rejects break/continue outside a loop, so loopStack_ should be non-empty;
            // check anyway, back() on an empty vector is undefined behaviour
            if (loopStack_.empty()) raiseError("compiler", "'break' outside a loop", s->range);
            auto& ctx = loopStack_.back();
            for (int i = 0; i < scopeDepth_ - ctx.scopeDepthAtEntry; ++i) emit(Op::SCOPE_POP);
            for (int i = 0; i < tryDepth_ - ctx.tryDepthAtEntry; ++i) emit(Op::TRY_POP);
            ctx.breakJumps.push_back(emitJump(Op::JUMP, s->range.startLine));
            break;
        }

        case Stmt::Kind::Continue: {
            if (loopStack_.empty()) raiseError("compiler", "'continue' outside a loop", s->range);
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
            emit(Op::DEFINE, cur_->intern(t->catchVar), 0.0, line, TypeSet::Any(), cur_->internLocal(t->catchVar)); // pops the error value handleTry() pushed
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
            // import changes the Interpreter (loads a plugin or a module), so it compiles to a call to the
            // built-in __import__ helper, which the VM hands to execImport. the path is a normal expression,
            // not always a string literal, so it's compiled like any other argument
            auto* im = static_cast<ImportStmt*>(s);
            compileExpr(im->pathExpr.get());
            // operand -1 = sentinel so the VM knows this CALL is an import
            emit(Op::CALL, -1, 0.0, s->range.startLine);
            break;
        }

        }
    }

    // emits the right DEFINE variant for a local/destructuring target. at true top level that is the
    // *_GLOBAL variant, plus a module-local mark so an importer doesn't export the name
    // (same as the tree-walker's Runner::defineTyped)
    void emitDefineTarget(const DestructureTarget& t, int line) {
        bool g = atTopLevel();
        int  slot = g ? -1 : cur_->internLocal(t.name);
        if (t.isAuto)              emit(g ? Op::DEFINE_GLOBAL_AUTO  : Op::DEFINE_AUTO,  cur_->intern(t.name), 0.0, line, TypeSet::Any(), slot);
        else if (!t.type.isAny())  emit(g ? Op::DEFINE_GLOBAL_TYPED : Op::DEFINE_TYPED, cur_->intern(t.name), 0.0, line, t.type, slot);
        else                       emit(g ? Op::DEFINE_GLOBAL       : Op::DEFINE,       cur_->intern(t.name), 0.0, line, TypeSet::Any(), slot);
        if (g) emit(Op::MARK_MODULE_LOCAL, cur_->intern(t.name), 0.0, line);
    }

    // compile an assignment, including chained index write-backs
    void compileAssign(AssignStmt* a) {
        if (a->target->kind == Expr::Kind::Var) {
            auto* ve = static_cast<VarExpr*>(a->target.get());
            int ln = a->range.startLine;
            // `x = x + simple`: try to append in place. the right side is compiled twice (once for the fast path, once for the
            // fallback), so it must be something that has no side effects
            if (a->value->kind == Expr::Kind::Binary) {
                auto* b = static_cast<BinaryExpr*>(a->value.get());
                bool simple = b->right->kind == Expr::Kind::String || b->right->kind == Expr::Kind::Var ||
                              b->right->kind == Expr::Kind::Int    || b->right->kind == Expr::Kind::Number;
                if (b->op == "+" && simple && b->left->kind == Expr::Kind::Var &&
                    static_cast<VarExpr*>(b->left.get())->name == ve->name) {
                    bool g = atTopLevel();
                    compileExpr(b->right.get());
                    int at = (int)cur_->code.size();
                    emit(Op::APPEND_VAR, cur_->intern(ve->name), 0.0, ln, TypeSet::Any(), g ? -1 : cur_->internLocal(ve->name));
                    emit(Op::POP, 0, 0.0, ln);
                    compileExpr(a->value.get());
                    emitStore(ve->name, ln);
                    cur_->code[at].intVal = (int)cur_->code.size();
                    return;
                }
            }
            // `x = push(x, v)`: same idea, grows the array in place when push is embrlib's. the stack is
            // [push, stash, v], which is exactly what the ordinary call needs if the fast path doesn't apply
            if (a->value->kind == Expr::Kind::Call) {
                auto* c = static_cast<CallExpr*>(a->value.get());
                if (c->callee->kind == Expr::Kind::Var && static_cast<VarExpr*>(c->callee.get())->name == "push" &&
                    c->args.size() == 2 && c->args[0]->kind == Expr::Kind::Var &&
                    static_cast<VarExpr*>(c->args[0].get())->name == ve->name) {
                    std::vector<std::string> callees;
                    Purity p = purityOf(c->args[1].get(), &callees);
                    compileExpr(c->callee.get());
                    emitStash(ve->name, ln, p, callees);
                    compileExpr(c->args[1].get());
                    int at = emitVarOp(Op::APPEND_ARR, ve->name, ln, 0);
                    emit(Op::CALL, 2, 0.0, ln);
                    emitStore(ve->name, ln);
                    cur_->code[at].intVal = here();
                    return;
                }
            }
            compileExpr(a->value.get());
            emitStore(ve->name, ln);
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
        if (idx->object->kind == Expr::Kind::Var) {
            compileVarIndexSet(static_cast<VarExpr*>(idx->object.get())->name, idx->index.get(), rhs, line,
                               [&] { compileExpr(rhs); });
            return;
        }
        compileExpr(idx->object.get());  // container
        compileExpr(idx->index.get());   // key
        compileExpr(rhs);                // new value
        emit(Op::INDEX_SET, 0, 0.0, line);
        // stack now has the updated container; write it back
        writeBackTarget(idx->object.get(), line);
    }

    // `name[key] = <whatever emitValue pushes>`. INDEX_SET_VAR changes the variable where it lives when it can, and jumps over the
    // STORE. otherwise it leaves an updated copy for the STORE, which is what INDEX_SET did every time
    template <class F>
    void compileVarIndexSet(const std::string& name, Expr* key, const Expr* value, int line, F emitValue) {
        std::vector<std::string> callees;
        Purity p = purityOf(key, &callees);
        if (value) { Purity pv = purityOf(value, &callees); if (pv > p) p = pv; }   // null value: nothing to run
        if (p != Purity::CallFree) emitStash(name, line, p, callees);
        compileExpr(key);
        emitValue();
        int at = emitVarOp(Op::INDEX_SET_VAR, name, line, p == Purity::CallFree ? 0 : kStash);
        emitStore(name, line);
        cur_->code[at].intVal = here();
    }

    // after INDEX_SET leaves the updated container on the stack store it back into the appropriate target
    void writeBackTarget(Expr* target, int line) {
        if (target->kind == Expr::Kind::Var) {
            auto* ve = static_cast<VarExpr*>(target);
            emitStore(ve->name, line);
        } else if (target->kind == Expr::Kind::Index) {
            auto* idx = static_cast<IndexExpr*>(target);
            std::string tmp = "__wb_tmp_" + std::to_string(here());
            int tmpSlot = cur_->internLocal(tmp);
            emit(Op::DEFINE, cur_->intern(tmp), 0.0, line, TypeSet::Any(), tmpSlot);  // pop+store the updated sub-container
            if (idx->object->kind == Expr::Kind::Var) {
                compileVarIndexSet(static_cast<VarExpr*>(idx->object.get())->name, idx->index.get(), nullptr, line, [&] {
                    emit(Op::LOAD, cur_->intern(tmp), 0.0, line, TypeSet::Any(), tmpSlot);
                });
                return;
            }
            compileExpr(idx->object.get());                  // outer container
            compileExpr(idx->index.get());                   // outer key
            emit(Op::LOAD, cur_->intern(tmp), 0.0, line, TypeSet::Any(), tmpSlot);    // reload updated sub-container as the value
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
        // every parameter gets a slot up front, even one the body never
        // references, pushFrame() binds args straight into slots by name
        // via slotOf(), which must never miss for a declared parameter.
        for (auto& p : params) chunk->internLocal(p.name);

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
        bool g = atTopLevel();
        emit(g ? Op::LOAD_GLOBAL : Op::LOAD, cur_->intern(name), 0.0, line, TypeSet::Any(), g ? -1 : cur_->internLocal(name));
    }

    void emitStore(const std::string& name, int line) {
        bool g = atTopLevel();
        emit(g ? Op::STORE_GLOBAL : Op::STORE, cur_->intern(name), 0.0, line, TypeSet::Any(), g ? -1 : cur_->internLocal(name));
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

// every chunk of a compiled program shares one sibling list that contains the chunks themselves (a
// deliberate strong cycle, see CompiledChunk::siblings). register a teardown hook so the list is emptied when the
// Interpreter goes away and the chunks are freed instead of leaked at exit. weak_ptr: the hook must not keep
// the list alive by itself. call this for every program compiled for an Interpreter (scripts and modules).
inline void registerChunkTeardown(Interpreter& interp, const CompiledProgram& prog) {
    if (prog.chunks.empty() || !prog.chunks[0]->siblings) return;
    interp.addTeardownHook([w = std::weak_ptr<std::vector<std::shared_ptr<CompiledChunk>>>(prog.chunks[0]->siblings)] {
        if (auto list = w.lock()) list->clear();
    });
}

// one activation record on the call stack
struct CallFrame {
    std::shared_ptr<CompiledChunk>           chunk;       // the function being executed
    int                                      ip = 0;      // instruction pointer into chunk->code
    int                                      stackBase;   // index of this frame's first local on the value stack

    // fast path: this frame's locals, indexed by the slot the compiler picked for each name
    // (CompiledChunk::internLocal). slotDefined[i] says whether slot i holds a value right now:
    // cleared by SCOPE_POP, set by DEFINE or the first STORE
    // slots is a shared_ptr so its address survives frames_ growing, because the gc has it registered as a root
    std::shared_ptr<std::vector<Value>>      slots = std::make_shared<std::vector<Value>>();
    std::vector<bool>                        slotDefined;

    // slow path: null until a closure captures this frame (see VmRunner::promoteToScope()). after that every
    // local access for this frame goes through this Scope by name instead of through slots, because the
    // closure (and the gc) now looks at this object, not at slots. see CaptureFrame in core/value.h
    std::shared_ptr<Scope>                   locals;
    bool promoted() const { return (bool)locals; }

    // this frame's own local `name`, or null if it isn't defined here. does not look at outer frames,
    // captured variables or globals (callers do that). knownSlot lets the current frame skip the name
    // lookup by passing the slot the compiler already picked. leave it at -2 for any other frame
    Value* findOwn(const std::string& name, int knownSlot = -2) {
        if (locals) {
            auto it = locals->find(name);
            return it != locals->end() ? &it->second : nullptr;
        }
        int slot = knownSlot != -2 ? knownSlot : chunk->slotOf(name);
        if (slot < 0 || slot >= (int)slotDefined.size() || !slotDefined[slot]) return nullptr;
        return &(*slots)[slot];
    }

    // unconditionally (re)defines this frame's own local, exactly like the
    // old `(*locals)[name] = v`, used by DEFINE* (always the current
    // frame) and by resolveStore()'s auto-vivify-in-innermost-frame path.
    void defineOwn(const std::string& name, Value v, int knownSlot = -2) {
        if (locals) { (*locals)[name] = std::move(v); return; }
        int slot = knownSlot != -2 ? knownSlot : chunk->slotOf(name);
        // every DEFINE/STORE target was given a slot at compile time. getting here without one means
        // a name reached the own-local path without being compiled as a local, which should not happen
        assert(slot >= 0);
        (*slots)[slot] = std::move(v);
        slotDefined[slot] = true;
    }

    // clears this frame's own local (SCOPE_POP and try-catch scope unwind)
    void eraseOwn(const std::string& name) {
        if (locals) { locals->erase(name); return; }
        int slot = chunk->slotOf(name);
        if (slot >= 0 && slot < (int)slotDefined.size()) slotDefined[slot] = false;
    }

    std::unordered_map<std::string, TypeSet> localTypes;  // set by DEFINE_TYPED/DEFINE_AUTO; consulted by STORE
    std::shared_ptr<CaptureFrame>            captured;    // closure env may be null
    TypeSet                                  returnType = TypeSet::Any();
    std::string                              fnName;

    // the gc root entry for this frame (from Interpreter::gcPushVmFrameRoots). promoteToScope() adds
    // this frame's new `locals` to it later. we keep the handle because the frame's position in the
    // shared root list can't be safely recomputed
    std::shared_ptr<std::vector<std::shared_ptr<Scope>>> gcRootsHandle;

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

    // true for a frame pushed by invokeChunk(): entered because native code (a plugin callback,
    // for_each_do, sort_cmp, ...) called into script code, not by a script CALL instruction, so there is
    // no script call site, error traces report such a frame as "called from native code" (line 0),
    // exactly like the tree-walker's doInvoke does for an invoke() with no call-site range
    bool fromNative = false;

    // set only on the persistent frame VmRunner::run() creates for its very first call
    // doReturn() special-cases it to survive running off the end of its chunk so locals defined by one run() call are still there for the next
    bool isPersistentTop = false;
};

class VmRunner {
public:
    enum class State { Ready, Running, Suspended, Done, Error };

    explicit VmRunner(Interpreter& interp) : interp_(interp) {}

    // the destructor deliberately doesn't touch interp_. cli.cpp keeps its VmRunner in a static that
    // outlives the Interpreter, so interp_ is already gone by then
    // the cost: frames still on frames_ stay registered as gc roots, which keeps their scopes alive a
    // bit longer instead of crashing. accepted limitation

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
            frame.slots->assign(frame.chunk->numLocalSlots, Value());
            frame.slotDefined.assign(frame.chunk->numLocalSlots, false);
            interp_.gcPushVmFrameValueRoots(frame.slots);
            frame.gcRootsHandle = interp_.gcPushVmFrameRoots({});  // its own locals join this later if the frame is promoted, see promoteToScope()
            frames_.push_back(std::move(frame));
        } else {
            while (frames_.size() > 1) {
                interp_.gcReleaseVmFrame(frames_.back().gcRootsHandle, frames_.back().slots);
                frames_.pop_back();
                interp_.exitCall();
            }
            stack_.clear();
            auto& f = frames_[0];
            if (!f.promoted() && f.chunk.get() != prog.chunks[0].get()) {
                // reusing the persistent top-level frame for a different program: the slot numbers belong to the
                // old chunk, so switch this frame to the by-name Scope now (while chunk still points at the old chunk)
                // and earlier top-level locals survive the swap. a frame never goes back to slots, so a reused
                // session pays for name lookups from here on. a script that runs once never does
                promoteToScope(f);
            }
            f.chunk = prog.chunks[0];
            f.ip    = 0;
            f.scopeDepth = 0;
            f.scopeNames.clear();
            f.iterStack.clear();
            if (!f.promoted()) {
                f.slots->assign(f.chunk->numLocalSlots, Value());
                f.slotDefined.assign(f.chunk->numLocalSlots, false);
            }
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

    // start a fresh runner directly on a VM-compiled script function (instead of a whole program like run())
    // the callee's chunk becomes the first frame and runs through the same dispatch() loop as run()/resume()
    // use this, not invoke()/invokeChunk(), when the code must be able to YIELD: invoke() assumes the
    // callee always runs to completion and would drop the suspended frames. coroutine_create() does this,
    // with one VmRunner per coroutine. call it once per runner, then resume() for every later call
    Value start(const Value& callee, const std::vector<Value>& args, const SourceRange& site = {}) {
        if (!frames_.empty())
            vmError("VmRunner::start() called on a runner that has already been started", site.startLine);
        if (!callee.isCallable())
            vmError("cannot start: value is not callable (got " + callee.typeName() + ")", site.startLine);
        const auto& c = callee.asCallable();
        if (!c.isScript() || !c.script.compiledChunk)
            vmError("cannot start: callee must be a script function compiled by the VM backend "
                    "(run embr with --vm)", site.startLine);
        auto chunk = std::static_pointer_cast<CompiledChunk>(c.script.compiledChunk);
        pushFrame(chunk, c.script.captured, args, c.script.returnType, c.script.name);
        state_ = State::Running;
        return dispatch();
    }

    // pause this runner right now, as if Op::YIELD had just run. for a native function (called via CALL)
    // that wants to hand control back to whoever called run()/resume()/start(). simpler than inject()ing
    // a YIELD because it doesn't touch injBuf_. the caller is coroutine_yield() in plugins/coroutine
    void suspend() { state_ = State::Suspended; }

    // drops every frame this runner still has: gc roots and call-depth count go back to the Interpreter. for a
    // coroutine that is dropped while suspended, or died with an error. the destructor can't do it (see above), so the
    // owner calls this while the Interpreter is still alive
    void releaseFrames() {
        while (!frames_.empty()) {
            CallFrame& f = frames_.back();
            interp_.gcReleaseVmFrame(f.gcRootsHandle, f.slots);
            if (!f.isPersistentTop) interp_.exitCall();
            frames_.pop_back();
        }
        stack_.clear();
        injBuf_.reset();
        state_ = State::Done;
    }

    // replace the value on top of the stack, or push one if the stack is empty. coroutine_resume() uses it:
    // coroutine_yield() pushes a placeholder return value before suspending (a native must return something),
    // and on resume this swaps the placeholder for the values passed to coroutine_resume()
    void setYieldResult(Value v) {
        if (stack_.empty()) stack_.push_back(std::move(v));
        else                stack_.back() = std::move(v);
    }

    // invoke a callable from C++. used by embr::invoke() and by native functions that wanna operate on runner's frame stack directly
    Value invoke(const Value& callee, const std::vector<Value>& args,
                 const SourceRange& site = {}) {
        if (!callee.isCallable()) {
            // dunder fallback, see core/registry.h's resolveDunder() and
            // tree_walker.h's doInvoke, which does the same thing.
            Value dfn;
            if (resolveDunder(interp_, callee, "call", dfn)) {
                std::vector<Value> dargs;
                dargs.reserve(args.size() + 1);
                dargs.push_back(callee);
                dargs.insert(dargs.end(), args.begin(), args.end());
                return invoke(dfn, dargs, site);
            }
            vmError("value is not callable (got " + callee.typeName() + ")", site.startLine);
        }

        const auto& c = callee.asCallable();
        if (c.isNative()) {
            checkNativeSig(c.name(), c.sig, args, site);
            try {
                return c.native(args);
            } catch (EmbrError& e) {
                if (!e.hasLocation && site.valid()) {
                    // carry over trace frames collected if this native called back into script code
                    try { raiseError(c.name(), e.message, site, interp_.sourceMap()); }
                    catch (EmbrError& located) { located.trace = std::move(e.trace); throw; }
                }
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
    //
    // the new code goes in at the current ip, so injecting twice in a row only goes in front of what
    // hasn't run yet. inserting at the start of the buffer would replay instructions that had already
    // run, and could loop forever
    void inject(std::vector<Instruction> code) {
        auto& cur = frames_.back();
        if (!injBuf_) {
            // copy remaining instructions from the current chunk into a mutable buffer
            auto remaining = std::vector<Instruction>(
                cur.chunk->code.begin() + cur.ip,
                cur.chunk->code.end());
            injBuf_ = std::make_unique<std::vector<Instruction>>(std::move(remaining));
            cur.ip = 0;
        }
        // insert right before whatever hasn't run yet (not at the start of the buffer)
        injBuf_->insert(injBuf_->begin() + cur.ip, code.begin(), code.end());
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

    // container[key] for INDEX_GET and INDEX_GET_VAR: pushes the element. never calls into script code
    void indexGet(const Value& cont, const Value& key, int line) {
        if (cont.isString()) {
            int ii = (int)key.asNumber();
            const auto& s = cont.asString();
            if (ii < 0 || ii >= (int)s.size())
                vmError("string index " + std::to_string(ii) + " out of bounds", line);
            push(Value(std::string(1, s[ii])));
        } else if (cont.isMap()) {
            if (!key.isString())
                vmError("map key must be string", line);
            // own slot, then (if key isn't one) the map's own "__proto__" chain, see lookupMapChain() in core/registry.h
            Value found;
            if (lookupMapChain(interp_, cont, key.asString(), found)) push(found);
            else vmError("key not found: " + key.asString(), line);
        } else if (cont.isArray()) {
            int ii = (int)key.asNumber();
            const auto& a = cont.asArray();
            if (ii < 0 || ii >= (int)a.size())
                vmError("array index " + std::to_string(ii) + " out of bounds", line);
            push(a[ii]);
        } else {
            vmError("cannot index " + cont.typeName(), line);
        }
    }

    // the variable an in-place op (INDEX_GET_VAR and friends) works on, or null if there is none. same lookup as LOAD
    Value* varForOp(const Instruction& in) { return varBySlot(in.operand, in.slot); }
    Value* varBySlot(int nameIdx, int slot) {
        CallFrame& f = frames_.back();
        if (slot < 0) return globalPtrOrNull(nameIdx);   // top-level code: a global
        if (slotIsOwnLocal(f, slot)) return &(*f.slots)[slot];
        if (onlyGlobalsLeft(f)) return globalPtrOrNull(nameIdx);
        return findVar(chunkStr(nameIdx), slot);
    }
    // a local of the running frame that no closure can see, so only this frame's own code can change it
    static bool slotIsOwnLocal(const CallFrame& f, int slot) {
        return !f.locals && slot >= 0 && slot < (int)f.slotDefined.size() && f.slotDefined[slot];
    }

    // true when every builtin this STASH_VAR's code calls still resolves to a native function (not a script function of the same name)
    bool pureCalleesNative(const Instruction& in) {
        CallFrame& f = frames_.back();
        for (int idx : f.chunk->pureSets[in.intVal]) {
            const Value* c = onlyGlobalsLeft(f) ? globalPtrOrNull(idx) : findVar(chunkStr(idx), -2);
            if (!c || !c->isCallable() || !c->asCallable().isNative()) return false;
        }
        return true;
    }

    // the Value STASH_VAR pushes when the variable is read later instead. no real Value has this nesting depth
    static constexpr uint32_t kStashMark = 0xFFFFFFFFu;
    static bool isStashMark(const Value& v) { return v.nestDepth == kStashMark; }

    // `cont[key] = newVal` on the container itself, no copy. checks the index before it changes anything
    void indexSetInPlace(Value& cont, const Value& key, Value&& newVal, int line) {
        if (cont.isArray()) {
            int ii = (int)key.asNumber();
            if (ii < 0 || ii >= (int)cont.asArray().size())
                vmError("array index " + std::to_string(ii) + " out of bounds", line);
            cont.arraySet((size_t)ii, std::move(newVal));
        } else if (cont.isMap()) {
            if (!key.isString())
                vmError("map key must be string", line);
            cont.mapSet(key.asString(), std::move(newVal));
        } else if (cont.isString()) {
            int ii = (int)key.asNumber();
            if (ii < 0 || ii >= (int)cont.asString().size())
                vmError("string index " + std::to_string(ii) + " out of bounds", line);
            cont.stringSetAt((size_t)ii, newVal.asString());
        } else {
            vmError("cannot index-assign on " + cont.typeName(), line);
        }
    }

    // finds a global through the chunk's lookup cache (see CompiledChunk::GlobalCache). same answer as interp_.find()
    Value* globalPtrOrNull(int nameIdx) {
        CompiledChunk::GlobalCache& gc = frames_.back().chunk->globalCacheFor(nameIdx);
        if (gc.epoch == interp_.scopeEpoch()) return gc.ptr;
        if (Value* g = interp_.findForCache(chunkStr(nameIdx), gc.typed)) {
            gc.ptr = g; gc.epoch = interp_.scopeEpoch();
            return g;
        }
        return nullptr;
    }
    Value* globalPtr(int nameIdx, int line) {
        if (Value* g = globalPtrOrNull(nameIdx)) return g;
        const std::string& nm = chunkStr(nameIdx);
        // same wording (and "did you mean") as resolveLoad() and the tree-walker
        std::string hint = suggest(nm);
        vmError("undefined variable: " + nm + (hint.empty() ? "" : "\n  did you mean '" + hint + "'?"), line);
    }
    void pushGlobal(int nameIdx, int line) { stack_.push_back(*globalPtr(nameIdx, line)); }

    // pops the top of the stack into a global, same as interp_.set() (a declared type is still checked)
    void storeGlobal(int nameIdx) {
        CompiledChunk::GlobalCache& gc = frames_.back().chunk->globalCacheFor(nameIdx);
        if (gc.epoch == interp_.scopeEpoch() && !gc.typed && !stack_.empty()) {
            *gc.ptr = std::move(stack_.back());
            stack_.pop_back();
            return;
        }
        Value v = pop();
        const std::string& nm = chunkStr(nameIdx);
        interp_.set(nm, std::move(v));   // checks the type. a brand new name also bumps the scope epoch
        bool typed = false;
        if (Value* g = interp_.findForCache(nm, typed)) { gc.ptr = g; gc.epoch = interp_.scopeEpoch(); gc.typed = typed; }
    }

    // true when a LOAD/STORE with no usable slot can only mean a global: the one and only frame, no captured layers
    bool onlyGlobalsLeft(const CallFrame& f) const { return frames_.size() == 1 && !f.locals && !f.captured; }

    // look up a name
    // innermost frame locals > captured frame > outer call frames > Interpreter global scope
    // knownSlot is the compile-time slot for the instruction running now, so it only applies to the
    // innermost frame (see CallFrame::findOwn)
    Value resolveLoad(const std::string& name, int line, int knownSlot = -2) {
        // walk frames from innermost outward
        bool first = true;
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if (Value* v = it->findOwn(name, first ? knownSlot : -2)) return *v;
            if (it->captured) {
                if (Value* v = it->captured->find(name)) return *v;
            }
            first = false;
        }
        // fall through to Interpreter global (read-only)
        if (const Value* g = interp_.find(name)) return *g;
        // not found suggest closest match
        std::string hint = suggest(name);
        std::string msg  = "undefined variable: " + name;
        if (!hint.empty()) msg += "\n  did you mean '" + hint + "'?";
        vmError(msg, line);
    }

    // like resolveLoad() but hands back the variable itself, or null if there is no such variable
    Value* findVar(const std::string& name, int knownSlot) {
        bool first = true;
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if (Value* v = it->findOwn(name, first ? knownSlot : -2)) return v;
            if (it->captured) {
                if (Value* v = it->captured->find(name)) return v;
            }
            first = false;
        }
        return interp_.findMut(name);
    }

    // store to nearest scope that already contains this name, else create in the innermost frame's locals (untyped)
    // knownSlot: see resolveLoad()'s own comment.
    void resolveStore(const std::string& name, Value v, int line, int knownSlot = -2) {
        bool first = true;
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if (Value* existing = it->findOwn(name, first ? knownSlot : -2)) {
                auto tyIt = it->localTypes.find(name);
                if (tyIt != it->localTypes.end() && !tyIt->second.isAny() && !tyIt->second.contains(v.tag()))
                    vmError("cannot assign " + v.typeName() + " to '" + name + "' declared as " +
                            tyIt->second.name(), line);
                *existing = std::move(v);
                return;
            }
            // writes straight into the shared captured layer, so every other closure sharing it
            // sees the change (see CaptureFrame in core/value.h)
            if (it->captured) {
                if (Value* slot = it->captured->find(name)) { *slot = std::move(v); return; }
            }
            first = false;
        }
        // not found in any frame
        // if it's an existing Interpreter global update it there instead of shadowing it with a new local
        if (interp_.has(name)) { interp_.set(name, std::move(v)); return; }
        // a name we've never seen. in the top-level frame it has to become a global, same as an assignment
        // at depth 0 (compiled to STORE_GLOBAL). otherwise a name first assigned inside an if/while at top
        // level becomes a frame-local that later splits from the global (this once made
        // `if 1 / i = 0 / end / i = 0 / while i < 3 / i += 1 / end` loop forever)
        // inside a function, the innermost frame's local is the right place
        if (frames_.size() == 1) { interp_.set(name, std::move(v)); return; }
        frames_.back().defineOwn(name, std::move(v), knownSlot);
    }

    // record that name was DEFINE'd while at least one SCOPE_PUSH is active in this frame so the matching SCOPE_POP knows to erase it
    // a name defined at frame scope depth isn't tracked here and correctly survives to the end of the frame
    void trackScopedDefine(CallFrame& f, const std::string& name) {
        if (!f.scopeNames.empty()) f.scopeNames.back().push_back(name);
    }

    // define (untyped) in the innermost frame. clears any type constraint set when the name was declared before
    void resolveDef(const std::string& name, Value v, int knownSlot = -2) {
        auto& f = frames_.back();
        f.defineOwn(name, std::move(v), knownSlot);
        f.localTypes.erase(name);
        trackScopedDefine(f, name);
    }

    void resolveDefTyped(const std::string& name, Value v, TypeSet ts, int line, int knownSlot = -2) {
        if (!ts.isAny() && !ts.contains(v.tag()))
            vmError("cannot assign " + v.typeName() + " to '" + name + "' declared as " + ts.name(), line);
        auto& f = frames_.back();
        f.defineOwn(name, std::move(v), knownSlot);
        if (!ts.isAny()) f.localTypes[name] = ts;
        else              f.localTypes.erase(name);
        trackScopedDefine(f, name);
    }

    void resolveDefAuto(const std::string& name, Value v, int knownSlot = -2) {
        auto& f = frames_.back();
        TypeSet ts(v.tag());
        f.defineOwn(name, std::move(v), knownSlot);
        f.localTypes[name] = ts;
        trackScopedDefine(f, name);
    }

    // builds the closure's CaptureFrame: the same live Scope objects visible right now (module scope if any, then each
    // call frame's captured layers and locals, outermost to innermost), not copies. sharing is what lets sibling
    // closures see each other's changes (see the wiki, scoping-and-closures)
    // - a frame's captured layers are included too, or a closure nested two levels deep couldn't see a variable its
    //   middle frame only has by capture
    // - frames still in fast-slot mode are promoted to a real Scope first (promoteToScope), because CaptureFrame
    //   holds Scopes, not slots
    std::shared_ptr<CaptureFrame> snapshotLocals() {
        auto cap = std::make_shared<CaptureFrame>();
        if (interp_.insideImportedModule())
            cap->layers.push_back(interp_.currentModuleScope());
        for (auto& f : frames_) {
            if (f.captured)
                for (auto& layer : f.captured->layers) cap->layers.push_back(layer);
            promoteToScope(f);
            cap->layers.push_back(f.locals);
        }
        return cap;
    }

    // turns frame f's fast-path slots into a real shared Scope (see CallFrame::locals for why). does
    // nothing if f is already promoted. the new Scope is added to f's gcRootsHandle
    void promoteToScope(CallFrame& f) {
        if (f.promoted()) return;
        f.locals = std::make_shared<Scope>();
        for (auto& [name, slot] : f.chunk->localSlots)
            if (slot < (int)f.slotDefined.size() && f.slotDefined[slot])
                (*f.locals)[name] = (*f.slots)[slot];
        interp_.gcRegisterScope(f.locals);
        assert(f.gcRootsHandle);
        f.gcRootsHandle->push_back(f.locals);
    }

    // dispatch loop

    Value dispatch() {
        while (state_ == State::Running && !frames_.empty()) {
            try {
                execOne();
            } catch (EmbrError& e) {
                if (!handleTry(e, 0)) throw;
            }
        }
        if (state_ == State::Done || frames_.empty()) {
            state_ = State::Done;
            return stack_.empty() ? Value(0.0) : pop();
        }
        return Value(0.0); // suspended
    }

    // find the nearest open try handler in frames_[floor..], innermost first. if there is one, unwind to it
    // and jump to its catch block. floor stops a nested dispatch loop from catching an error meant for the
    // caller's own try (the caller finds it when the exception propagates out). returns false if none is open
    bool handleTry(EmbrError& e, int floor) {
        // first work out which frame (if any) will catch this, so the trace records exactly the script
        // calls being unwound between the raise and the handler (or, with no handler in range, every
        // frame this loop owns), before the unwinding below pops them
        int handler = floor - 1;
        for (int fi = (int)frames_.size() - 1; fi >= floor; --fi)
            if (!frames_[fi].tryHandlers.empty()) { handler = fi; break; }
        appendTrace(e, handler);

        for (int fi = (int)frames_.size() - 1; fi >= floor; --fi) {
            if (frames_[fi].tryHandlers.empty()) continue;
            auto h = frames_[fi].tryHandlers.back();
            frames_[fi].tryHandlers.pop_back();

            // discard any frames pushed by calls that were still in progress when the error was raised
            while ((int)frames_.size() - 1 > fi) {
                interp_.gcReleaseVmFrame(frames_.back().gcRootsHandle, frames_.back().slots);
                frames_.pop_back();
                interp_.exitCall();
            }

            auto& f = frames_[fi];
            // unwind scopes the try block itself opened
            while (f.scopeDepth > h.scopeDepthAtEntry) {
                if (!f.scopeNames.empty()) {
                    for (auto& nm : f.scopeNames.back()) { f.eraseOwn(nm); f.localTypes.erase(nm); }
                    f.scopeNames.pop_back();
                }
                f.scopeDepth--;
            }
            while (stack_.size() > h.stackBase) stack_.pop_back();

            push(errorToMap(e)); // consumed by the catch block's own DEFINE
            f.ip = h.catchIp;
            injBuf_.reset();
            state_ = State::Running;
            return true;
        }
        return false;
    }

    // appends one TraceFrame per script function frame above `handlerFrame`, innermost first. the call
    // site of frames_[k] is the CALL instruction the caller frames_[k-1] just executed (its ip has
    // already advanced past it, hence ip - 1). top-level script code is not a call, and a frame entered
    // from native code has no script call site (line 0).
    void appendTrace(EmbrError& e, int handlerFrame) {
        for (int k = (int)frames_.size() - 1; k > handlerFrame; --k) {
            const CallFrame& fr = frames_[k];
            if (fr.isPersistentTop) continue;
            TraceFrame t;
            t.fn = fr.chunk ? fr.chunk->name : std::string();
            if (k > 0) {
                const CallFrame& caller = frames_[k - 1];
                if (caller.chunk) {
                    t.file = caller.chunk->sourceFile;
                    int ip = caller.ip - 1;
                    if (!fr.fromNative && ip >= 0 && ip < (int)caller.chunk->code.size())
                        t.line = caller.chunk->code[ip].line;
                }
            }
            e.trace.push_back(std::move(t));
        }
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
        case Op::LOAD: {
            // the common case: a variable of the running function that already has a slot. same answer as
            // resolveLoad(), which would find it first too, minus the frame walk and the extra Value moves
            CallFrame& f = frames_.back();
            if (!f.locals && instr.slot >= 0 && instr.slot < (int)f.slotDefined.size() && f.slotDefined[instr.slot]) {
                stack_.push_back((*f.slots)[instr.slot]);
                break;
            }
            if (onlyGlobalsLeft(f)) { pushGlobal(instr.operand, line); break; }
            push(resolveLoad(chunkStr(instr.operand), line, instr.slot));
            break;
        }

        case Op::STORE: {
            // same shortcut as LOAD. a declared type (localTypes) needs the checked path in resolveStore()
            CallFrame& f = frames_.back();
            if (!f.locals && instr.slot >= 0 && instr.slot < (int)f.slotDefined.size() && f.slotDefined[instr.slot] &&
                f.localTypes.empty() && !stack_.empty()) {
                (*f.slots)[instr.slot] = std::move(stack_.back());
                stack_.pop_back();
                break;
            }
            // not a local yet: with only the top frame running, resolveStore() would end up in interp_.set() anyway
            if (onlyGlobalsLeft(f) && f.localTypes.empty()) { storeGlobal(instr.operand); break; }
            resolveStore(chunkStr(instr.operand), pop(), line, instr.slot);
            break;
        }

        case Op::DEFINE:
            resolveDef(chunkStr(instr.operand), pop(), instr.slot);
            break;

        case Op::DEFINE_TYPED:
            resolveDefTyped(chunkStr(instr.operand), pop(), instr.typeHint, line, instr.slot);
            break;

        case Op::DEFINE_AUTO:
            resolveDefAuto(chunkStr(instr.operand), pop(), instr.slot);
            break;

        case Op::LOAD_GLOBAL:
            pushGlobal(instr.operand, line);
            break;

        case Op::STORE_GLOBAL:
            storeGlobal(instr.operand);
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

        case Op::APPEND_VAR: {
            Value* target = findVar(chunkStr(instr.operand), instr.slot);
            if (target && target->isString() && !stack_.empty()) {
                Value r = pop();
                std::get<std::string>(target->data) += r.isString() ? std::get<std::string>(r.data) : r.formatAsString();
                frames_.back().ip = (int)instr.intVal;
                if (injBuf_) injBuf_.reset();
            }
            break;
        }

        case Op::MARK_MODULE_LOCAL:
            interp_.markModuleLocal(chunkStr(instr.operand));
            break;

        // arithmetic
        // Int stays exact for +,-,*,%. mixed or float widens to double. division always gives a float
        // a pointer operand whose tag has a matching __tag_<op> dunder (add/sub/mul/div/mod/gt/lt/gte/lte)
        // calls it instead of raising, left operand first then right (see resolveDunder() in core/registry.h)
        case Op::ADD: {
            // two ints (the hot case) are added in place on the stack, no pops and pushes
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = intAdd(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
            }
            Value r = pop(), l = pop();
            if (l.isString()) {
                // l is our own copy of the variable, so grow it instead of building a third string
                std::string& ls = std::get<std::string>(l.data);
                if (r.isString()) ls += std::get<std::string>(r.data); else ls += r.formatAsString();
                push(std::move(l));
                break;
            }
            if (r.isString())                        { push(Value(l.formatAsString() + std::get<std::string>(r.data))); break; }
            if (l.isInt() && r.isInt())               { push(intAdd(l.asInt(), r.asInt())); break; }
            if (l.isNumeric() && r.isNumeric())       { push(Value(l.asNumber() + r.asNumber())); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "add", dfn) || resolveDunder(interp_, r, "add", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '+': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::SUB: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = intSub(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
            }
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt())         { push(intSub(l.asInt(), r.asInt())); break; }
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.asNumber() - r.asNumber())); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "sub", dfn) || resolveDunder(interp_, r, "sub", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '-': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::MUL: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = intMul(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
            }
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt())         { push(intMul(l.asInt(), r.asInt())); break; }
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.asNumber() * r.asNumber())); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "mul", dfn) || resolveDunder(interp_, r, "mul", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '*': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::DIV: {
            Value r = pop(), l = pop();
            if (l.isNumeric() && r.isNumeric()) {
                double rd = r.asNumber();
                if (rd == 0.0) vmError("division by zero", line);
                push(Value(l.asNumber() / rd));
                break;
            }
            Value dfn;
            if (resolveDunder(interp_, l, "div", dfn) || resolveDunder(interp_, r, "div", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '/': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::MOD: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt() && rr.asInt() != 0) { ll = intMod(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
            }
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt()) {
                int64_t rv = r.asInt();
                if (rv == 0) vmError("modulo by zero", line);
                push(intMod(l.asInt(), rv));
                break;
            }
            if (l.isNumeric() && r.isNumeric()) {
                double rd = r.asNumber();
                if (rd == 0.0) vmError("modulo by zero", line);
                push(Value(std::fmod(l.asNumber(), rd)));
                break;
            }
            Value dfn;
            if (resolveDunder(interp_, l, "mod", dfn) || resolveDunder(interp_, r, "mod", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '%': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::NEG: {
            Value v = pop();
            if (v.isNumeric()) { push(v.isInt() ? intNeg(v.asInt()) : Value(-v.asNumber())); break; }
            // dunder fallback, see tree_walker.h's evalUnary, which does
            // the same thing (and its comment on why '!'/Op::NOT has none).
            Value dfn;
            if (resolveDunder(interp_, v, "neg", dfn)) { push(invoke(dfn, {v})); break; }
            vmError("unsupported operand for unary '-': " + v.typeName(), line);
            break;
        }
        case Op::NOT: { Value v = pop(); push(Value(!v.truthy())); break; }

        // comparison. "==" and "!=" are never dunder-overridable (see arithDunderOp())
        case Op::EQ:  { Value r=pop(),l=pop(); push(Value(l==r)); break; }
        case Op::NEQ: { Value r=pop(),l=pop(); push(Value(l!=r)); break; }
        case Op::LT: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() < rr.asInt()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()<r.asInt()) : (l.asNumber()<r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "lt", dfn) || resolveDunder(interp_, r, "lt", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '<': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::GT: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() > rr.asInt()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()>r.asInt()) : (l.asNumber()>r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "gt", dfn) || resolveDunder(interp_, r, "gt", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '>': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::LTE: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() <= rr.asInt()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()<=r.asInt()) : (l.asNumber()<=r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "lte", dfn) || resolveDunder(interp_, r, "lte", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '<=': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::GTE: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() >= rr.asInt()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()>=r.asInt()) : (l.asNumber()>=r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "gte", dfn) || resolveDunder(interp_, r, "gte", dfn)) { push(invoke(dfn, {l, r})); break; }
            vmError("unsupported operands for '>=': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }

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
                    f.eraseOwn(name);
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
            indexGet(cont, key, line);
            break;
        }

        case Op::INDEX_GET_VAR: {
            Value key = pop();
            // the variable itself, not a copy. indexGet() runs no user code, so nothing can change it meanwhile
            if (instr.flags & kStash) {
                Value stash = pop();
                if (!isStashMark(stash)) { indexGet(stash, key, line); break; }   // a copy taken before the key ran
            }
            const Value* cont = varForOp(instr);
            if (!cont) { Value v = resolveLoad(chunkStr(instr.operand), line, instr.slot); indexGet(v, key, line); break; }
            indexGet(*cont, key, line);
            break;
        }

        case Op::STASH_VAR: {
            Value* v = varForOp(instr);
            if (!v) { push(resolveLoad(chunkStr(instr.operand), line, instr.slot)); break; }   // raises undefined variable
            if ((instr.flags & kCallFree) || slotIsOwnLocal(frames_.back(), instr.slot) ||
                ((instr.flags & kPureCalls) && pureCalleesNative(instr))) {
                Value mark; mark.nestDepth = kStashMark;
                push(std::move(mark));
            } else {
                push(*v);
            }
            break;
        }

        case Op::REF_PATH: {
            RefPath path;
            path.nameIdx = instr.operand;
            path.slot    = instr.slot;
            path.name    = chunkStr(instr.operand);
            path.keys.resize((size_t)instr.intVal);
            for (size_t i = path.keys.size(); i-- > 0; ) path.keys[i] = pop();
            push(makeRefPath(std::move(path)));
            break;
        }

        case Op::INDEX_SET: {
            // stack: [container, key, new_value] (new_value on top)
            Value newVal = pop();
            Value key    = pop();
            Value cont   = pop();
            indexSetInPlace(cont, key, std::move(newVal), line);
            push(std::move(cont));
            break;
        }

        case Op::INDEX_SET_VAR: {
            // stack: [stash?, key, new_value]
            Value newVal = pop();
            Value key    = pop();
            Value stash;
            bool  copy = false;
            if (instr.flags & kStash) { stash = pop(); copy = !isStashMark(stash); }
            if (copy) {
                // the variable may have changed while the key and value ran, so work on the copy and let the STORE write it back
                indexSetInPlace(stash, key, std::move(newVal), line);
                push(std::move(stash));
                break;
            }
            Value* target = varForOp(instr);
            if (!target) { resolveLoad(chunkStr(instr.operand), line, instr.slot); break; }   // raises undefined variable
            indexSetInPlace(*target, key, std::move(newVal), line);
            frames_.back().ip = (int)instr.intVal;   // skip the STORE
            if (injBuf_) injBuf_.reset();
            break;
        }

        case Op::APPEND_ARR: {
            if (stack_.size() < 3) break;
            Value& callee = stack_[stack_.size() - 3];
            Value& stash  = stack_[stack_.size() - 2];
            bool   fresh  = isStashMark(stash);
            Value* target = fresh ? varForOp(instr) : nullptr;
            if (fresh && !target) { resolveLoad(chunkStr(instr.operand), line, instr.slot); break; }   // raises undefined variable
            if (target && target->isArray() && callee.isCallable() && callee.asCallable().isNative() &&
                callee.asCallable().name() == "push") {
                Value x = pop(); pop(); pop();
                target->arrayAppend(std::move(x));
                frames_.back().ip = (int)instr.intVal;   // skip the CALL and the STORE
                if (injBuf_) injBuf_.reset();
                break;
            }
            if (fresh) stash = *target;   // the ordinary call below needs the real array
            break;
        }

        // functions
        case Op::MAKE_CLOSURE: {
            int fnIdx = instr.operand;
            // looked up in the sibling list of the chunk running right now, not prog_: a chunk that escaped from
            // another program (say a module's exported function called later) runs under a different prog_
            // see CompiledChunk::siblings
            const auto& siblings = *frames_.back().chunk->siblings;
            if (fnIdx < 0 || fnIdx >= (int)siblings.size())
                vmError("invalid function index " + std::to_string(fnIdx), line);
            auto chunk = siblings[fnIdx];
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
                if (!pathVal.isString())
                    vmError("import path must evaluate to a string, got " + pathVal.typeName(), line);
                execImport(pathVal.asString(), line);
                push(Value(0.0));
                break;
            }

            // normal call stack is [callee, arg0, arg1, ..., argN]
            // args are on TOS, callee below them
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();
            Value callee = pop();

            if (!callee.isCallable()) {
                // dunder fallback, see core/registry.h's resolveDunder().
                Value dfn;
                if (resolveDunder(interp_, callee, "call", dfn)) {
                    args.insert(args.begin(), callee);
                    callee = dfn;
                } else {
                    vmError("value is not callable (got " + callee.typeName() + ")", line);
                }
            }

            const auto& c = callee.asCallable();
            const bool hasRef = (instr.flags & kHasRef) != 0;
            if (hasRef) resolveRefArgs(c, args, line);
            if (c.isNative()) {
                checkNativeSig(c.name(), c.sig, args, {line, 0, line, 0});
                try {
                    Value result = c.native(args);
                    if (hasRef) {
                        settleRefs(args);
                        if (isRef(result)) vmError("'" + c.name() + "' returned a reference. natives must not hand one back", line);
                    }
                    push(std::move(result));
                } catch (EmbrError& e) {
                    if (hasRef) settleRefs(args);
                    // same as invoke()'s re-raise below: add this call's line and the native's name only if the native's
                    // own error didn't already carry a location. otherwise rethrow it as it was, so its kind ("assert",
                    // "error", a plugin's own tag, ...) reaches catch intact
                    if (!e.hasLocation) {
                        try { raiseError(c.name(), e.message, {line, 0, line, 0}, interp_.sourceMap()); }
                        catch (EmbrError& located) { located.trace = std::move(e.trace); throw; }
                    }
                    throw;
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

        frame.slots->assign(chunk->numLocalSlots, Value());
        frame.slotDefined.assign(chunk->numLocalSlots, false);
        // every param got a slot up front in Compiler::compileFunction, so
        // slotOf() here can never miss
        for (size_t i = 0; i < fixedCount; ++i) {
            int slot = chunk->slotOf(chunk->params[i].name);
            assert(slot >= 0);
            (*frame.slots)[slot] = args[i];
            frame.slotDefined[slot] = true;
        }
        if (hasVariadic) {
            int slot = chunk->slotOf(chunk->params.back().name);
            assert(slot >= 0);
            Value::array_type rest(args.begin() + fixedCount, args.end());
            (*frame.slots)[slot] = Value(std::move(rest));
            frame.slotDefined[slot] = true;
        }

        // gc: register this frame's slots as a value root, and record what its captured layers add to the
        // Scope roots. its own locals join that list later if the frame gets promoted (see promoteToScope())
        // done before frames_ takes ownership of the frame
        interp_.gcPushVmFrameValueRoots(frame.slots);
        {
            std::vector<std::shared_ptr<Scope>> roots;
            if (frame.captured)
                for (auto& layer : frame.captured->layers) roots.push_back(layer);
            frame.gcRootsHandle = interp_.gcPushVmFrameRoots(std::move(roots));
        }

        // recursion guard. entered only once the frame is really about to be pushed, and paired with the
        // exitCall() in doReturn() where this frame is popped. a scope-based guard wouldn't work here
        // because the frame outlives pushFrame() for a plain in-VM CALL
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
        interp_.gcReleaseVmFrame(leaving.gcRootsHandle, leaving.slots);
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

    // import: dispatches on the extension like the tree-walker's Runner::execImport (see tree_walker.h). a ".embr"
    // path is a script module, anything else is a native plugin
    void execImport(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;
        if (fs::path(rawPath).extension() == ".embr") execImportEmbrModule(rawPath, line);
        else                                          execImportPlugin(rawPath, line);
    }

    // import a native plugin (.so / .dll / .dylib)
    void execImportPlugin(const std::string& rawPath, int line) {
        // see tree_walker.h: one shared, policy-checking loader for both backends
        std::string err = importNativePlugin(interp_, rawPath);
        if (!err.empty()) vmError(err, line);
    }

    // import an embr script as a module. it gets a fresh VmRunner, because running it on this one would
    // clobber the frame that is executing now. like the tree-walker's execImportEmbrModule, it merges
    // into the importer's scope instead of returning a map the way load_module() does (see exportModuleScope in module.h)
    void execImportEmbrModule(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;

        auto cands = fileCandidates(rawPath, interp_.scriptDir, ".embr", "modules");
        fs::path resolved = findExistingCandidate(cands);
        if (resolved.empty())
            vmError("cannot find module \"" + rawPath + "\"\n  tried:" + describeCandidates(cands), line);

        const std::string canonStr = canonicalOrSelf(resolved);
        if (interp_.loadedModules_.count(canonStr)) return;
        interp_.loadedModules_.insert(canonStr);

        auto src = tryReadFile(resolved);
        if (!src) vmError("cannot open module file: " + resolved.string(), line);

        std::string savedDir  = interp_.scriptDir;
        SourceMap   savedMap  = interp_.sourceMap();
        std::string savedFile = interp_.currentFile();
        interp_.scriptDir = resolved.parent_path().string();

        interp_.pushModuleScope();
        try {
            Lexer     lex(*src);
            auto      tokens = lex.tokenize();
            SourceMap sm     = lex.sourceMap();
            Parser    parser(std::move(tokens), sm);
            auto      prog   = parser.parse();

            Compiler        compiler(&interp_);
            CompiledProgram compiled = compiler.compile(prog, resolved.string());
            registerChunkTeardown(interp_, compiled);

            interp_.setSource(sm, resolved.string());
            VmRunner moduleRunner(interp_);
            moduleRunner.run(compiled);
        } catch (...) {
            interp_.popModuleScope();  // discard result on error
            interp_.scriptDir = savedDir;
            interp_.setSource(savedMap, savedFile);
            throw;
        }

        auto res = interp_.popModuleScope();
        interp_.scriptDir = savedDir;
        interp_.setSource(savedMap, savedFile);

        exportModuleScope(interp_, std::move(res), /*mutateCallerScope=*/true);
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
        frames_.back().fromNative = true;

        // RAII: if anything escapes this call (an uncaught EmbrError or any other exception),
        // pop every frame pushed since targetDepth so their gc roots and call-depth entries are released
        // dismissed on normal exit, where doReturn()/handleTry() already did this bookkeeping
        struct UnwindGuard {
            VmRunner& vm; int depth; bool armed = true;
            ~UnwindGuard() {
                if (!armed) return;
                while ((int)vm.frames_.size() > depth) {
                    vm.interp_.gcReleaseVmFrame(vm.frames_.back().gcRootsHandle, vm.frames_.back().slots);
                    vm.frames_.pop_back();
                    vm.interp_.exitCall();
                }
            }
        } guard{*this, targetDepth};

        State savedState = state_;
        state_ = State::Running;
        while ((int)frames_.size() > targetDepth && state_ == State::Running) {
            try {
                execOne();
            } catch (EmbrError& e) {
                // a handler at or below targetDepth belongs to whatever outer call pushed frames up to targetDepth not to this invocation
                // leave it for that caller's own loop and let the exception propagate out of this call instead
                if (!handleTry(e, targetDepth)) throw;
            }
        }

        guard.armed = false;
        Value result = stack_.empty() ? Value(0.0) : pop();
        state_ = savedState;
        return result;
    }

    // turns the reference paths among a call's arguments into references. done after every argument is in, so
    // evaluating a later argument can't move the container a reference points into
    void resolveRefArgs(const Value::Callable& c, std::vector<Value>& args, int line) {
        if (!c.isNative() || c.sig.empty())
            vmError("'" + c.name() + "' can't take '&': only natives with an in-out parameter do", line);
        for (auto& a : args) {
            if (!isRefPath(a)) continue;
            RefPath& p = refPathOf(a);
            Value* root = varBySlot(p.nameIdx, p.slot);
            if (!root) { resolveLoad(p.name, line, p.slot); vmError("undefined variable: " + p.name, line); }   // resolveLoad raises
            std::shared_ptr<RefChain> chain;
            std::string err;
            Value* target = resolveRefPath(root, p, chain, err);
            if (!target) vmError("&" + p.name + ": " + err, line);
            a = makeRef(target, std::move(chain));
        }
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
            const bool ref = isRef(args[i]);
            if (p && p->inout != ref)
                vmError(ref ? "argument '" + p->name + "' to '" + fname + "' is not an in-out parameter, so it can't take '&'"
                            : "argument '" + p->name + "' to '" + fname + "' is changed in place, so pass it as &name",
                        site.startLine);
            if (!p && ref) vmError("'" + fname + "' can't take '&' for argument " + std::to_string(i + 1), site.startLine);
            if (!p || p->type.isAny()) continue;
            const Value& shown = ref ? refTarget(args[i]) : args[i];
            if (!p->type.contains(shown.tag()))
                vmError("argument '" + p->name + "' to '" + fname + "': expected " +
                        p->type.name() + " but got " + shown.typeName(), site.startLine);
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
        throw EmbrError(out.str(), line > 0, {line, 0, line, 0}, msg, "vm");
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
            if (f.locals) {
                for (auto& [k, _] : *f.locals) {
                    size_t d = editDist(name, k, bestD);
                    if (d < bestD) { bestD = d; best = k; }
                }
            } else {
                for (auto& [k, slot] : f.chunk->localSlots) {
                    if (slot >= (int)f.slotDefined.size() || !f.slotDefined[slot]) continue;
                    size_t d = editDist(name, k, bestD);
                    if (d < bestD) { bestD = d; best = k; }
                }
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
    registerChunkTeardown(interp, prog);
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
        std::cerr << e.what() << formatTrace(e);
    }
}

} // namespace vm
} // namespace embr

#endif // EMBR_WITH_VM
#endif // EMBR_BACKENDS_VM_H
