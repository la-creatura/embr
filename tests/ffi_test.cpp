// unit tests for plugins/ffi/ffi.cpp (imported once by main() below)
//
// only covers what needs nothing but libc, which every Linux CI runner has
// the ffi_callback path is tested by hand, not here (it needs a real C API that calls back, like qsort)
//
// type descriptors (type_i32(), type_str(), struct_type(), nil for void) come from embrtypes, see ffi.cpp's header

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "ptr_null: formats as a null typed pointer",
          "print(ptr_null())\n",
          "<ptr 0x0>\n" },

        { "ffi_open + ffi_sym + ffi_call: strlen via libc",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "print(ffi_call(sym, type_i64(), [type_str()], [\"hello world\"]))\n",
          "11\n" },

        { "ffi_call: nil as ret means void",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"free\")\n"
          "print(ffi_call(sym, nil, [type_ptr()], [ptr_null()]))\n",
          "0\n" },

        { "buffer + ptr_read/ptr_write: out-param round trip, freed automatically",
          "r = buffer(8)\n"
          "print(ptr_read(r, type_i32()))\n"
          "ptr_write(r, type_i32(), 42)\n"
          "print(ptr_read(r, type_i32()))\n",
          "0\n42\n" },

        { "ffi_prepare + ffi_invoke: cached call matches ffi_call",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "p = ffi_prepare(sym, type_i64(), [type_str()])\n"
          "print(ffi_invoke(p, [\"abc\"]))\n"
          "print(ffi_invoke(p, [\"abcdef\"]))\n",
          "3\n6\n" },

        { "ffi_close: using a sym from a closed library raises instead of crashing",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "ffi_close(lib)\n"
          "try\n"
          "ffi_call(sym, type_i64(), [type_str()], [\"x\"])\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "ptr_offset: walks a buffer by byte count",
          "base = ptr_null()\n"
          "same = ptr_offset(base, 0)\n"
          "print(same)\n",
          "<ptr 0x0>\n" },

        { "struct_type: ptr_read/ptr_write round trip a struct buffer",
          "desc = struct_type([[\"x\", type_i32()], [\"y\", type_i32()]])\n"
          "buf = buffer(8)\n"
          "ptr_write(buf, desc, {\"x\": 3, \"y\": 4})\n"
          "v = ptr_read(buf, desc)\n"
          "print(v[\"x\"])\n"
          "print(v[\"y\"])\n",
          "3\n4\n" },

        { "struct_type: nested struct fields",
          "point = struct_type([[\"x\", type_i32()], [\"y\", type_i32()]])\n"
          "line = struct_type([[\"a\", point], [\"b\", point]])\n"
          "buf = buffer_pack(line, {\"a\": {\"x\": 1, \"y\": 2}, \"b\": {\"x\": 3, \"y\": 4}})\n"
          "v = ptr_read(buf, line)\n"
          "print(v[\"a\"][\"x\"])\n"
          "print(v[\"b\"][\"y\"])\n",
          "1\n4\n" },

        { "struct_type: struct returned by value from a real libc call (div_t div(int,int))",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"div\")\n"
          "div_t = struct_type([[\"quot\", type_i32()], [\"rem\", type_i32()]])\n"
          "r = ffi_call(sym, div_t, [type_i32(), type_i32()], [17, 5])\n"
          "print(r[\"quot\"])\n"
          "print(r[\"rem\"])\n",
          "3\n2\n" },

#ifndef _WIN32
        { "ffi_call_var: variadic call via snprintf(NULL, 0, ...) sizing",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"snprintf\")\n"
          "n = ffi_call_var(sym, type_i32(), [type_ptr(), type_u64(), type_str(), type_i32()],\n"
          "                 [ptr_null(), 0, \"n=%d\", 42], 3)\n"
          "print(n)\n",
          "4\n" },

        { "ffi_prepare_var + ffi_invoke: cached variadic call",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"snprintf\")\n"
          "p = ffi_prepare_var(sym, type_i32(), [type_ptr(), type_u64(), type_str(), type_i32()], 3)\n"
          "print(ffi_invoke(p, [ptr_null(), 0, \"n=%d\", 7]))\n",
          "3\n" },

#else   // msvcrt.dll has no snprintf, _scprintf(fmt, ...) returns the same length
        { "ffi_call_var: variadic call via _scprintf length",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"_scprintf\")\n"
          "n = ffi_call_var(sym, type_i32(), [type_str(), type_i32()], [\"n=%d\", 42], 1)\n"
          "print(n)\n",
          "4\n" },

        { "ffi_prepare_var + ffi_invoke: cached variadic call",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"_scprintf\")\n"
          "p = ffi_prepare_var(sym, type_i32(), [type_str(), type_i32()], 1)\n"
          "print(ffi_invoke(p, [\"n=%d\", 7]))\n",
          "3\n" },

#endif
        { "ffi_call: passing a type embrtypes can't marshal raises a clear error",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "try\n"
          "ffi_call(sym, type_arr(), [type_str()], [\"x\"])\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "ffi_callback: qsort with a script comparator sorts a C array",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "qsort = ffi_sym(lib, \"qsort\")\n"
          "arr = buffer(16)\n"
          "vals = [4, 1, 3, 2]\n"
          "i = 0\nwhile i < 4\nptr_write(ptr_offset(arr, i * 4), type_i32(), vals[i])\ni = i + 1\nend\n"
          "cmp = ffi_callback(fn(a, b)\nreturn ptr_read(a, type_i32()) - ptr_read(b, type_i32())\nend, type_i32(), [type_ptr(), type_ptr()])\n"
          "ffi_call(qsort, nil, [type_ptr(), type_u64(), type_u64(), type_ptr()], [arr, 4, 4, cmp])\n"
          "i = 0\nwhile i < 4\nprint(ptr_read(ptr_offset(arr, i * 4), type_i32()))\ni = i + 1\nend\n"
          "ffi_callback_free(cmp)\n",
          "1\n2\n3\n4\n" },

        { "ffi_callback: error inside a callback is re-raised after the C call, not thrown through C frames",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "qsort = ffi_sym(lib, \"qsort\")\n"
          "arr = buffer(16)\n"
          "cmp = ffi_callback(fn(a, b)\nerror(\"boom\")\nend, type_i32(), [type_ptr(), type_ptr()])\n"
          "try\nffi_call(qsort, nil, [type_ptr(), type_u64(), type_u64(), type_ptr()], [arr, 4, 4, cmp])\n"
          "print(\"unreachable\")\ncatch e\nprint(e[\"message\"])\nend\n"
          "print(\"still alive\")\n",
          "boom\nstill alive\n" },

        { "ptr_offset / ffi args: NaN and huge numbers raise instead of hitting UB",
          "p = buffer(8)\n"
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "try\nptr_offset(p, big)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n"
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "abs = ffi_sym(lib, \"abs\")\n"
          "try\nffi_call(abs, type_i32(), [type_i32()], [big])\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\ncaught\n" },

        { "ffi_call_var: negative nfixed raises",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"printf\")\n"
          "try\nffi_call_var(sym, type_i32(), [type_str()], [\"x\"], -1)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },
    };
    return embr_test::runSuite("ffi plugin test suite", tests, {"embrlib", "embrtypes", "ffi"});
}
