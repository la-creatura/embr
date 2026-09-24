#ifndef EMBR_CORE_REGISTRY_H
#define EMBR_CORE_REGISTRY_H

// the plugin-facing runtime with global scope, plugin handles, source map, and module import/export

#include "diagnostics.h"
#include "types.h"
#include "value.h"
#include "ast.h"
#include "platform.h"

#include <string>
#include <vector>
#include <deque>
#include <set>
#include <unordered_map>
#include <memory>
#include <iostream>

namespace embr {

class Interpreter {
public:
    using NativeFn = Value::NativeFn;

    Interpreter() {
        scopes_.emplace_back();
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
        // remove anything holding plugin function pointers
        scopes_.clear();

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

    void push() { scopes_.emplace_back(); }
    void pop()  { scopes_.pop_back(); }

    // assign into nearest scope that already holds the name,
    // falling back to the current module floor (not necessarily scopes_[0])
    void set(const std::string& k, Value v) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->count(k)) { (*it)[k] = std::move(v); return; }
        scopes_[moduleBase_][k] = std::move(v);
    }

    // ideally should be wrapped in has() where used to throw a more meaningful error
    Value get(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        raiseError("runtime", "undefined variable: " + name);
    }

    bool has(const std::string& name) const {
        for (auto& sc : scopes_) if (sc.count(name)) return true;
        return false;
    }

    void define(const std::string& k, Value v) { scopes_.back()[k] = std::move(v); }

    // bind nativefn with no sigcheck
    void bind(const std::string& name, NativeFn fn) {
        scopes_.front()[name] = Value::makeNative(name, std::move(fn), {});
    }

    // bind nativefn with sig
    void bindSig(const std::string& name, std::vector<Param> sig, NativeFn fn) {
        scopes_.front()[name] = Value::makeNative(name, std::move(fn), std::move(sig));
    }

    // register scriptfn into innermost scope
    void defineScriptFn(ScriptFn fn) {
        std::string nm = fn.name;
        scopes_.back()[nm] = Value::makeScript(std::move(fn));
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
    // reference must outlive interpreter
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

    // capture non-native scopes visible from the current call depth
    // closures capture everything from moduleBase_ upward so they can
    // read/write module globals as well as genuinely-local upvalues
    std::shared_ptr<CaptureFrame> captureLocals() const {
        if (scopes_.size() <= moduleBase_ + 1) return nullptr;
        auto frame = std::make_shared<CaptureFrame>();
        for (size_t i = moduleBase_; i < scopes_.size(); ++i)
            for (const auto& [k, v] : scopes_[i])
                (*frame)[k] = v;
        return frame;
    }

    void pushCapture(const std::shared_ptr<CaptureFrame>& cap) {
        if (cap) scopes_.push_back(*cap);
        else     scopes_.emplace_back();
    }
    // returns the current module's "global" scope (the assignment floor)
    // for the top-level script this is scopes_[0] (the native layer)
    // for an imported module it is the module's own export scope
    const std::unordered_map<std::string, Value>& globals() const { return scopes_[moduleBase_]; }
    // plugin fns, built-ins always scopes_[0]
    const std::unordered_map<std::string, Value>& nativeGlobals() const { return scopes_[0]; }

    const std::vector<std::unordered_map<std::string, Value>>& allScopes() const { return scopes_; }
    const std::vector<StmtPtr>* storeProgram(std::vector<StmtPtr> prog) {
        ownedPrograms_.push_back(std::move(prog));
        return &ownedPrograms_.back();
    }

    std::string               scriptDir;
    std::vector<PluginHandle> pluginHandles_;

    // push a new module scope
    // sets moduleBase_ to the new scope
    size_t pushModuleScope() {
        scopes_.emplace_back();
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

    // pop the current module scope and restores moduleBase_ to the parent
    // returns the scope and its local-name set so the caller can filter exports
    ModuleScopeResult popModuleScope() {
        ModuleScopeResult res;
        res.scope  = std::move(scopes_.back());
        res.locals = std::move(moduleLocalSets_.back());
        scopes_.pop_back();
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

    // canonical set of already-loaded module paths (deduplication)
    std::set<std::string> loadedModules_;

private:
    std::deque<std::vector<StmtPtr>> ownedPrograms_;
    std::vector<std::unordered_map<std::string, Value>> scopes_;

    size_t                             moduleBase_ = 0;
    std::vector<size_t>                prevModuleBases_;
    std::vector<std::set<std::string>> moduleLocalSets_;

    SourceMap   srcMap_;
    std::string currentFile_ = "<input>";

    // c++ type to Value
    static Value toValue(double v)             { return Value(v); }
    static Value toValue(float v)              { return Value((double)v); }
    static Value toValue(int v)                { return Value((double)v); }
    static Value toValue(bool v)               { return Value(v); }
    static Value toValue(const std::string& v) { return Value(v); }
    static Value toValue(void* p)              { return Value(p); }

    // Value to c++ variable
    static void fromValue(const Value& v, double& out)      { out = v.asNumber(); }
    static void fromValue(const Value& v, float& out)       { out = (float)v.asNumber(); }
    static void fromValue(const Value& v, int& out)         { out = (int)v.asNumber(); }
    static void fromValue(const Value& v, bool& out)        { out = v.truthy(); }
    static void fromValue(const Value& v, std::string& out) { out = v.asString(); }
    static void fromValue(const Value& v, void*& out)       { out = v.asPointer(); }

    // return typeset for T
    template<typename T> static constexpr TypeSet typeConstraintFor() { return TypeSet::Any(); }
};

// typeConstraintFor<T> specialisations
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<double>()      { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<float>()       { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<int>()         { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<bool>()        { return TypeSet(TypeTag::Number); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<std::string>() { return TypeSet(TypeTag::String); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<void*>()       { return TypeSet(TypeTag::Pointer);}

} // namespace embr

#endif // EMBR_CORE_REGISTRY_H
