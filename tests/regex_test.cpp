// unit tests for plugins/regex/regex.cpp (imported once by main() below)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "re_test: matches anywhere in the string",
          "print(re_test(\"hello world\", \"wor.d\"))\nprint(re_test(\"hello world\", \"xyz\"))\n",
          "1\n0\n" },

        { "re_match: whole match plus capture groups",
          "m = re_match(\"2024-01-02\", \"(\\\\d+)-(\\\\d+)-(\\\\d+)\")\n"
          "print(m[0])\nprint(m[1])\nprint(m[2])\nprint(m[3])\n",
          "2024-01-02\n2024\n01\n02\n" },

        { "re_match: empty array when there's no match",
          "print(len(re_match(\"abc\", \"[0-9]+\")))\n",
          "0\n" },

        { "re_find_all: every non-overlapping match, each with its own captures",
          "r = re_find_all(\"a1 b22 c333\", \"[a-z](\\\\d+)\")\n"
          "print(len(r))\n"
          "print(r[0][0])\nprint(r[0][1])\n"
          "print(r[1][0])\nprint(r[1][1])\n"
          "print(r[2][0])\nprint(r[2][1])\n",
          "3\na1\n1\nb22\n22\nc333\n333\n" },

        { "re_find_all: empty array when there's no match",
          "print(len(re_find_all(\"abc\", \"[0-9]+\")))\n",
          "0\n" },

        { "re_replace: replaces every match",
          "print(re_replace(\"2024-01-02\", \"-\", \"/\"))\n",
          "2024/01/02\n" },

        { "re_replace: replacement can reference capture groups",
          "print(re_replace(\"2024-01-02\", \"(\\\\d+)-(\\\\d+)-(\\\\d+)\", \"$3/$2/$1\"))\n",
          "02/01/2024\n" },

        { "re_test/re_match/re_find_all/re_replace: raise on a malformed pattern instead of crashing",
          "try\nre_test(\"x\", \"(unclosed\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught 1\")\nend\n"
          "try\nre_match(\"x\", \"(unclosed\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught 2\")\nend\n"
          "try\nre_find_all(\"x\", \"(unclosed\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught 3\")\nend\n"
          "try\nre_replace(\"x\", \"(unclosed\", \"y\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught 4\")\nend\n",
          "caught 1\ncaught 2\ncaught 3\ncaught 4\n" },

        // ---- the reason the engine was replaced: std::regex overflowed the native stack at ~30,000 bytes
        // and backtracked exponentially on nested quantifiers
        { "regex: long subjects no longer crash the process (100k bytes, simple, alternation and counted patterns)",
          "big = str_repeat(\"ab\", 50000)\n"
          "print(re_test(big, \"[ab]*c\"))\nprint(re_test(big, \"(a|b)*$\"))\n"
          "print(len(re_match(big, \"^(?:ab)*\")[0]))\n"
          "huge = str_repeat(\"a\", 100000)\nprint(re_test(huge, \"a*b\"))\nprint(len(re_find_all(huge, \"a{50}\")))\n",
          "0\n1\n100000\n0\n2000\n" },

        { "regex: catastrophic-backtracking patterns run in linear time (ReDoS)",
          "s = str_repeat(\"a\", 5000) + \"b\"\n"
          "print(re_test(s, \"(a+)+$\"))\nprint(re_test(s, \"(a|aa)+$\"))\nprint(re_test(s, \"(a*)*c\"))\n"
          "print(re_test(s, \"^(\\\\w+\\\\s?)*$\"))\n",
          "0\n0\n0\n1\n" },

        { "regex: replacement syntax ($&, $$, $`, $', two-digit groups, a missing group stays literal)",
          "print(re_replace(\"abc\", \"b\", \"[$&]\"))\nprint(re_replace(\"abc\", \"b\", \"$$\"))\n"
          "print(re_replace(\"abc\", \"b\", \"<$`|$'>\"))\nprint(re_replace(\"abc\", \"(b)\", \"$1$1\"))\n"
          "print(re_replace(\"abc\", \"(b)\", \"$2\"))\nprint(re_replace(\"abc\", \"x*\", \"-\"))\n",
          "a[b]c\na$c\na<a|c>c\nabbc\na$2c\n-a-b-c-\n" },

        { "regex: groups that did not take part are empty strings; anchors, word boundaries, classes, counted and lazy quantifiers",
          "m = re_match(\"ac\", \"(a)(b)?(c)\")\nprint(len(m))\nprint(m[2] == \"\")\nprint(m[3])\n"
          "print(re_test(\"foo bar\", \"\\\\bbar\\\\b\"))\nprint(re_test(\"foobar\", \"\\\\bbar\\\\b\"))\n"
          "print(re_match(\"aaaa\", \"a{2,3}\")[0])\nprint(re_match(\"aaaa\", \"a{2,3}?\")[0])\n"
          "print(re_match(\"<a><b>\", \"<.+?>\")[0])\nprint(re_match(\"x-y_z\", \"[\\\\w-]+\")[0])\n"
          "print(re_test(\"abc\", \"^abc$\"))\nprint(re_test(\"abc\\n\", \"^abc$\"))\n",
          "4\n1\nc\n1\n0\naaa\naa\n<a>\nx-y_z\n1\n0\n" },

        { "regex: backreferences and lookahead still work via the std::regex fallback, with a size limit instead of a crash",
          "print(re_test(\"abab\", \"(ab)\\\\1\"))\nprint(re_match(\"foobar\", \"foo(?=bar)\")[0])\n"
          "print(re_test(\"abc\", \"(a)\\\\1\"))\n"
          "try\n  re_test(str_repeat(\"a\", 20000), \"(a)\\\\1\")\n  print(\"unreachable\")\ncatch e\n  print(\"refused long subject\")\nend\n",
          "1\nfoo\n0\nrefused long subject\n" },

        { "regex: hostile patterns are errors, not crashes (deep nesting, huge repeat counts, unbalanced)",
          "deep = str_repeat(\"(\", 5000) + \"a\" + str_repeat(\")\", 5000)\n"
          "try\n  re_test(\"a\", deep)\n  print(\"unreachable\")\ncatch e\n  print(\"deep pattern rejected\")\nend\n"
          "try\n  re_test(\"a\", \"a{99999}\")\n  print(\"unreachable\")\ncatch e\n  print(\"huge count rejected\")\nend\n"
          "try\n  re_test(\"a\", \"a)\")\n  print(\"unreachable\")\ncatch e\n  print(\"unbalanced rejected\")\nend\n"
          "try\n  re_test(\"a\", \"*a\")\n  print(\"unreachable\")\ncatch e\n  print(\"nothing to repeat rejected\")\nend\n"
          "try\n  re_test(\"a\", \"[a\")\n  print(\"unreachable\")\ncatch e\n  print(\"unclosed class rejected\")\nend\n",
          "deep pattern rejected\nhuge count rejected\nunbalanced rejected\nnothing to repeat rejected\nunclosed class rejected\n" },

        { "re_replace: a result that would be huge raises instead of running for ages",
          "s = str_repeat(\"x\", 100000)\ntry\n  re_replace(s, \"\", s)\n  print(\"unreachable\")\ncatch e\n  print(\"capped\")\nend\n",
          "capped\n" },
    };
    return embr_test::runSuite("regex plugin test suite", tests, {"embrlib", "str", "regex"});
}
