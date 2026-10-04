#ifndef EMBR_CORE_REGISTRY_H
#define EMBR_CORE_REGISTRY_H

// the plugin-facing runtime with global scope, plugin handles, source map, and module import/export

#include "diagnostics.h"
#include "types.h"
#include "value.h"
#include "ast.h"
#include "platform.h"

#include <atomic>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <unordered_map>
#include <memory>
#include <iostream>

namespace embr {

// which native plugins a script may `import`, and whether embrlib may touch files
// the default is unrestricted. a restricted policy (--sandbox, --allow-plugins, --no-plugins, or an embedder's own)
// is enforced in one place, importNativePlugin() in module.h, which both backends call. it is also copied into the
// worker Interpreters that parallel/async create, so a script can't escape by spawning a worker
// this is an import allow-list, not a full sandbox: nothing limits cpu, memory or time, and an allowed plugin can do anything it can do
struct PluginPolicy {
    bool                     restricted = false;   // false: everything allowed
    std::set<std::string>    allowed;              // plugin names (bare, no extension) when restricted
    bool                     fileIo = true;        // embrlib's load_file / write_file / append_file

    // "" if importing `rawPath` is permitted, else a message saying why not
    std::string check(const std::string& rawPath) const {
        if (!restricted) return "";
        // a restricted policy only accepts bare names: a path ("./json", "../plugins/os", "/tmp/x/json")
        // would let a script pick the library file, and an attacker's json.so is a plugin whose name
        // happens to be allowed
        if (rawPath.empty() || rawPath.find_first_of("/\\") != std::string::npos ||
            rawPath.find('.') != std::string::npos)
            return "plugin imports must be bare names (no path, no extension) in a restricted sandbox: \"" + rawPath + "\"";
        if (!allowed.count(rawPath)) {
            std::string list;
            for (const auto& n : allowed) list += (list.empty() ? "" : ", ") + n;
            return "plugin \"" + rawPath + "\" is not allowed in this sandbox (allowed: " +
                   (list.empty() ? "none" : list) + ")";
        }
        return "";
    }
};

class Interpreter {
public:
    using NativeFn = Value::NativeFn;

    Interpreter() {
        auto s0 = std::make_shared<Scope>();
        scopes_.push_back(s0);
        gcRegisterScope(s0);
        varTypes_.emplace_back();
        bindSig("print", {Param::rest("args", TS::Any)},
        [](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << args[i].formatAsString();
            }
            std::cout << '\n';
            return Value(0.0);
        });
        bindSig("globals", {},
        [this](const std::vector<Value>&) -> Value {
            Value::map_type m;
            for (const auto& [k, v] : this->globals())
                m[k] = v;
            return Value(std::move(m));
        });
    }
    ~Interpreter() {
        // break reference cycles before anything is released. a closure stored in a scope captures that same scope,
        // so dropping scopes_ only drops our references and the cycle would leak at exit. emptying every live scope
        // frees their contents, and lets plugin-owned values (tables, handles) die while their plugin is still loaded
        scopesChanged();
        for (auto& sc : gcAllLiveScopes()) sc->clear();
        // empty every plugin container that is still alive (a table holding itself is a cycle no scope owns)
        for (auto& c : clearables_) if (auto owner = c.owner.lock()) { (void)owner; c.clear(); }
        clearables_.clear();
        for (auto& hook : teardownHooks_) hook();
        teardownHooks_.clear();        // hooks are std::functions whose code may live in a plugin: drop before dlclose
        // remove anything holding plugin function pointers
        scopes_.clear();
        // GC root providers are std::functions whose code lives in the plugin that registered them;
        // destroying one after pluginClose() below would run code in an unmapped .so (segfault)
        gcRootProviders_.clear();

        for (auto h : pluginHandles_)
            pluginClose(h);
    }

    Interpreter(const Interpreter&)            = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    class ScopeGuard {
        Interpreter& i_;
    public:
        explicit ScopeGuard(Interpreter& i) : i_(i) { i_.push(); }
        ~ScopeGuard()                               { i_.pop();  }
    };

