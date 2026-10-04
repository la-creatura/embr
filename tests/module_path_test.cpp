// module search path: EMBR_PATH entries are searched by load_module() (see fileCandidates in core/module.h)
//
// the fixture directory is made under the CTest working directory, which is not on the default search path
// (scriptDir is empty for harness code), so the module is only found through EMBR_PATH. fixtures clean up after
// themselves, like fs_test.cpp and pkg_embr_test.cpp

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {
        { "EMBR_PATH: a module only present in an EMBR_PATH directory is found",
          "base = path_join(os_cwd(), \"mp_fixture_a\")\n"
          "fs_mkdir(path_join(base, \"lib\"), 1)\n"
          "write_file(path_join(base, \"lib\", \"mp_mod_a.embr\"), \"answer = 41 + 1\\n\")\n"
          "os_setenv(\"EMBR_PATH\", path_join(base, \"lib\"))\n"
          "m = load_module(\"mp_mod_a\")\nprint(m[\"answer\"])\n"
          "os_unsetenv(\"EMBR_PATH\")\n"
          "fs_remove(path_join(base, \"lib\", \"mp_mod_a.embr\"))\nfs_remove(path_join(base, \"lib\"))\nfs_remove(base)\n",
          "42\n" },

        { "EMBR_PATH: several entries (colon-separated, semicolon on windows) are searched in order, unset means not found",
          "base = path_join(os_cwd(), \"mp_fixture_b\")\n"
          "fs_mkdir(path_join(base, \"one\"), 1)\nfs_mkdir(path_join(base, \"two\"), 1)\n"
          "write_file(path_join(base, \"two\", \"mp_mod_b.embr\"), \"which = 2\\n\")\n"
          "os_setenv(\"EMBR_PATH\", path_join(base, \"one\") + \"" EMBR_TEST_PATHSEP "\" + path_join(base, \"two\"))\n"
          "print(load_module(\"mp_mod_b\")[\"which\"])\n"
          "os_unsetenv(\"EMBR_PATH\")\n"
          "try\nload_module(\"mp_mod_b\", 1)\nprint(\"unreachable\")\ncatch e\nprint(\"not found without EMBR_PATH\")\nend\n"
          "fs_remove(path_join(base, \"two\", \"mp_mod_b.embr\"))\nfs_remove(path_join(base, \"one\"))\n"
          "fs_remove(path_join(base, \"two\"))\nfs_remove(base)\n",
          "2\nnot found without EMBR_PATH\n" },
    };
    return embr_test::runSuite("module search path test suite", tests, {"embrlib", "os", "fs", "path"});
}
