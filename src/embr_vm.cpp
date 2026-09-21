// compiler and VM for embr
//
// usage
//
//   #define EMBR_NO_MAIN
//   #include "embr_vm.cpp"
//
//   embr::Interpreter interp;
//   embr::vm::runSource(src, interp, "<input>");
//
// existing tree-walking Runner is untouched
// compileSource() + VmRunner::run() replace runSource() for the VM path
//
//  Compiler is a one-pass AST walker that emits a flat Instruction stream
//  each compiled function (including top-level) lives in a CompiledChunk
//  VmRunner owns its own scope stack. the Interpreter holds only the global scope (index 0 of its existing scopes_) which is read-only at runtime
//  natives receive a VmRunner& so they can invoke script callbacks without constructing a separate runner
//  YIELD suspends the runner. the caller may resume it by calling step() repeatedly or run() again after injecting instructions
//  inject() prepends raw instructions into a mutable override buffer so plugins can splice in loops, async continuations and shi

#ifndef EMBR_VM_CPP_INCLUDED
#define EMBR_VM_CPP_INCLUDED

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <sstream>

// WARNING! dented bullshit for functions to store compiled code
namespace embr { namespace vm { struct CompiledChunk; } }
#define EMBR_CHUNK_TYPE embr::vm::CompiledChunk
#include "embr.cpp"

namespace embr {
namespace vm {


// instruction set

enum class Op : uint8_t {
    // literals
    PUSH_NUM,       // numVal  = the double
    PUSH_STR,       // operand = string-table index
    PUSH_TRUE,      // push 1.0
    PUSH_FALSE,     // push 0.0
    PUSH_NIL,       // push Value()

    // stack
    POP,            // discard top

    // variables
    LOAD,           // operand = string-table index  ->  push value
    STORE,          // operand = string-table index  <-  pop value, assign-or-create in nearest scope
    DEFINE,         // operand = string-table index  <-  pop value, define in current (innermost) scope
    LOAD_GLOBAL,    // operand = string-table index  ->  read from Interpreter global scope
    STORE_GLOBAL,   // operand = string-table index  <-  write to Interpreter global scope (via queue in future)

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
    JUMP_IF_TRUE,   // operand = absolute instruction index; pops condition

    // jump but leave value on stack
    JUMP_IF_FALSE_PEEK,
    JUMP_IF_TRUE_PEEK,

    // containers
    BUILD_ARRAY,    // operand = element count; pops N, pushes array
    BUILD_MAP,      // operand = entry count;   pops 2N (key,val pairs), pushes map
    INDEX_GET,      // pops container + key, pushes element
    INDEX_SET,      // pops container + key + value, pushes updated container, then STORE follows

    // callables
    MAKE_CLOSURE,   // operand = function-table index; snapshots current locals -> pushes callable
    CALL,           // operand = arg count; stack: [callee, arg0..argN]
    RETURN,         // pops return value, unwinds frame

    // async / plugin injection
    YIELD,          // suspend this runner; return control to caller of step()
    NOP,            // no-op; useful as a patch target

    // scope management
    SCOPE_PUSH,
    SCOPE_POP,
};

struct Instruction {
    Op     op      = Op::NOP;
    int    operand = 0;      // string-table index, jump target, count, fn index
    double numVal  = 0.0;    // payload for PUSH_NUM
    int    line    = 0;      // for error reporting
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
    // the Interpreter is consulted only to know which names are already bound (globals/natives) so can emit LOAD_GLOBAL vs LOAD
    // pass nullptr to skip that optimisation
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

    // emit helpers
    void emit(Op op, int operand = 0, double num = 0.0, int line = 0) {
        cur_->code.push_back({op, operand, num, line});
    }

    // returns the index of the emitted instruction for later back-patching
    int emitJump(Op op, int line = 0) {
        cur_->code.push_back({op, -1, 0.0, line});
        return (int)cur_->code.size() - 1;
    }

    void patchJump(int idx) {
        assert(idx >= 0 && idx < (int)cur_->code.size());
        cur_->code[idx].operand = (int)cur_->code.size();
    }

    int here() const { return (int)cur_->code.size(); }

