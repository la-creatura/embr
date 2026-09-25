// unit tests for plugins/str/str.cpp (imported once by main() below)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "str_split: comma separated",
          "print(str_split(\"a,b,c\", \",\"))\n",
          "[\"a\", \"b\", \"c\"]\n" },

        { "str_split: empty separator splits into characters",
          "print(str_split(\"abc\", \"\"))\n",
          "[\"a\", \"b\", \"c\"]\n" },

        { "str_replace: replaces every occurrence",
          "print(str_replace(\"a-b-c\", \"-\", \"_\"))\n",
          "a_b_c\n" },

        { "str_strip: trims both ends",
          "print(str_strip(\"  hi  \"))\n",
          "hi\n" },

        { "str_lower / str_upper",
          "print(str_lower(\"HeLLo\"))\nprint(str_upper(\"HeLLo\"))\n",
          "hello\nHELLO\n" },

        { "str_find: returns index of first match",
          "print(str_find(\"hello\", \"ll\"))\n",
          "2\n" },

        { "str_find: -1 when not found",
          "print(str_find(\"hello\", \"zz\"))\n",
          "-1\n" },

        { "str_startswith / str_endswith",
          "print(str_startswith(\"hello\", \"he\"))\nprint(str_endswith(\"hello\", \"lo\"))\n"
          "print(str_startswith(\"hello\", \"lo\"))\n",
          "1\n1\n0\n" },

        { "str_isdigit / str_isalpha / str_isalnum",
          "print(str_isdigit(\"123\"))\nprint(str_isalpha(\"abc\"))\nprint(str_isalnum(\"abc123\"))\n"
          "print(str_isdigit(\"12a\"))\n",
          "1\n1\n1\n0\n" },

        { "str_count: counts non-overlapping occurrences",
          "print(str_count(\"banana\", \"a\"))\nprint(str_count(\"banana\", \"na\"))\n",
          "3\n2\n" },

        { "str_ord / str_chr round trip",
          "print(str_ord(\"A\"))\nprint(str_chr(65))\nprint(str_chr(str_ord(\"z\")))\n",
          "65\nA\nz\n" },

        { "str_repeat: repeats n times, 0 gives empty string",
          "print(str_repeat(\"ab\", 3))\nprint(str_repeat(\"x\", 0))\nprint(str_repeat(\"x\", 0) == \"\")\n",
          "ababab\n\n1\n" },

        { "str_reverse",
          "print(str_reverse(\"hello\"))\n",
          "olleh\n" },

        { "str_pad_left / str_pad_right",
          "print(str_pad_left(\"7\", 4, \"0\"))\nprint(str_pad_right(\"hi\", 5, \".\"))\n"
          "print(str_pad_left(\"longer\", 3))\n",  // already >= width: unchanged
          "0007\nhi...\nlonger\n" },

        { "str_ltrim / str_rtrim: one-sided trims",
          "print(str_ltrim(\"  hi  \") == \"hi  \")\nprint(str_rtrim(\"  hi  \") == \"  hi\")\n",
          "1\n1\n" },
    };
    return embr_test::runSuite("str plugin test suite", tests, {"str"});
}
