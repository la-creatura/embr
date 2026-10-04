#ifndef EMBR_CORE_INVOKE_H
#define EMBR_CORE_INVOKE_H

// backend-agnostic callable invocation
//
// a plugin that needs to call back into script code should call embr::invoke(), not build a specific
// backend's runner. then the same plugin binary works whether the host has the tree-walker, the VM, or both
// see the EMBR_WITH_* options in the top-level CMakeLists.txt
//
// the choice is made per callable, not per active backend:
//   - a native function is just called
//   - a script function compiled by the vm gets a VmRunner
//   - a script function that only has an AST body gets a tree_walker::Runner
// a new runner is made for that one call and thrown away, so either backend can call a function the
// other one made, as long as both are compiled in

#include "value.h"
#include "registry.h"
#include "module.h"

#if !defined(EMBR_WITH_TREE_WALKER) && !defined(EMBR_WITH_VM)
#error "embr: no execution backend compiled in. define EMBR_WITH_TREE_WALKER and/or EMBR_WITH_VM"
#endif

namespace embr {

inline Value invoke(Interpreter& interp, const Value& callee,
                    const std::vector<Value>& args, const SourceRange& site = {}) {
    // resolve a __<tag>_call dunder (resolveDunder() in core/registry.h) BEFORE picking a backend. the backend
    // choice depends on whether the callee is a VM-compiled function, which only means something once we know
    // what is really going to run. a pointer with a call dunder isn't a ScriptFn itself, so picking the backend
    // first could choose the wrong one for the resolved function, and the tree-walker would then try to walk
    // a null AST body (or the other way round) and crash instead of raising a clean error
    Value target = callee;
    std::vector<Value> targetArgs = args;
    if (!target.isCallable()) {
        Value dfn;
        if (resolveDunder(interp, target, "call", dfn)) {
            targetArgs.insert(targetArgs.begin(), target);
            target = dfn;
        }
        // else: target is still not callable; fall through and let
        // whichever backend gets picked below raise its own clear
        // "not callable" error, same as always.
    }

#if defined(EMBR_WITH_VM)
    if (target.isCallable()) {
        const auto& c = target.asCallable();
        if (c.isScript() && c.script.compiledChunk) {
            vm::VmRunner r(interp);
            return r.invoke(target, targetArgs, site);
        }
    }
#endif

    // native functions, tree-walked script functions, and a still-
    // unresolved non-callable value all go through the tree-walker's
    // invoke, which already knows how to call a native directly without
    // needing any AST/bytecode at all
#ifdef EMBR_WITH_TREE_WALKER
    Runner r(interp);
    return r.invoke(target, targetArgs, site);
#elif defined(EMBR_WITH_VM)
    // vm only build: VmRunner::invoke() already handles natives itself
    vm::VmRunner r(interp);
    return r.invoke(target, targetArgs, site);
#endif
}

// backend-agnostic .embr module loading, used by embrlib's load_module()
//
// unlike invoke() there is no compiled callable to inspect, so nothing says which backend should run the
// module. the `import` statement doesn't have this problem (each backend runs imports with itself, see
// execImportEmbrModule in tree_walker.h and vm.h), but load_module() is a plain native function that either
// backend may have called. so it uses the usual default: tree-walker if compiled in, otherwise the VM
// (same rule as runSourceAny in tests/harness.h)
//
// mutateCallerScope=false (load_module()) returns the module's exports as a map instead of merging them
// into the caller's scope, see exportModuleScope() in module.h
inline Value loadEmbrModule(Interpreter& interp, const std::string& rawPath,
                            const SourceRange& site, bool mutateCallerScope,
                            bool forceReload = false)
{
    namespace fs = std::filesystem;

    auto cands = fileCandidates(rawPath, interp.scriptDir, ".embr", "modules");
    fs::path resolved = findExistingCandidate(cands);
    if (resolved.empty())
        raiseError("load_module", "cannot find module \"" + rawPath + "\"\n  tried:" +
                   describeCandidates(cands), site, interp.sourceMap());

    const std::string canonStr = canonicalOrSelf(resolved);

    if (forceReload) interp.loadedModules_.erase(canonStr);
    if (interp.loadedModules_.count(canonStr))
        return mutateCallerScope ? Value(0.0) : Value(Value::map_type{});
    interp.loadedModules_.insert(canonStr);

    auto src = tryReadFile(resolved);
    if (!src) raiseError("load_module", "cannot open module file: " + resolved.string(),
                         site, interp.sourceMap());

    std::string savedDir  = interp.scriptDir;
    SourceMap   savedMap  = interp.sourceMap();
    std::string savedFile = interp.currentFile();
    interp.scriptDir = resolved.parent_path().string();

    interp.pushModuleScope();
    try {
        Lexer     lex(*src);
        auto      tokens = lex.tokenize();
        SourceMap sm     = lex.sourceMap();
        Parser    parser(std::move(tokens), sm);
        auto      prog   = parser.parse();

#if defined(EMBR_WITH_TREE_WALKER)
        const std::vector<StmtPtr>* stable = interp.storeProgram(std::move(prog));
        Runner runner(interp);
        runner.run(*stable, sm, resolved.string());
#elif defined(EMBR_WITH_VM)
        vm::Compiler        compiler(&interp);
        vm::CompiledProgram compiled = compiler.compile(prog, resolved.string());
        vm::registerChunkTeardown(interp, compiled);       // free the chunks' sibling cycle at teardown
        interp.setSource(sm, resolved.string());
        vm::VmRunner runner(interp);
        runner.run(compiled);
#endif
    } catch (...) {
        interp.popModuleScope();
        interp.scriptDir = savedDir;
        interp.setSource(savedMap, savedFile);
        throw;
    }

    auto res = interp.popModuleScope();
    interp.scriptDir = savedDir;
    interp.setSource(savedMap, savedFile);

    return exportModuleScope(interp, std::move(res), mutateCallerScope);
}

} // namespace embr

#endif // EMBR_CORE_INVOKE_H