    // expression compilation
    void compileExpr(Expr* e) {
        switch (e->kind) {

        case Expr::Kind::Number:
            emit(Op::PUSH_NUM, 0, static_cast<NumberExpr*>(e)->v, e->range.startLine);
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

        // short-circuit AND. if left is falsy push left and skip right
        case Expr::Kind::And: {
            auto* log = static_cast<LogicExpr*>(e);
            compileExpr(log->left.get());
            int skipRight = emitJump(Op::JUMP_IF_FALSE_PEEK, e->range.startLine);
            emit(Op::POP);                       // discard left; we want right's value
            compileExpr(log->right.get());
            patchJump(skipRight);
            break;
        }

        // short-circuit OR. if left is truthy push left and skip right
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
            emitLoad(c->name, e->range.startLine);
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
            compileExpr(l->value.get());
            emit(Op::DEFINE, cur_->intern(l->name), 0.0, s->range.startLine);
            break;
        }

        case Stmt::Kind::Assign:
            compileAssign(static_cast<AssignStmt*>(s));
            break;

        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            compileExpr(i->cond.get());
            int toElse = emitJump(Op::JUMP_IF_FALSE, s->range.startLine);

            emit(Op::SCOPE_PUSH);
            for (auto& st : i->thenBlock) compileStmt(st.get());
            emit(Op::SCOPE_POP);

            if (!i->elseBlock.empty()) {
                int toEnd = emitJump(Op::JUMP, s->range.startLine);
                patchJump(toElse);
                emit(Op::SCOPE_PUSH);
                for (auto& st : i->elseBlock) compileStmt(st.get());
                emit(Op::SCOPE_POP);
                patchJump(toEnd);
            } else {
                patchJump(toElse);
            }
            break;
        }

        case Stmt::Kind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            int loopTop = here();
            compileExpr(w->cond.get());
            int toEnd = emitJump(Op::JUMP_IF_FALSE, s->range.startLine);

            emit(Op::SCOPE_PUSH);
            for (auto& st : w->body) compileStmt(st.get());
            emit(Op::SCOPE_POP);

            emit(Op::JUMP, loopTop, 0.0, s->range.startLine);
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
            emit(Op::DEFINE, cur_->intern(f->name), 0.0, s->range.startLine);
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
            // emit a special call to the built-in __import__ helper which the VM dispatches to execImport.
            // the string operand is the path
            auto* im = static_cast<ImportStmt*>(s);
            emit(Op::PUSH_STR, cur_->intern(im->path), 0.0, s->range.startLine);
            // operand 1 = sentinel value so the VM knows this CALL is an import
            emit(Op::CALL, -1, 0.0, s->range.startLine);  // -1 = import opcode
            break;
        }

        }
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

    // for a[k] = v need: eval container, eval key, eval value, INDEX_SET,
    // then write the updated container back to wherever a lives
    // for nested cases like a[i][j] = v recurse
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
            // nested: a[i][j] = v  ->  after inner INDEX_SET we need to update a[i]
            auto* idx = static_cast<IndexExpr*>(target);
            // re-evaluate container and key, then INDEX_SET again, then recurse
            compileExpr(idx->object.get());  // container (again)
            compileExpr(idx->index.get());   // key (again)
            // store in a hidden temp, re-eval, reload
            // synthesise a unique temp name using the instruction counter
            std::string tmp = "__wb_tmp_" + std::to_string(here());
            emit(Op::DEFINE, cur_->intern(tmp), 0.0, line);  // store updated sub-container
            compileExpr(idx->object.get());
            compileExpr(idx->index.get());
            emit(Op::LOAD, cur_->intern(tmp), 0.0, line);    // reload updated sub-container
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
        CompiledChunk* saved = cur_;
        cur_ = chunk.get();

        for (auto& s : body) compileStmt(s.get());

        // implicit return 0 at end of function if no explicit return reached
        emit(Op::PUSH_NIL);
        emit(Op::RETURN);

