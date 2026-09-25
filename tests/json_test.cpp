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

        { "json_parse: true/false/null all map to falsy-consistent numbers",
          "print(json_parse(\"true\"))\nprint(json_parse(\"false\"))\nprint(json_parse(\"null\"))\n",
          "1\n0\n0\n" },

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
    };
    return embr_test::runSuite("json plugin test suite", tests, {"json"});
}
