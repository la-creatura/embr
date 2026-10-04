// unit tests for plugins/random/random.cpp -- only properties that hold for
// every outcome (lengths, formats, permutations, seeded reproducibility)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {
        { "rand_bytes / rand_token / rand_uuid4: sizes and formats",
          "print(len(rand_bytes(0)))\nprint(len(rand_bytes(37)))\n"
          "print(len(rand_token()))\nprint(len(rand_token(4)))\n"
          "u = rand_uuid4()\nprint(len(u))\nprint(u[14])\nprint(u[8])\n"
          "print(rand_token(16) == rand_token(16))\n",
          "0\n37\n32\n8\n36\n4\n-\n0\n" },

        { "rand_bytes: bad counts raise",
          "try\nrand_bytes(-1)\nprint(\"unreachable\")\ncatch e\nprint(\"neg\")\nend\n"
          "try\nrand_bytes(1.5)\nprint(\"unreachable\")\ncatch e\nprint(\"float\")\nend\n"
          "try\nrand_bytes(100000000)\nprint(\"unreachable\")\ncatch e\nprint(\"huge\")\nend\n",
          "neg\nfloat\nhuge\n" },

        { "rand_seed: shuffle/choice/sample are reproducible",
          "rand_seed(42)\nx1 = rand_shuffle(range(0, 20))\nc1 = rand_choice(range(0, 100))\ns1 = rand_sample(range(0, 50), 5)\n"
          "rand_seed(42)\nx2 = rand_shuffle(range(0, 20))\nc2 = rand_choice(range(0, 100))\ns2 = rand_sample(range(0, 50), 5)\n"
          "print(join(for_each_do(x1, fn(v) return str(v) end), \",\") == join(for_each_do(x2, fn(v) return str(v) end), \",\"))\n"
          "print(c1 == c2)\nprint(join(for_each_do(s1, fn(v) return str(v) end), \",\") == join(for_each_do(s2, fn(v) return str(v) end), \",\"))\n",
          "1\n1\n1\n" },

        { "rand_shuffle: is a permutation and leaves the input alone",
          "src = range(0, 30)\nout = rand_shuffle(src)\nprint(len(out))\nprint(sum(out))\nprint(src[0])\nprint(src[29])\n",
          "30\n435\n0\n29\n" },

        { "rand_sample: distinct elements, bounds checked; rand_choice on empty raises",
          "s = rand_sample(range(0, 10), 10)\nprint(sum(s))\nprint(len(rand_sample([1,2], 0)))\n"
          "try\nrand_sample([1,2], 3)\nprint(\"unreachable\")\ncatch e\nprint(\"too many\")\nend\n"
          "try\nrand_choice([])\nprint(\"unreachable\")\ncatch e\nprint(\"empty\")\nend\n",
          "45\n0\ntoo many\nempty\n" },
    };
    return embr_test::runSuite("random plugin test suite", tests, {"embrlib", "random"});
}