        cur_ = saved;
        return idx;
    }

    // variable load/store helpers 

    // LOAD for everything and let the VM scope-search fall through to the interpreter global scope
    // LOAD_GLOBAL is an explicit override for the future async design where global reads are queued
    void emitLoad(const std::string& name, int line) {
        emit(Op::LOAD, cur_->intern(name), 0.0, line);
    }

    void emitStore(const std::string& name, int line) {
        emit(Op::STORE, cur_->intern(name), 0.0, line);
    }
};


// one activation record on the call stack
struct CallFrame {
    std::shared_ptr<CompiledChunk>  chunk;          // the function being executed
    int                             ip = 0;         // instruction pointer into chunk->code
    int                             stackBase;      // index of this frame's first local on the value stack
    std::unordered_map<std::string, Value> locals;  // this frame's scope layer
    std::shared_ptr<CaptureFrame>   captured;       // closure env may be null
    TypeSet                         returnType = TypeSet::Any();
    std::string                     fnName;
};

class VmRunner {
public:
    enum class State { Ready, Running, Suspended, Done, Error };

    explicit VmRunner(Interpreter& interp) : interp_(interp) {}

    // run a compiled program to completion or until YIELD
    // returns the final value left on the stack. usually 0.0 for a script
    Value run(const CompiledProgram& prog) {
        prog_ = &prog;
        if (frames_.empty()) {
            // first call. set up top-level frame
            auto frame = CallFrame{};
            frame.chunk      = prog.chunks[0];
            frame.ip         = 0;
            frame.stackBase  = 0;
            frame.fnName     = "<top>";
            frames_.push_back(std::move(frame));
            state_ = State::Running;
        }
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

    // invoke a callable (script or native) from C++. used by native functions like for_each_do, sort_do so they operate on this runner's scope
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
                    raiseError(c.name(), e.what(), site, interp_.sourceMap());
                throw;
            }
        }

        // script call via compiled chunk
        const ScriptFn& sf = c.script;
        if (sf.compiledChunk) {
            return invokeChunk(sf.compiledChunk, sf.captured, args, site,
                               sf.returnType, sf.name);
        }

        // fallback. the callee was created by the tree-walking compiler (no compiled chunk).
        // run it via the old runner
        Runner treeRunner(interp_);
        return treeRunner.invoke(callee, args, site);
    }

    // inject instructions to be executed before the current instruction pointer resumes.
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
    Value& peekAt(int n)  { return stack_[stack_.size() - 1 - n]; } // 0 = TOS

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

    // store to nearest scope that already contains this name, else to the innermost frame's locals (create in innermost scope)
    void resolveStore(const std::string& name, Value v) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if (it->locals.count(name)) { it->locals[name] = std::move(v); return; }
            if (it->captured && it->captured->count(name)) {
                (*it->captured)[name] = std::move(v); return;
            }
        }
        // not found anywhere. create in innermost frame
        frames_.back().locals[name] = std::move(v);
    }

    // define in the innermost frame (local)
    void resolveDef(const std::string& name, Value v) {
        frames_.back().locals[name] = std::move(v);
    }

    // snapshot all locals across the current call stack into a CaptureFrame
    std::shared_ptr<CaptureFrame> snapshotLocals() const {
        auto cap = std::make_shared<CaptureFrame>();
        for (auto& f : frames_)
            for (auto& [k, v] : f.locals)
                (*cap)[k] = v;
        return cap;
    }

    // dispatch loop 

    Value dispatch() {
        while (state_ == State::Running && !frames_.empty()) {
            execOne();
        }
        if (state_ == State::Done || frames_.empty()) {
            state_ = State::Done;
            return stack_.empty() ? Value(0.0) : pop();
        }
        return Value(0.0); // suspended
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
        case Op::PUSH_STR:   push(Value(chunkStr(instr.operand))); break;
        case Op::PUSH_TRUE:  push(Value(1.0)); break;
        case Op::PUSH_FALSE: push(Value(0.0)); break;
        case Op::PUSH_NIL:   push(Value(0.0)); break;

        case Op::POP: pop(); break;
        case Op::NOP: break;

        // variables 
        case Op::LOAD:
            push(resolveLoad(chunkStr(instr.operand), line));
            break;

        case Op::STORE:
            resolveStore(chunkStr(instr.operand), pop());
            break;

        case Op::DEFINE:
            resolveDef(chunkStr(instr.operand), pop());
            break;

        case Op::LOAD_GLOBAL:
            if (!interp_.has(chunkStr(instr.operand)))
                vmError("undefined global: " + chunkStr(instr.operand), line);
            push(interp_.get(chunkStr(instr.operand)));
            break;

        case Op::STORE_GLOBAL:
            interp_.set(chunkStr(instr.operand), pop());
            break;

        // arithmetic 
        case Op::ADD: {
            Value r = pop(), l = pop();
            if (l.isString() || r.isString())
                push(Value(l.formatAsString() + r.formatAsString()));
            else if (l.isNumber() && r.isNumber())
                push(Value(l.asNumber() + r.asNumber()));
            else
                vmError("unsupported operands for '+': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::SUB: { Value r=pop(),l=pop(); push(Value(l.asNumber()-r.asNumber())); break; }
        case Op::MUL: { Value r=pop(),l=pop(); push(Value(l.asNumber()*r.asNumber())); break; }
        case Op::DIV: {
            Value r=pop(),l=pop();
            if (r.asNumber()==0.0) vmError("division by zero", line);
            push(Value(l.asNumber()/r.asNumber())); break;
        }
        case Op::MOD: {
            Value r=pop(),l=pop();
            if (r.asNumber()==0.0) vmError("modulo by zero", line);
            push(Value(std::fmod(l.asNumber(),r.asNumber()))); break;
        }
        case Op::NEG: { Value v=pop(); push(Value(-v.asNumber())); break; }
        case Op::NOT: { Value v=pop(); push(Value(!v.truthy())); break; }

        // comparison 
        case Op::EQ:  { Value r=pop(),l=pop(); push(Value(l==r));                       break; }
        case Op::NEQ: { Value r=pop(),l=pop(); push(Value(l!=r));                       break; }
        case Op::LT:  { Value r=pop(),l=pop(); push(Value(l.asNumber()<r.asNumber()));  break; }
        case Op::GT:  { Value r=pop(),l=pop(); push(Value(l.asNumber()>r.asNumber()));  break; }
        case Op::LTE: { Value r=pop(),l=pop(); push(Value(l.asNumber()<=r.asNumber())); break; }
        case Op::GTE: { Value r=pop(),l=pop(); push(Value(l.asNumber()>=r.asNumber())); break; }

        // control flow 
        case Op::JUMP:
            frames_.back().ip = instr.operand;
            if (injBuf_) { injBuf_.reset(); } // jump clears injection buffer
            break;

        case Op::JUMP_IF_FALSE:
            if (!pop().truthy()) {
                frames_.back().ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        case Op::JUMP_IF_TRUE:
            if (pop().truthy()) {
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
            CallFrame inner{};
            inner.chunk     = frames_.back().chunk;
            inner.ip        = frames_.back().ip;  // will be immediately overwritten
            inner.stackBase = (int)stack_.size();
            inner.fnName    = frames_.back().fnName + "#scope";
            // bump a scope-depth counter and remember the set of names introduced in this scope
            scopeDepth_++;
            scopeNames_.emplace_back(); // new set for this depth
            break;
        }

        case Op::SCOPE_POP: {
            // remove all locals introduced at the current depth
            if (scopeDepth_ > 0 && !scopeNames_.empty()) {
                for (auto& name : scopeNames_.back())
                    frames_.back().locals.erase(name);
                scopeNames_.pop_back();
                scopeDepth_--;
            }
            break;
        }

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
            // stack: [container, key, new_value]   (new_value on top)
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
                    vmError(e.what(), line);
                }
                break;
            }

            // script call
            const ScriptFn& sf = c.script;
            if (sf.compiledChunk) {
                // push a new call frame and continue the dispatch loop
                pushFrame(sf.compiledChunk, sf.captured, args, sf.returnType, sf.name);
            } else {
                // callee was compiled by the tree-walker fallback to old runner
                Runner treeRunner(interp_);
                push(treeRunner.invoke(callee, args));
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
        // typecheck args
        for (size_t i = 0; i < chunk->params.size() && i < args.size(); ++i) {
            const auto& p = chunk->params[i];
            if (!p.type.isAny() && !p.type.contains(args[i].tag()))
                vmError("argument '" + p.name + "' to '" + fnName + "': expected " +
                        p.type.name() + " but got " + args[i].typeName(), 0);
        }
        if (args.size() < chunk->params.size())
            vmError("'" + fnName + "' expects " + std::to_string(chunk->params.size()) +
                    " args, got " + std::to_string(args.size()), 0);

        // save current ip in the caller's frame
        // ip was already advanced past the CALL instruction by fetch()

        CallFrame frame{};
        frame.chunk      = chunk;
        frame.ip         = 0;
        frame.stackBase  = (int)stack_.size();
        frame.captured   = captured;
        frame.returnType = returnType;
        frame.fnName     = fnName;

        // bind parameters into this frame's locals
        for (size_t i = 0; i < chunk->params.size(); ++i)
            frame.locals[chunk->params[i].name] = args[i];

        frames_.push_back(std::move(frame));
        // clear injection buffer when entering a new frame
        injBuf_.reset();
    }

    void doReturn(Value result) {
        if (frames_.empty()) { state_ = State::Done; push(std::move(result)); return; }

        CallFrame leaving = std::move(frames_.back());
        frames_.pop_back();

        // typecheck return value
        if (!leaving.returnType.isAny() && !leaving.returnType.contains(result.tag()))
            vmError("'" + leaving.fnName + "' declared return type " +
                    leaving.returnType.name() + " but returned " + result.typeName(), 0);

        // discard any values the returning frame left on the stack above its base
        while ((int)stack_.size() > leaving.stackBase) stack_.pop_back();

        if (frames_.empty()) {
            state_ = State::Done;
            push(std::move(result));
        } else {
            push(std::move(result)); // caller receives return value
        }
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

    // invoke helper for built-in invoke() method
    Value invokeChunk(
        const std::shared_ptr<CompiledChunk>& chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        const std::vector<Value>&             args,
        const SourceRange&                    site,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        // push new frame and run dispatch loop until the frame returns
        // record the depth to detect when the frame returns
        int targetDepth = (int)frames_.size();
        pushFrame(chunk, captured, args, returnType, fnName);

        State savedState = state_;
        state_ = State::Running;
        while ((int)frames_.size() > targetDepth && state_ == State::Running)
            execOne();

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
        throw EmbrError(out.str(), line > 0, {line, 0, line, 0});
    }

    // incomprehensible. may lord have mercy on my soul
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
        // search vm frames
        for (auto& f : frames_) {
            for (auto& [k, _] : f.locals) {
                size_t d = editDist(name, k, bestD);
                if (d < bestD) { bestD = d; best = k; }
            }
        }
        // search interpreter globals
        for (auto& [k, _] : interp_.globals()) {
            size_t d = editDist(name, k, bestD);
            if (d < bestD) { bestD = d; best = k; }
        }
        return best;
    }

    // scope tracking for SCOPE_PUSH/POP 
    int                                   scopeDepth_ = 0;
    std::vector<std::vector<std::string>> scopeNames_; // names defined at each depth
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
    // Store AST so lambdas that reference tree-walk bodies stay valid
    interp.storeProgram(std::move(ast));
    // Re-parse for the compiler (storeProgram moved the AST; parse again)
    // Actually: compile before storing.  Restructure:
    Lexer lex2(src);
    auto tokens2 = lex2.tokenize();
    Parser parser2(std::move(tokens2), sm);
    auto ast2 = parser2.parse();
    Compiler compiler(&interp);
    auto prog = compiler.compile(ast2, filename);
    interp.storeProgram(std::move(ast2));
    return prog;
}

// compile and run in one call
inline void runSource(const std::string& src,
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
    interp.storeProgram(std::move(ast));

    VmRunner runner(interp);
    try {
        runner.run(prog);
    } catch (const EmbrError& e) {
        std::cerr << e.what();
    }
}

} // namespace vm
} // namespace embr

#endif // EMBR_VM_CPP_INCLUDED