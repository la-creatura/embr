// unit tests for plugins/embrmath/embrmath.cpp
//
// math_random()/math_randint() are tested through math_seed() for a repeatable sequence. the test only checks that
// seeding makes two runs agree, not any particular PRNG output

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "math_sqrt / math_pow",
          "print(math_sqrt(16))\nprint(math_pow(2, 10))\n",
          "4\n1024\n" },

        { "math_abs / math_floor / math_ceil / math_round",
          "print(math_abs(-3.5))\nprint(math_floor(3.7))\nprint(math_ceil(3.2))\nprint(math_round(3.5))\n",
          "3.5\n3\n4\n4\n" },

        { "math_min / math_max / math_clamp",
          "print(math_min(3, 7))\nprint(math_max(3, 7))\nprint(math_clamp(15, 0, 10))\nprint(math_clamp(-5, 0, 10))\nprint(math_clamp(5, 0, 10))\n",
          "3\n7\n10\n0\n5\n" },

        { "math_clamp: raises when lo > hi",
          "try\nmath_clamp(5, 10, 0)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "trig: sin/cos/tan of 0, atan2",
          "print(math_sin(0))\nprint(math_cos(0))\nprint(math_tan(0))\nprint(math_atan2(0, 1))\n",
          "0\n1\n0\n0\n" },

        { "log/exp: round trip",
          "print(math_log(math_exp(1)))\nprint(math_log2(8))\nprint(math_log10(1000))\n",
          "1\n3\n3\n" },

        { "PI / E constants",
          "print(PI > 3.14 and PI < 3.15)\nprint(E > 2.71 and E < 2.72)\n",
          "1\n1\n" },

        { "math_seed: same seed produces the same sequence",
          "math_seed(1234)\na = math_randint(1, 1000000)\nb = math_random()\n"
          "math_seed(1234)\nc = math_randint(1, 1000000)\nd = math_random()\n"
          "print(a == c)\nprint(b == d)\n",
          "1\n1\n" },

        { "math_randint: stays within bounds and is inclusive of both ends",
          "math_seed(7)\nok = 1\ni = 0\nwhile i < 200\nv = math_randint(1, 3)\nif v < 1 or v > 3\nok = 0\nend\ni = i + 1\nend\nprint(ok)\n",
          "1\n" },

        { "math_randint: raises when lo > hi",
          "try\nmath_randint(10, 0)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "math_randint / math_seed: huge or NaN-ish numbers raise a clear error instead of a wrapped-around bound",
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "try\n  math_randint(0, big)\n  print(\"unreachable\")\ncatch e\n  print(\"hi rejected\")\nend\n"
          "try\n  math_seed(big)\n  print(\"unreachable\")\ncatch e\n  print(\"seed rejected\")\nend\n"
          "x = math_randint(5, 5)\nprint(x)\n",
          "hi rejected\nseed rejected\n5\n" },
    };
    return embr_test::runSuite("embrmath plugin test suite", tests, {"embrmath"});
}
