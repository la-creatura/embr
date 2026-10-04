// unit tests for modules/ffi.embr, a script module (tests/CMakeLists.txt copies it to BIN_DIR/modules/ so
// `import "ffi.embr"` finds it)
//
// runs the real ffi_cdef() pipeline (parse the declaration, resolve embrtypes descriptors, ffi_prepare, meta_set)
// against libc functions, like tests/ffi_test.cpp does for the native plugin. only libc is used, so it runs
// wherever the ffi tests do

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "ffi_cdef: parses a simple declaration and installs a callable global (strlen)",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "ffi_cdef(lib, \"size_t strlen(const char *s);\")\n"
          "print(strlen(\"hello world\"))\n",
          "11\n" },

        { "ffi_cdef: a void return type maps to nil/no return value (srand)",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "ffi_cdef(lib, \"void srand(unsigned int seed);\")\n"
          "print(srand(42))\n",
          "0\n" },

#ifndef _WIN32
        { "ffi_cdef: variadic C functions work through the generated wrapper (snprintf)",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "ffi_cdef(lib, \"int snprintf(char *buf, size_t size, const char *fmt, ...);\")\n"
          "print(snprintf(ptr_null(), 0, \"n=%d\", 42))\n",
          "4\n" },

#else   // msvcrt.dll has no snprintf, _scprintf(fmt, ...) returns the same length
        { "ffi_cdef: variadic C functions work through the generated wrapper (_scprintf)",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "ffi_cdef(lib, \"int _scprintf(const char *fmt, ...);\")\n"
          "print(_scprintf(\"n=%d\", 42))\n",
          "4\n" },

#endif
        { "ffi_cdef: ffi_typedef registers a resolvable alias for a later declaration (atoi)",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "ffi_typedef(\"mystr\", type_str())\n"
          "ffi_cdef(lib, \"int atoi(mystr s);\")\n"
          "print(atoi(\"42\"))\n",
          "42\n" },

        { "ffi_cdef: raises a catchable error for an unregistered type instead of crashing",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "try\n"
          "ffi_cdef(lib, \"void some_fn(NotARealType x);\")\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "ffi_cdef: raises a catchable error for a declaration it can't parse",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "try\n"
          "ffi_cdef(lib, \"this is not a c declaration\")\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },
    };
    return embr_test::runSuite("ffi.embr module test suite", tests, {"ffi.embr"});
}