    void push() {
        scopesChanged();
        auto s = std::make_shared<Scope>();
        scopes_.push_back(s);
        gcRegisterScope(s);
        varTypes_.emplace_back();
    }
    void pop()  { scopesChanged(); scopes_.pop_back(); varTypes_.pop_back(); }

    // assign into nearest scope that already holds the name
    // checking it against that scope's recorded type constraint if there is one
    // falling back to the current module floor (not necessarily scopes_[0]) for a name never declared
    void set(const std::string& k, Value v) {
        for (size_t idx = scopes_.size(); idx-- > 0; ) {
            auto it = scopes_[idx]->find(k);
            if (it == scopes_[idx]->end()) continue;
            if (!varTypes_[idx].empty()) {
                auto tyIt = varTypes_[idx].find(k);
                if (tyIt != varTypes_[idx].end() && !tyIt->second.isAny() && !tyIt->second.contains(v.tag()))
                    raiseError("runtime", "cannot assign " + v.typeName() + " to '" + k +
                               "' declared as " + tyIt->second.name());
            }
            it->second = std::move(v);
            return;
        }
        scopesChanged();
        (*scopes_[assignFloor()])[k] = std::move(v);
    }

    // the scope where assigning to a brand-new name creates it: the running function's scope (so a name first
    // assigned in a fn is local to that call), or the module floor when no function is running (top-level code,
    // including inside its if/while blocks, makes module-level globals). the VM does the same (VmRunner::resolveStore)
    // to set a global from inside a function, use the embrmeta plugin
    size_t assignFloor() const { return funcFloor_ > moduleBase_ ? funcFloor_ : moduleBase_; }
    size_t topScopeIndex() const { return scopes_.size() - 1; }
    // a function call points the floor at its own (innermost) scope; returns the previous floor so
    // the caller can restore it when the call ends
    size_t swapFuncFloor(size_t idx) { size_t old = funcFloor_; funcFloor_ = idx; return old; }

