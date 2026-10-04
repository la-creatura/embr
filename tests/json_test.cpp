// unit tests for plugins/json/json.cpp (imported once by main() below)
//
// json_stringify tests here stick to single-key maps or arrays
// since map key order comes from an unordered_map and isn't guaranteed
// a multi-key object is instead round-tripped through json_parse and checked by index

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "json_parse: array of numbers",
          "v = json_parse(\"[1,2,3]\")\nprint(v[0])\nprint(v[1])\nprint(v[2])\n",
          "1\n2\n3\n" },

        { "json_parse: object field access",
          "v = json_parse(\"{\\\"a\\\": 1, \\\"b\\\": [2,3]}\")\nprint(v[\"a\"])\nprint(v[\"b\"][1])\n",
          "1\n3\n" },

        { "json_parse: integers stay exact past double precision",
          "v = json_parse(\"9007199254740993\")\nprint(v)\n",
          "9007199254740993\n" },

        { "json_parse: a decimal point makes it a float",
          "v = json_parse(\"5.5\")\nprint(v)\n",
          "5.5\n" },

        { "json_parse: true/false truthy, null falsy (see embrtypes for real bool/nil values)",
          "print(!json_parse(\"true\"))\nprint(!json_parse(\"false\"))\nprint(!json_parse(\"null\"))\n",
          "0\n1\n1\n" },

        { "json_parse + json_stringify: true/false/null all round-trip exactly",
          "print(json_stringify(json_parse(\"true\")))\n"
          "print(json_stringify(json_parse(\"false\")))\n"
          "print(json_stringify(json_parse(\"null\")))\n",
          "true\nfalse\nnull\n" },

        { "json_parse: nested object round trip via index",
          "v = json_parse(\"{\\\"outer\\\": {\\\"inner\\\": 42}}\")\nprint(v[\"outer\"][\"inner\"])\n",
          "42\n" },

        { "json_valid: rejects malformed input, accepts well-formed",
          "print(json_valid(\"{not json\"))\nprint(json_valid(\"{\\\"ok\\\":1}\"))\n",
          "0\n1\n" },

        { "json_stringify: array",
          "print(json_stringify([1,2,3]))\n",
          "[1,2,3]\n" },

        { "json_stringify: single-key object",
          "print(json_stringify({\"x\": 1}))\n",
          "{\"x\":1}\n" },

        { "json_stringify: nested array",
          "print(json_stringify([[1,2],[3,4]]))\n",
          "[[1,2],[3,4]]\n" },

        { "json_stringify: integer stays exact past double precision",
          "print(json_stringify(9007199254740993))\n",
          "9007199254740993\n" },

        { "json_stringify . json_parse round trip preserves structure",
          "v = {\"a\": 1, \"b\": 2, \"c\": [1,2,3]}\n"
          "v2 = json_parse(json_stringify(v))\n"
          "print(v2[\"a\"])\nprint(v2[\"b\"])\nprint(v2[\"c\"][2])\n",
          "1\n2\n3\n" },

        { "json_stringify: pretty printing adds newlines and indent",
          "print(json_stringify([1,2], 2))\n",
          "[\n  1,\n  2\n]\n" },

        { "json_parse: syntax error reports line and column",
          "try\njson_parse(\"{\\n  \\\"a\\\": ?\\n}\")\ncatch e\nprint(e[\"message\"])\nend\n",
          "unexpected character '?' at line 2, column 8 (position 9)\n" },

        { "json_parse: absurdly deep nesting raises instead of crashing",
          "s = \"\"\ni = 0\nwhile i < 5000\ns = s + \"[\"\ni = i + 1\nend\n"
          "try\njson_parse(s)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "json_parse: surrogate pair decodes, lone surrogates raise",
          "print(json_parse(\"\\\"\\\\ud83d\\\\ude00\\\"\"))\n"
          "try\njson_parse(\"\\\"\\\\ud83d\\\"\")\ncatch e\nprint(\"caught\")\nend\n"
          "try\njson_parse(\"\\\"\\\\ude00\\\"\")\ncatch e\nprint(\"caught\")\nend\n",
          "\xF0\x9F\x98\x80\ncaught\ncaught\n" },

        { "json_parse: lenient by default (raw invalid bytes pass through), {\"strict_utf8\": 1} rejects them",
          "raw = \"\\\"a\\xffb\\\"\"\n"
          "print(len(json_parse(raw)))\nprint(json_valid(raw))\nprint(json_valid(raw, {\"strict_utf8\": 1}))\n"
          "try\n  json_parse(raw, {\"strict_utf8\": 1})\n  print(\"unreachable\")\ncatch e\n  print(e[\"message\"])\nend\n",
          "3\n1\n0\ninvalid UTF-8 in string at line 1, column 3 (position 2)\n" },

        { "json_parse strict_utf8: accepts valid multi-byte text, rejects overlong, surrogate-half, truncated sequences and raw control characters",
          "ok = \"\\\"caf\\u00e9 \\u20ac \\u{1F600}\\\"\"\nprint(len(json_parse(ok, {\"strict_utf8\": 1})))\n"
          "print(json_valid(\"\\\"\\xc0\\x80\\\"\", {\"strict_utf8\": 1}))\n"
          "print(json_valid(\"\\\"\\xed\\xa0\\x80\\\"\", {\"strict_utf8\": 1}))\n"
          "print(json_valid(\"\\\"\\xe2\\x82\\\"\", {\"strict_utf8\": 1}))\n"
          "print(json_valid(\"\\\"a\\nb\\\"\"))\nprint(json_valid(\"\\\"a\\nb\\\"\", {\"strict_utf8\": 1}))\n",
          "14\n0\n0\n0\n1\n0\n" },

        { "json_stringify: a string with invalid UTF-8 raises by default ({\"lossy\": 1} substitutes U+FFFD); valid UTF-8 and the indent cap are unaffected",
          "bad = \"a\\xffb\"\n"
          "try\n  json_stringify(bad)\n  print(\"unreachable\")\ncatch e\n  print(\"invalid utf-8 rejected\")\nend\n"
          "m = {}\nm[bad] = 1\ntry\n  json_stringify(m)\n  print(\"unreachable\")\ncatch e\n  print(\"key rejected too\")\nend\n"
          "print(len(json_stringify(bad, 0, {\"lossy\": 1})))\n"
          "print(len(json_stringify(\"\\u00e9\")))\n"
          "print(len(json_stringify([1], 1000000)) < 100)\n",
          "invalid utf-8 rejected\nkey rejected too\n7\n4\n1\n" },
    };
    return embr_test::runSuite("json plugin test suite", tests, {"embrlib", "json"});
}
