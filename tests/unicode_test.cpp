// unit tests for plugins/unicode/unicode.cpp (imported once by main() below)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "unicode_ord / unicode_chr round trip: ASCII",
          "print(unicode_ord(\"A\"))\nprint(unicode_chr(65))\n",
          "65\nA\n" },

        { "unicode_ord / unicode_chr round trip: multi-byte codepoint",
          "print(unicode_ord(\"\xc3\xa9\"))\nprint(unicode_chr(233) == \"\xc3\xa9\")\n",
          "233\n1\n" },

        { "unicode_ord / unicode_chr round trip: codepoint above 255",
          "print(unicode_chr(unicode_ord(\"\xe2\x82\xac\")) == \"\xe2\x82\xac\")\n",
          "1\n" },

        { "unicode_chr: rejects surrogate halves and out-of-range codepoints",
          "try\nunicode_chr(55296)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "unicode_len: counts codepoints, not bytes",
          "print(unicode_len(\"h\xc3\xa9llo\"))\n",
          "5\n" },

        { "unicode_valid: true for well-formed UTF-8",
          "print(unicode_valid(\"h\xc3\xa9llo\"))\n",
          "1\n" },

        { "unicode_valid: false for a stray continuation byte",
          "print(unicode_valid(\"\x80\"))\n",
          "0\n" },

        { "unicode_chars: splits by codepoint, not byte",
          "print(unicode_chars(\"h\xc3\xa9llo\"))\n",
          "[\"h\", \"\xc3\xa9\", \"l\", \"l\", \"o\"]\n" },

        { "unicode_isalpha / isdigit / isalnum on ASCII",
          "print(unicode_isalpha(\"abc\"))\nprint(unicode_isdigit(\"123\"))\nprint(unicode_isalnum(\"abc123\"))\n"
          "print(unicode_isdigit(\"12a\"))\n",
          "1\n1\n1\n0\n" },

        { "unicode_isalpha: false on empty string",
          "print(unicode_isalpha(\"\"))\n",
          "0\n" },
    };
    return embr_test::runSuite("unicode plugin test suite", tests, {"unicode"});
}
