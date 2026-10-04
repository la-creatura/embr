// unit tests for VmRunner's C++ API (backends/vm.h), which no embr script or plugin reaches, so harness.h's
// script-driven tests can't cover it
//
// right now that's inject(). a native calling inject() twice before the first buffer was used up by a jump or call
// used to insert at the wrong position and replay already-run instructions, which hung the runner (probe() below
// triggers it). plugins/coroutine uses suspend() instead and never touches this path, so it needs its own test
//
// tests/CMakeLists.txt sets a TIMEOUT on this test, so a regression fails fast instead of hanging CI

#include "harness.h"

#ifdef EMBR_WITH_VM

static bool testDoubleInjectDoesNotReplayStaleInstructions() {
    const char* name = "inject(): a second call before the first injection is fully "
                        "consumed doesn't replay stale instructions";

    embr::Interpreter interp;
    embr::vm::VmRunner* runnerPtr = nullptr;

    // each call splices in a single, otherwise-inert NOP -- any observable
    // difference in behavior comes purely from inject()'s own bookkeeping,
    // not from anything the injected instruction itself does.
    interp.bind("probe", [&](const std::vector<embr::Value>&) -> embr::Value {
        runnerPtr->inject({ embr::vm::Instruction{embr::vm::Op::NOP} });
        return embr::Value(0.0);
    });

    // two probe() calls in a row, deliberately with no loop/jump between
    // them -- that's exactly the shape that leaves injBuf_ still active for
    // the second call. under the old bug, the second inject() replayed the
    // first injection's stale prefix (including the "print(\"b\")" and the
    // first probe() CALL itself), corrupting the stack and infinite-looping.
    std::string src =
        "print(\"a\")\n"
        "probe()\n"
        "print(\"b\")\n"
        "probe()\n"
        "print(\"c\")\n";

    std::ostringstream capOut;
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());

    bool passed = true;
    std::string errorMsg;
    try {
        auto prog = embr::vm::compileSource(src, interp, "<probe>");
        embr::vm::VmRunner runner(interp);
        runnerPtr = &runner;
        runner.run(prog);
    } catch (const embr::EmbrError& e) {
        passed = false;
        errorMsg = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        passed = false;
        errorMsg = std::string("exception: ") + e.what();
    }

    std::cout.rdbuf(origOut);
    std::string actual = capOut.str();
    if (passed && actual != "a\nb\nc\n") {
        passed = false;
        errorMsg = "output mismatch";
    }

    static const char* G = "\033[32m";
    static const char* R = "\033[31m";
    static const char* Y = "\033[33m";
    static const char* X = "\033[0m";
    if (passed) {
        std::cout << G << " [+]" << X << " " << name << "\n";
    } else {
        std::cout << R << " [-]" << X << " " << name << "\n";
        if (!errorMsg.empty()) std::cout << Y << "        error: " << X << errorMsg << "\n";
        std::cout << "        expected: " << embr::valueRepr(embr::Value(std::string("a\nb\nc\n"))) << "\n";
        std::cout << "        actual:   " << embr::valueRepr(embr::Value(actual)) << "\n";
    }
    return passed;
}

#endif // EMBR_WITH_VM

int main() {
#ifdef EMBR_WITH_VM
    std::cout << "\n\033[1m vm_internals \033[0m\n";
    bool ok = testDoubleInjectDoesNotReplayStaleInstructions();
    std::cout << "\n  " << (ok ? 1 : 0) << " passed, " << (ok ? 0 : 1) << " failed"
               << (ok ? "  \033[32m\xE2\x88\x9A\033[0m" : "  \033[31mX\033[0m") << "\n\n";
    return ok ? 0 : 1;
#else
    // inject() only exists under EMBR_WITH_VM -- nothing to test on a
    // tree-walker-only build.
    return 0;
#endif
}