    // ideally should be wrapped in has() where used to throw a more meaningful error
    Value get(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = (*it)->find(name);
            if (f != (*it)->end()) return f->second;
        }
        raiseError("runtime", "undefined variable: " + name);
    }

    // the value bound to name in the innermost scope that has it, or null. one lookup, no copy
    const Value* find(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = (*it)->find(name);
            if (f != (*it)->end()) return &f->second;
        }
        return nullptr;
    }

    // bumped whenever a scope is pushed, popped, or gains or loses a name, which is exactly when a pointer
    // from findForCache() could stop being the right one. the vm keeps such pointers between instructions and
    // checks this number before using one. the number is unique across interpreters
    uint64_t scopeEpoch() const { return scopeEpoch_; }
    void scopesChanged() {
        static std::atomic<uint64_t> next{1};
        scopeEpoch_ = next.fetch_add(1, std::memory_order_relaxed);
    }

    // like findMut(), and also says whether the scope holding the name has a type constraint on it
    // (then assigning has to go through set(), which checks it)
    Value* findForCache(const std::string& name, bool& typed) {
        for (size_t idx = scopes_.size(); idx-- > 0; ) {
            auto it = scopes_[idx]->find(name);
            if (it == scopes_[idx]->end()) continue;
            typed = false;
            if (!varTypes_[idx].empty()) {
                auto ty = varTypes_[idx].find(name);
                typed = ty != varTypes_[idx].end() && !ty->second.isAny();
            }
            return &it->second;
        }
        return nullptr;
    }

    Value* findMut(const std::string& name) {
        return const_cast<Value*>(find(name));
    }

    bool has(const std::string& name) const {
        for (auto& sc : scopes_) if (sc->count(name)) return true;
        return false;
    }

    // records a type constraint checked on every later assignment to k within this scope's lifetime
    // used by local <type> name and local auto name declarations
    void define(const std::string& k, Value v, TypeSet ts = TypeSet::Any()) {
        scopesChanged();
        (*scopes_.back())[k] = std::move(v);
        if (!ts.isAny()) varTypes_.back()[k] = ts;
        else              varTypes_.back().erase(k);
    }

    // bind nativefn with no sigcheck
    void bind(const std::string& name, NativeFn fn) {
        scopesChanged();
        (*scopes_.front())[name] = Value::makeNative(name, std::move(fn), {});
    }

    // bind nativefn with sig
    void bindSig(const std::string& name, std::vector<Param> sig, NativeFn fn) {
        scopesChanged();
        (*scopes_.front())[name] = Value::makeNative(name, std::move(fn), std::move(sig));
    }

    // defines name in the native/global scope (scopes_[0]), the same place bind()/bindSig() write to, but for any
    // Value. unlike define(), which writes wherever the caller is running, this is visible from every module
    // exists for metaprogramming (plugins/embrmeta): a script installs a binding under a name it only knows at
    // runtime, like ffi_cdef naming a function after a C declaration. plain `name = value` can't do that
    void defineGlobal(const std::string& name, Value v) {
        scopesChanged();
        (*scopes_.front())[name] = std::move(v);
    }

    // removes name from the native/global scope only (mirrors
    // defineGlobal's scope, not set()/get()'s full scope-chain search).
    // returns true if something was actually erased.
    bool undefineGlobal(const std::string& name) {
        scopesChanged();
        return scopes_.front()->erase(name) > 0;
    }

    bool hasGlobal(const std::string& name) const {
        return scopes_.front()->count(name) > 0;
    }

    // reads a binding from the native/global scope specifically, unlike
    // get() (which walks the whole scope chain and returns whatever shadows
    // it first), this always answers "what did defineGlobal() last set this
    // to", even if some inner/module scope happens to define the same name
    const Value& getGlobal(const std::string& name) const {
        return scopes_.front()->at(name);
    }

    // register scriptfn into innermost scope
    void defineScriptFn(ScriptFn fn) {
        std::string nm = fn.name;
        scopesChanged();
        (*scopes_.back())[nm] = Value::makeScript(std::move(fn));
    }

    void setSource(const std::string& src,
                   const std::string& filename = "<input>") {
        srcMap_.load(src); currentFile_ = filename;
    }
    void setSource(SourceMap m, const std::string& filename = "<input>") {
        srcMap_ = std::move(m); currentFile_ = filename;
    }
    const SourceMap&     sourceMap() const { return srcMap_; }
    const std::string& currentFile() const { return currentFile_; }

    // exposes C++ variable as getter/setter pair
    // reference must outlive Interpreter
    template<typename T>
    void bindVar(const std::string& name, T& ref) {
        // name() -> value
        bind(name, [&ref](const std::vector<Value>&) -> Value {
            return toValue(ref);
        });
        // set_name(val)
        bindSig("set_" + name, {Param::req("val", typeConstraintFor<T>())},
                [&ref](const std::vector<Value>& args) -> Value {
            fromValue(args[0], ref);
            return Value(0.0);
        });
    }

    // capture the scope layers visible right now, for a closure being created. it captures everything from
    // moduleBase_ upward (module globals too) as a list of the same live Scope objects, not copies. that is what
    // lets sibling closures see each other's changes, and what keeps a module's `local` names alive after
    // popModuleScope() (see CaptureFrame in core/value.h)
    //
    // returns null (meaning "just use the normal scope chain") only for a plain top-level script with nothing
    // nested. that's a shortcut, not a correctness need: capturing scopes_[0] would be just as right
    std::shared_ptr<CaptureFrame> captureLocals() const {
        if (moduleBase_ == 0 && scopes_.size() <= 1) return nullptr;
        auto frame = std::make_shared<CaptureFrame>();
        frame->layers.assign(scopes_.begin() + moduleBase_, scopes_.end());
        return frame;
    }

    // pushes every layer of a capture onto the live scope stack (the same objects, not copies), so a call to the
    // closure sees and can change the environment it closed over. paired with popCapture(), which must pop exactly
    // cap->layers.size() layers (or 1 if there was no capture). see doInvoke in tree_walker.h, the only caller
    //
    // a captured variable's type constraint is not carried over, so assigning to it from inside the closure is not
    // type-checked even if it was declared typed
    void pushCapture(const std::shared_ptr<CaptureFrame>& cap) {
        scopesChanged();
        if (cap) {
            for (auto& layer : cap->layers) scopes_.push_back(layer);
            varTypes_.resize(varTypes_.size() + cap->layers.size());
        } else {
            scopes_.push_back(std::make_shared<Scope>());
            varTypes_.emplace_back();
        }
    }
    // returns the current module's global scope (the assignment floor)
    // for the top-level script this is scopes_[0] (the native layer)
    // for an imported module it is the module's own export scope
    const Scope& globals() const { return *scopes_[moduleBase_]; }
    // plugin fns, built-ins always scopes_[0]
    const Scope& nativeGlobals() const { return *scopes_[0]; }

    const std::vector<std::shared_ptr<Scope>>& allScopes() const { return scopes_; }

    // --- support for the opt-in gc plugin (plugins/gc/gc.cpp) ----------
    //
    // closures share live Scope objects, and reference counting alone can't free a cycle (a closure stored in a
    // variable inside its own captured scope). the gc plugin finds such cycles by mark-and-sweep over every scope:
    // whatever isn't reachable from the active call stack gets its contents cleared
    //
    // gcRegisterScope() tracks (weakly) every Scope either backend creates. call it only where a new Scope is made,
    // not when an existing one is pushed back on a stack (pushCapture() doesn't re-register)
    void gcRegisterScope(const std::shared_ptr<Scope>& s) { gcScopes_.push_back(s); }

    // a plugin function only ever gets an Interpreter&, never the VmRunner, so the VM mirrors its call frames into
    // this stack. each entry is the scopes one VM CallFrame contributes to the root set (its locals plus its
    // captured layers), pushed and popped together with vm.h's frames_. the tree-walker needs nothing like it,
    // its active scopes are scopes_ itself (see allScopes() above)
    //
    // returns the live entry, not an index, so a frame captured after being pushed (VmRunner::promoteToScope())
    // can append its new Scope later through its own stored handle. an index isn't safe: this is one
    // Interpreter-wide stack that several VmRunners push to (a module import runs a nested one), so a frame's
    // position in its own frames_ says nothing about its entry's position here
    std::shared_ptr<std::vector<std::shared_ptr<Scope>>> gcPushVmFrameRoots(std::vector<std::shared_ptr<Scope>> roots) {
        auto entry = std::make_shared<std::vector<std::shared_ptr<Scope>>>(std::move(roots));
        gcVmFrameRoots_.push_back(entry);
        return entry;
    }
    void gcPopVmFrameRoots() {
        if (!gcVmFrameRoots_.empty()) gcVmFrameRoots_.pop_back();
    }

    // takes one frame's entries out of both root stacks by identity, searching from the top. a suspended coroutine
    // leaves its entries in the middle of the stack while other frames come and go above it, so "pop the top" would
    // remove the wrong ones
    void gcReleaseVmFrame(const std::shared_ptr<std::vector<std::shared_ptr<Scope>>>& roots,
                          const std::shared_ptr<std::vector<Value>>& slots) {
        for (size_t i = gcVmFrameRoots_.size(); i-- > 0; )
            if (gcVmFrameRoots_[i] == roots) { gcVmFrameRoots_.erase(gcVmFrameRoots_.begin() + i); break; }
        for (size_t i = gcVmFrameValueRoots_.size(); i-- > 0; )
            if (gcVmFrameValueRoots_[i] == slots) { gcVmFrameValueRoots_.erase(gcVmFrameValueRoots_.begin() + i); break; }
    }

    // a VM frame that no closure has captured yet keeps its locals in plain slots, not a Scope, so there is no
    // Scope to hand gcPushVmFrameRoots(). but a Value in one of those slots (a closure, say) can still reach live
    // Scopes the gc must not miss. same push/pop-together protocol as above, but carrying raw Values.
    // gc.cpp reads them with gcValueRoots()
    void gcPushVmFrameValueRoots(std::shared_ptr<std::vector<Value>> slots) {
        gcVmFrameValueRoots_.push_back(std::move(slots));
    }
    void gcPopVmFrameValueRoots() {
        if (!gcVmFrameValueRoots_.empty()) gcVmFrameValueRoots_.pop_back();
    }

    // scopes reachable right now from an active call stack on either
    // backend, the root set a mark phase starts from.
    std::vector<std::shared_ptr<Scope>> gcRoots() const {
        std::vector<std::shared_ptr<Scope>> roots = scopes_;
        for (auto& frameRoots : gcVmFrameRoots_)
            roots.insert(roots.end(), frameRoots->begin(), frameRoots->end());
        return roots;
    }

    // raw Value roots contributed by not-yet-captured VM frames, see
    // gcPushVmFrameValueRoots() above. a mark phase must walk each Value in
    // each of these (e.g. gc.cpp's markValue()) alongside gcRoots()'s Scopes.
    const std::vector<std::shared_ptr<std::vector<Value>>>& gcValueRoots() const {
        return gcVmFrameValueRoots_;
    }

    // a plugin container that can hold Values (a table) registers here so it gets emptied when the Interpreter is
    // destroyed. a container that holds itself (`table_set(t, "self", t)`) is a cycle no scope owns and would leak
    // `owner` is held weakly, and `clear` runs before the plugin is unloaded
    // teardown only: gc_collect() doesn't use this, because a value that lives only on the VM operand stack
    // mid-expression isn't a known root and could be emptied while still in use
    void addClearable(std::weak_ptr<void> owner, std::function<void()> clear) {
        if (clearables_.size() >= clearablePruneAt_) {      // drop entries whose owner is already gone
            size_t w = 0;
            for (auto& c : clearables_) if (!c.owner.expired()) clearables_[w++] = std::move(c);
            clearables_.resize(w);
            clearablePruneAt_ = std::max<size_t>(4096, clearables_.size() * 2);
        }
        clearables_.push_back({std::move(owner), std::move(clear)});
    }

    // run once when the Interpreter is destroyed, before its scopes are released and before any plugin
    // is unloaded. for things that hold reference cycles the scopes can't reach (the VM's compiled chunks
    // share a sibling list that contains themselves). a hook must not rely on the Interpreter's scopes.
    void addTeardownHook(std::function<void()> h) { teardownHooks_.push_back(std::move(h)); }

    // Values a plugin keeps in its own state that must stay alive even if no script variable refers to a handle
    // for them. example: ffi callbacks. C code holds the function pointer until ffi_callback_free(), so the script
    // closure behind it is a root even after the script drops its handle. gc_collect() calls every provider and
    // marks what it visits. (values only reachable through a handle should use TypedPtr::tracer instead, see core/value.h)
    // a provider must not keep plugin state alive on its own: capture a weak_ptr
    using GcRootProvider = std::function<void(const TraceVisitor&)>;
    void addGcRootProvider(GcRootProvider p) { gcRootProviders_.push_back(std::move(p)); }
    const std::vector<GcRootProvider>& gcRootProviders() const { return gcRootProviders_; }

    // every scope still alive anywhere (on a stack, captured by a live
    // closure, or orphaned in an unreachable cycle), prunes dead weak_ptrs
    // from the registry as a side effect, so the registry doesn't grow
    // unboundedly across a long-lived interpreter's lifetime.
    std::vector<std::shared_ptr<Scope>> gcAllLiveScopes() {
        std::vector<std::shared_ptr<Scope>> live;
        live.reserve(gcScopes_.size());
        size_t w = 0;
        for (size_t r = 0; r < gcScopes_.size(); ++r) {
            if (auto sp = gcScopes_[r].lock()) {
                live.push_back(sp);
                gcScopes_[w++] = std::move(gcScopes_[r]);
            }
        }
        gcScopes_.resize(w);
        return live;
    }

    const std::vector<StmtPtr>* storeProgram(std::vector<StmtPtr> prog) {
        ownedPrograms_.push_back(std::move(prog));
        return &ownedPrograms_.back();
    }

    std::string               scriptDir;
    PluginPolicy              pluginPolicy;       // see PluginPolicy
    // arguments after a bare `--` on the embr command line (embr script.embr -- a b c);
    // read by the os plugin's os_args()
    std::vector<std::string>  scriptArgs;
    std::vector<PluginHandle> pluginHandles_;

    // push a new module scope
    // sets moduleBase_ to the new scope
    size_t pushModuleScope() {
        scopesChanged();
        auto s = std::make_shared<Scope>();
        scopes_.push_back(s);
        gcRegisterScope(s);
        varTypes_.emplace_back();
        size_t idx = scopes_.size() - 1;
        prevModuleBases_.push_back(moduleBase_);
        moduleBase_ = idx;
        moduleLocalSets_.emplace_back();
        return idx;
    }

    struct ModuleScopeResult {
        std::unordered_map<std::string, Value> scope;
        std::set<std::string>                  locals;  // names declared with 'local'
    };

    // pop the current module scope and restore moduleBase_ to the parent
    // returns a copy of the scope's contents (and its local-name set) so the caller can decide what to export
    // it is a copy, not a move, because an exported closure may still hold the real scope object and must keep seeing it
    ModuleScopeResult popModuleScope() {
        ModuleScopeResult res;
        scopesChanged();
        res.scope  = *scopes_.back();
        res.locals = std::move(moduleLocalSets_.back());
        scopes_.pop_back();
        varTypes_.pop_back();
        moduleBase_ = prevModuleBases_.back();
        prevModuleBases_.pop_back();
        moduleLocalSets_.pop_back();
        return res;
    }

    // mark a name as module-local at the current module top scope
    // called by exec(LocalStmt) when exactly at the module floor
    void markModuleLocal(const std::string& name) {
        if (!moduleLocalSets_.empty())
            moduleLocalSets_.back().insert(name);
    }

    // true when executing statements directly in the module top scope
    bool atModuleTopScope() const {
        return scopes_.size() - 1 == moduleBase_;
    }

    // true while running inside an imported module (a top-level script never moves moduleBase_ off 0)
    // vm.h's snapshotLocals() uses it to decide whether a closure made inside a module also needs the module's
    // top scope in its capture (same idea as captureLocals() above, different mechanism per backend)
    bool insideImportedModule() const {
        return moduleBase_ != 0;
    }

    // a shared reference to the current module's top scope (or the script's, at moduleBase_ == 0). vm.h's
    // snapshotLocals() adds it to a closure's CaptureFrame the way captureLocals() does, since the VM can't reach scopes_ directly
    std::shared_ptr<Scope> currentModuleScope() const {
        return scopes_[moduleBase_];
    }

    // canonical set of already-loaded module paths (deduplication)
    std::set<std::string> loadedModules_;

    // guards one callable invocation against unbounded native C++ stack recursion from a deeply/infinitely recursive script
    // both backends construct one of these per call so a script that recurses too deep raises a clean, catchable EmbrError
    // shared on Interpreter cuz a call chain can hop between backends and the depth must be tracked across that
    struct CallDepthGuard {
        Interpreter& i;
        explicit CallDepthGuard(Interpreter& interp, const SourceRange& site = {}) : i(interp) {
            i.enterCall(site);
        }
        CallDepthGuard(const CallDepthGuard&) = delete;
        ~CallDepthGuard() { i.exitCall(); }
    };

    // non-RAII form of the same guard for vm whose call frame outlives the C++ function that pushed it
    // pair every enterCall() with exactly one later exitCall() when that frame is actually popped (see vm.h's pushFrame/doReturn)
    void enterCall(const SourceRange& site = {}) {
        if (++callDepth_ > maxCallDepth_) {
            --callDepth_;
            raiseError("runtime", "stack overflow: max call depth (" +
                       std::to_string(maxCallDepth_) + ") exceeded", site, sourceMap());
        }
    }
    void exitCall() { --callDepth_; }

    void   setMaxCallDepth(size_t n) { maxCallDepth_ = n; }
    size_t maxCallDepth() const      { return maxCallDepth_; }

