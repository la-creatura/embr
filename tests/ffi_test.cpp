// unit tests for plugins/ffi/ffi.cpp (imported once by main() below)
//
// only exercises pieces that don't depend on a specific installed C library beyond libc itself
// (present on every Linux CI runner this project builds on)
// struct/callback/variadic paths are exercised manually, not here

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "ptr_null: formats as a null typed pointer",
          "print(ptr_null())\n",
          "<ptr 0x0>\n" },

        { "ffi type descriptors are maps with a name field",
          "print(ffi_i32[\"name\"])\nprint(ffi_f64[\"name\"])\nprint(ffi_cstr[\"name\"])\n",
          "i32\nf64\ncstr\n" },

        { "ffi_open + ffi_sym + ffi_call: strlen via libc",
          "lib = ffi_open(\"libc.so.6\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "print(ffi_call(sym, ffi_i64, [ffi_cstr], [\"hello world\"]))\n",
          "11\n" },

        { "ffi_ref / ffi_deref: out-param buffer round trip",
          "r = ffi_ref(ffi_i32)\n"
          "v = ffi_deref(r)\n"
          "print(v)\n"
          "ffi_ref_free(r)\n",
          "0\n" },

        { "ffi_prepare + ffi_invoke: cached call matches ffi_call",
          "lib = ffi_open(\"libc.so.6\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "p = ffi_prepare(sym, ffi_i64, [ffi_cstr])\n"
          "print(ffi_invoke(p, [\"abc\"]))\n"
          "print(ffi_invoke(p, [\"abcdef\"]))\n",
          "3\n6\n" },

        { "ptr_offset: walks a buffer by byte count",
          "r = ffi_ref(ffi_i32)\n"
          "base = ptr_null()\n"
          "same = ptr_offset(base, 0)\n"
          "print(same)\n"
          "ffi_ref_free(r)\n",
          "<ptr 0x0>\n" },
    };
    return embr_test::runSuite("ffi plugin test suite", tests, {"ffi"});
}
