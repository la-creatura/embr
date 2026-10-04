// unit tests for plugins/embrmeta/embrmeta.cpp (imported once by main() below,
// alongside embrlib for write_file()/import, used only to build a throwaway
// module fixture for the shadowing test, not itself under test here)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "meta_set: defines a new global variable, readable as an ordinary identifier",
          "meta_set(\"mm_x\", 42)\nprint(mm_x)\n",
          "42\n" },

        { "meta_set: overwrites an existing global",
          "meta_set(\"mm_y\", 1)\nmeta_set(\"mm_y\", 2)\nprint(mm_y)\n",
          "2\n" },

        { "meta_set: installs a callable that behaves like any other function",
          "meta_set(\"mm_add\", fn(a, b) return a + b end)\nprint(mm_add(3, 4))\n",
          "7\n" },

        { "meta_has: reflects presence and absence",
          "print(meta_has(\"mm_never_defined\"))\n"
          "meta_set(\"mm_present\", 1)\n"
          "print(meta_has(\"mm_present\"))\n",
          "0\n1\n" },

        { "meta_undef: removes a global; meta_has and later use both reflect it's gone",
          "meta_set(\"mm_temp\", 5)\n"
          "print(meta_has(\"mm_temp\"))\n"
          "print(meta_undef(\"mm_temp\"))\n"
          "print(meta_has(\"mm_temp\"))\n"
          "try\nprint(mm_temp)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "1\n1\n0\ncaught\n" },

        { "meta_undef: removing something already gone returns 0, doesn't raise",
          "print(meta_undef(\"mm_was_never_there\"))\n",
          "0\n" },

        { "meta_get: reads a global by name, raises on an undefined one",
          "meta_set(\"mm_g\", \"hello\")\n"
          "print(meta_get(\"mm_g\"))\n"
          "try\nmeta_get(\"mm_totally_missing\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "hello\ncaught\n" },

        // meta_get/meta_has read the native/global layer, the one bind() and a top-level assignment fall back to, not
        // whatever shadows the name in the caller's scope. the module is written with write_file(), so no fixture file is needed
        { "meta_get: sees the global binding even when a module shadows the same name locally",
          "meta_set(\"mm_shadow_test\", 111)\n"
          "write_file(\"test_embrmeta_shadow.embr\","
          " \"local mm_shadow_test = 222\\nmm_shadow_result = meta_get(\\\"mm_shadow_test\\\")\\n\")\n"
          "import \"test_embrmeta_shadow.embr\"\n"
          "print(mm_shadow_result)\n",
          "111\n" },
    };
    return embr_test::runSuite("embrmeta plugin test suite", tests, {"embrmeta", "embrlib"});
}
