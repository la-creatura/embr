// unit tests for modules/parallel.embr's worker_map() (a *script* module
// built on plugins/parallel's raw primitives -- see tests/parallel_test.cpp
// for those). copied to BIN_DIR/modules/parallel.embr by the parallel_module
// custom target (tests/CMakeLists.txt), the same way ffi.embr is.

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "worker_map: runs a function over every input and preserves order, across multiple workers",
          "write_file(\"test_parallel_embr_worker.embr\","
          " \"fn square(x)\\n    return x * x\\nend\\n\")\n"
          "r = worker_map(\"test_parallel_embr_worker.embr\", \"square\", [1,2,3,4,5,6,7], 3)\n"
          "print(r[0])\nprint(r[1])\nprint(r[2])\nprint(r[3])\nprint(r[4])\nprint(r[5])\nprint(r[6])\n",
          "1\n4\n9\n16\n25\n36\n49\n" },

        { "worker_map: n_workers greater than the input count is clamped down rather than erroring",
          "r = worker_map(\"test_parallel_embr_worker.embr\", \"square\", [2,3], 10)\n"
          "print(r[0])\nprint(r[1])\n",
          "4\n9\n" },

        { "worker_map: a single worker still works (n_workers=1)",
          "r = worker_map(\"test_parallel_embr_worker.embr\", \"square\", [5,6,7], 1)\n"
          "print(r[0])\nprint(r[1])\nprint(r[2])\n",
          "25\n36\n49\n" },

        { "worker_map: empty input returns an empty array without spawning anything",
          "r = worker_map(\"test_parallel_embr_worker.embr\", \"square\", [], 4)\n"
          "print(len(r))\n",
          "0\n" },

        { "worker_map: a task that raises aborts the call with a catchable error",
          "write_file(\"test_parallel_embr_failing_worker.embr\","
          " \"fn maybe_fail(x)\\n    if x == 3\\n        error(\\\"boom on 3\\\")\\n    end\\n    return x\\nend\\n\")\n"
          "try\n"
          "worker_map(\"test_parallel_embr_failing_worker.embr\", \"maybe_fail\", [1,2,3,4], 2)\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },
    };
    return embr_test::runSuite("parallel.embr module test suite", tests, {"parallel.embr", "embrlib"});
}
