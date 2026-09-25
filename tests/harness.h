#ifndef EMBR_TEST_HARNESS_H
#define EMBR_TEST_HARNESS_H

// shared driver for embr's CTest-based unit test executables
//
// each plugin (and the core language) gets its own small executable built from a single .cpp that #includes this header
// calls runSuite() with its own list of Tests and the plugins it needs imported
// keeping each translation unit small means editing one plugin's tests only recompiles and relinks that one binary

#include <embr/embr.h>
#include <sstream>
#include <string>
#include <vector>

namespace embr_test {

// these suites exercise language/stdlib behavior so they run against whichever backend the build was configured with 
inline void runSourceAny(const std::string& src, embr::Interpreter& interp,
                         const std::string& filename) {
#ifdef EMBR_WITH_TREE_WALKER
    embr::runSource(src, interp, filename);
#elif defined(EMBR_WITH_VM)
    embr::vm::runSource(src, interp, filename);
#endif
}

struct Test {
    std::string name;
    std::string code;
    std::string expectedOutput;  // exact stdout content
    std::string stdinData = {};  // piped to input() calls
};

struct Result {
    std::string name;
    bool        passed = false;
    std::string actual;
    std::string expected;
    std::string errorMsg;
};

inline Result runTest(const Test& t, embr::Interpreter& interp) {
    std::ostringstream capOut;
    std::istringstream fakeIn(t.stdinData);
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());
    std::streambuf* origIn  = std::cin.rdbuf(fakeIn.rdbuf());

    Result r; r.name = t.name; r.expected = t.expectedOutput;
    try {
        runSourceAny(t.code, interp, t.name);
        r.actual = capOut.str();
        r.passed = (r.actual == r.expected);
        if (!r.passed) r.errorMsg = "output mismatch";
    } catch (const embr::EmbrError& e) {
        r.actual = capOut.str(); r.passed = false;
        r.errorMsg = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        r.actual = capOut.str(); r.passed = false;
        r.errorMsg = std::string("exception: ") + e.what();
    }
    std::cout.rdbuf(origOut);
    std::cin.rdbuf(origIn);
    return r;
}

inline void printResult(const Result& r) {
    static const char* G = "\033[32m";
    if (r.passed) {
        std::cout << G << " [+]" << embr::X << " " << r.name << "\n";
    } else {
        std::cout << embr::R << " [-]" << embr::X << " " << r.name << "\n";
        if (!r.errorMsg.empty())
            std::cout << embr::Y << "        error: " << embr::X << r.errorMsg << "\n";
        if (!r.expected.empty() || !r.actual.empty()) {
            std::cout << "        expected: " << embr::valueRepr(embr::Value(r.expected)) << "\n";
            std::cout << "        actual:   " << embr::valueRepr(embr::Value(r.actual))   << "\n";
        }
    }
}

// runs tests against a fresh Interpreter that has already import()ed each name in plugins
// individual Test::code strings should NOT re-import the same plugin
// or its "[runtime] loaded plugin: " banner will pollute the captured stdout an exact-match test compares against
// prints a summary and returns a process exit code (0 = all passed) that CTest reads as pass/fail
inline int runSuite(const std::string& suiteName,
                     const std::vector<Test>& tests,
                     const std::vector<std::string>& plugins = {}) {
#ifdef _WIN32
    embr::enableAnsi();
#endif
    embr::Interpreter interp;
    for (const auto& p : plugins)
        runSourceAny("import \"" + p + "\"", interp, "");

    int passed = 0, failed = 0;
    std::cout << "\n\033[1m " << suiteName << " \033[0m\n";
    for (const auto& t : tests) {
        Result r = runTest(t, interp);
        printResult(r);
        r.passed ? ++passed : ++failed;
    }

    std::cout << "\n"
              << "  " << passed << " passed, " << failed << " failed"
              << (failed ? "  \033[31mX\033[0m" : "  \033[32m√\033[0m") << "\n\n";
    return failed == 0 ? 0 : 1;
}

} // namespace embr_test

#endif // EMBR_TEST_HARNESS_H