private:
    std::deque<std::vector<StmtPtr>> ownedPrograms_;
    // each layer is shared_ptr-managed so a closure's CaptureFrame can hold
    // a reference to the same live object rather than a copy of its
    // contents, see core/value.h's CaptureFrame doc comment.
    std::vector<std::shared_ptr<Scope>> scopes_;
    // parallel to scopes_
    // per-scope type constraints recorded by typed/auto local declarations, consulted by set()
    std::vector<std::unordered_map<std::string, TypeSet>> varTypes_;
    uint64_t                                              scopeEpoch_ = 0;

    // gc plugin support, see gcRegisterScope()/gcPushVmFrameRoots() above
    std::vector<std::weak_ptr<Scope>>                              gcScopes_;
    std::vector<std::shared_ptr<std::vector<std::shared_ptr<Scope>>>> gcVmFrameRoots_;
    // parallel to gcVmFrameRoots_, pushed/popped alongside it for the same
    // frame, see gcPushVmFrameValueRoots()'s doc comment.
    std::vector<std::shared_ptr<std::vector<Value>>> gcVmFrameValueRoots_;
    std::vector<GcRootProvider>                      gcRootProviders_;
    std::vector<std::function<void()>>               teardownHooks_;
    struct Clearable { std::weak_ptr<void> owner; std::function<void()> clear; };
    std::vector<Clearable>                           clearables_;
    size_t                                           clearablePruneAt_ = 4096;
    size_t                                           funcFloor_ = 0;   // see assignFloor()

    size_t                             moduleBase_ = 0;
    std::vector<size_t>                prevModuleBases_;
    std::vector<std::set<std::string>> moduleLocalSets_;
    size_t                             callDepth_    = 0;
    size_t                             maxCallDepth_ = 1000;

    SourceMap   srcMap_;
    std::string currentFile_ = "<input>";

    // c++ type to Value
    static Value toValue(double v)             { return Value(v); }
    static Value toValue(float v)              { return Value((double)v); }
    static Value toValue(int v)                { return Value((double)v); }
    static Value toValue(int64_t v)            { return Value(v); }
    static Value toValue(bool v)               { return Value(v); }
    static Value toValue(const std::string& v) { return Value(v); }
    static Value toValue(void* p)              { return Value::makePointer(p); }

    // Value to c++ variable
    static void fromValue(const Value& v, double& out)      { out = v.asNumber(); }
    static void fromValue(const Value& v, float& out)       { out = (float)v.asNumber(); }
    static void fromValue(const Value& v, int& out)         { out = (int)v.asNumber(); }
    static void fromValue(const Value& v, int64_t& out)     { out = v.asInt(); }
    static void fromValue(const Value& v, bool& out)        { out = v.truthy(); }
    static void fromValue(const Value& v, std::string& out) { out = v.asString(); }
    static void fromValue(const Value& v, void*& out)       { out = v.asPointer().ptr; }

    // return typeset for T
    template<typename T> static constexpr TypeSet typeConstraintFor() { return TypeSet::Any(); }
};

