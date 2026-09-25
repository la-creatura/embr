#ifndef EMBR_CORE_INVOKE_H
#define EMBR_CORE_INVOKE_H

// backend-agnostic callable invocation
//
// plugins that need to call back into embr script code should call embr::invoke()  instead of constructing a specific backend's runner
// that way the same plugin binary works whether the host was built with the tree-walker, the VM, or both
// see the EMBR_WITH_* options in the top-level CMakeLists.txt / cmake/amalgamate.cmake
//
// dispatch is per-callable, not per the currently active backend
// native function is always called directly
// script function whose body was compiled by the vm needs a VmRunner
// one that only has a tree-walkable AST body needs a tree_walker::Runner
// a new runner is constructed for the one call and discarded
// either backend can therefore invoke a callable produced by the other, as long as both happen to be compiled into this build

#include "value.h"
#include "registry.h"

#if !defined(EMBR_WITH_TREE_WALKER) && !defined(EMBR_WITH_VM)
#error "embr: no execution backend compiled in. define EMBR_WITH_TREE_WALKER and/or EMBR_WITH_VM"
#endif

namespace embr {

inline Value invoke(Interpreter& interp, const Value& callee,
                    const std::vector<Value>& args, const SourceRange& site = {}) {
    if (!callee.isCallable())
        raiseError("runtime", "value is not callable (got " + callee.typeName() + ")",
                   site, interp.sourceMap());

#if defined(EMBR_WITH_VM)
    const auto& c = callee.asCallable();
    if (c.isScript() && c.script.compiledChunk) {
        vm::VmRunner r(interp);
        return r.invoke(callee, args, site);
    }
#endif

    // native functions and tree-walked script functions go through the tree-walker's invoke
    // which already knows how to call a native directly without needing any AST/bytecode at all
#ifdef EMBR_WITH_TREE_WALKER
    Runner r(interp);
    return r.invoke(callee, args, site);
#elif defined(EMBR_WITH_VM)
    // vm only build: VmRunner::invoke() already handles natives itself
    vm::VmRunner r(interp);
    return r.invoke(callee, args, site);
#endif
}

} // namespace embr

#endif // EMBR_CORE_INVOKE_H