// typeConstraintFor<T> specialisations
// double/float/int/bool all accept either numeric subtype (Number or Int)
// asNumber()/asInt() widen on their own, so bindVar()'s auto-marshaling shouldn't reject a literal that
// defaulted to the wrong numeric tag
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<double>()      { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<float>()       { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<int>()         { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<int64_t>()     { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<bool>()        { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<std::string>() { return TypeSet(TypeTag::String); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<void*>()       { return TypeSet(TypeTag::Pointer);}

// "dunder" dispatch: a plugin-tagged pointer can opt into being called, used with + - * / % > < >= <=, or act as a
// map's prototype (see lookupMapChain() below). one convention, three uses: look up a specially named global function
//
// the name is __<tag>_<op>. to make it always a valid identifier, every '.' in the tag becomes '_' and every real
// '_' is doubled to '__' ("table.handle" -> __table_handle_call). so a script can define a dunder for its own tag
// with a plain `fn __mytag_call(...) ... end`. a plugin can register a dotted one straight from C++ (bind() takes any string)
//
// doubling '_' keeps it collision-free: just replacing '.' with '_' would give "a.b_c" and "a_b.c" the same name.
// the encoding only has to be injective (never decoded back)
inline std::string dunderName(const std::string& tag, const std::string& op) {
    std::string s;
    s.reserve(tag.size());
    for (char c : tag) {
        if      (c == '.') s += '_';
        else if (c == '_') s += "__";
        else                s += c;
    }
    return "__" + s + "_" + op;
}

// resolves __<tag>_<op> as a global callable for v (a typed pointer with a
// non-empty tag). returns false (outFn untouched) if v isn't a tagged
// pointer, no such global exists, or it exists but isn't callable, callers
// treat false as "no override, fall back to the built-in behavior."
inline bool resolveDunder(const Interpreter& interp, const Value& v, const std::string& op, Value& outFn) {
    if (!v.isPointer()) return false;
    const std::string& tag = v.asPointer().type;
    if (tag.empty()) return false;
    std::string name = dunderName(tag, op);
    if (!interp.hasGlobal(name)) return false;
    Value fn = interp.getGlobal(name);
    if (!fn.isCallable()) return false;
    outFn = std::move(fn);
    return true;
}

// maps a binary operator to the dunder op name resolveDunder() uses, or nullptr if it can't be overridden
// shared by both backends (tree_walker.h evalBinary, vm.h Op::ADD..Op::GTE)
// "==" and "!=" are left out on purpose: they always work structurally through Value::operator==, and letting
// a plugin override equality could break things that rely on it (map dedup, shared_table keys, ...)
inline const char* arithDunderOp(const std::string& op) {
    if (op == "+")  return "add";
    if (op == "-")  return "sub";
    if (op == "*")  return "mul";
    if (op == "/")  return "div";
    if (op == "%")  return "mod";
    if (op == ">")  return "gt";
    if (op == "<")  return "lt";
    if (op == ">=") return "gte";
    if (op == "<=") return "lte";
    return nullptr;
}

// map member lookup with prototype fallback: if key isn't in the map and the map has a "__proto__" key, the
// lookup continues there instead of failing
//   - a map prototype: same lookup again, recursively (multi-level chains work), depth-capped so a cycle fails cleanly
//   - a typed pointer prototype: looks up the global __<tag>_<key> (same dunder convention, with the key as the "op"),
//     so a map whose __proto__ is a "shape" pointer falls back to __shape_area for m["area"]
// returns false (out untouched) on a total miss
// only m[key] reads use this. writes always go to the map's own slots, and `for k in m` only sees its own keys
inline bool lookupMapChain(const Interpreter& interp, const Value& map, const std::string& key,
                           Value& out, int depth = 0) {
    if (depth > 32) return false;
    const auto& m = map.asMap();
    auto it = m.find(key);
    if (it != m.end()) { out = it->second; return true; }
    auto protoIt = m.find("__proto__");
    if (protoIt == m.end()) return false;
    const Value& proto = protoIt->second;
    if (proto.isMap())     return lookupMapChain(interp, proto, key, out, depth + 1);
    if (proto.isPointer()) return resolveDunder(interp, proto, key, out);
    return false;
}

} // namespace embr

#endif // EMBR_CORE_REGISTRY_H
