#include <embr/embr.h>
// #include "embr_vm.cpp"
#include <sstream>


namespace embr_test {

static const char* G = "\033[32m";

struct Test {
    std::string name;
    std::string code;
    std::string expectedOutput;  // exact stdout content
    std::string stdinData = {};  // piped to input() calls
};

struct Result {
    std::string name;
    bool        passed   = false;
    std::string actual;
    std::string expected;
    std::string errorMsg;
};

Result runTest(const Test& t, embr::Interpreter& interp) {
    std::ostringstream capOut;
    std::istringstream fakeIn(t.stdinData);
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());
    std::streambuf* origIn  = std::cin.rdbuf(fakeIn.rdbuf());

    Result r; r.name = t.name; r.expected = t.expectedOutput;
    try {
        embr::runSource(t.code, interp, t.name);
        r.actual = capOut.str();
        r.passed = (r.actual == r.expected);
        if (!r.passed) r.errorMsg = "output mismatch";
    } catch (const embr::EmbrError& e) {
        r.actual = capOut.str(); r.passed = false;
        r.errorMsg = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        r.actual = capOut.str(); r.passed = false;
        r.errorMsg = std::string("exception: ") + e.what();
    }
    std::cout.rdbuf(origOut);
    std::cin.rdbuf(origIn);
    return r;
}

void printResult(const Result& r) {
    if (r.passed) {
        std::cout << G << " [+]" << embr::X << " " << r.name << "\n";
    } else {
        std::cout << embr::R << " [-]" << embr::X << " " << r.name << "\n";
        if (!r.errorMsg.empty())
            std::cout << embr::Y << "        error: " << embr::X << r.errorMsg << "\n";
        // show first differing line for easy scanning
        if (!r.expected.empty() || !r.actual.empty()) {
            std::cout << "        expected: " << embr::valueRepr(embr::Value(r.expected)) << "\n";
            std::cout << "        actual:   " << embr::valueRepr(embr::Value(r.actual))   << "\n";
        }
    }
}

int runAll(const std::vector<Test>& tests) {
    int passed = 0, failed = 0;
    //std::cout << "\n\033[1m embr test suite \033[0m\n";
    auto interp = new embr::Interpreter();  // share context to avoid importing embrlib repeatedly to test its functions

    embr::runSource("import \"embrlib\"", *interp, "");

    for (const auto& t : tests) {
        Result r = runTest(t, *interp);
        printResult(r);
        r.passed ? ++passed : ++failed;
    }

    std::cout << "\n"
              << "  " << passed << " passed, " << failed << " failed"
              << (failed ? "  \033[31mX\033[0m" : "  \033[32m√\033[0m") << "\n\n";
    std::cout << "before delete\n";
    delete interp;
    std::cout << "after delete\n";
    return failed == 0 ? 0 : 1;
}

} // namespace embr_test


int main() {
    using embr_test::Test;
#ifdef _WIN32
    embr::enableAnsi();
#endif
    std::vector<Test> tests = {

        { "arithmetic: basic ops",
          "print(1+2)\nprint(10-3)\nprint(3*4)\nprint(10/4)\n",
          "3\n7\n12\n2.5\n" },

        { "arithmetic: integer display",
          "print(9.0)\nprint(1.0+2.0)\n",
          "9\n3\n" },



        { "string: concat",
          "print(\"hello \" + \"world\")\n",
          "hello world\n" },

        { "string: num coercion in +",
          "print(\"n=\" + 42)\n",
          "n=42\n" },

        { "string: index",
          "s = \"abc\"\nprint(s[1])\n",
          "b\n" },



        { "variables: basic",
          "x = 10\ny = x + 5\nprint(y)\n",
          "15\n" },

        { "variables: local shadows global",
          "x = 1\nlocal x = 99\nprint(x)\n",
          "99\n" },



        { "fn block: basic",
          "fn double(n)\nreturn n * 2\nend\nprint(double(7))\n",
          "14\n" },

        { "fn block: multiple params",
          "fn add(a, b)\nreturn a + b\nend\nprint(add(3, 4))\n",
          "7\n" },



        { "fn assign: single arg",
          "square(x) = x * x\nprint(square(5))\n",
          "25\n" },

        { "fn assign: zero args",
          "answer() = 42\nprint(answer())\n",
          "42\n" },

        { "fn assign: multi arg",
          "mul(a, b) = a * b\nprint(mul(3, 7))\n",
          "21\n" },

        { "fn assign: string result",
          "greet(n) = \"hello \" + n\nprint(greet(\"world\"))\n",
          "hello world\n" },

        { "fn assign: compose",
          "double(x) = x * 2\nquad(x) = double(double(x))\nprint(quad(3))\n",
          "12\n" },



        { "recursion: factorial",
          "fn fact(n)\nif n <= 1\nreturn 1\nend\nreturn n * fact(n-1)\nend\nprint(fact(5))\n",
          "120\n" },

        { "recursion: fibonacci",
          "fn fib(n)\nif n <= 1\nreturn n\nend\nreturn fib(n-1) + fib(n-2)\nend\nprint(fib(8))\n",
          "21\n" },



        { "if/else: true branch",
          "if 1 > 0\nprint(\"yes\")\nelse\nprint(\"no\")\nend\n",
          "yes\n" },

        { "if/else: false branch",
          "if 0\nprint(\"yes\")\nelse\nprint(\"no\")\nend\n",
          "no\n" },

        { "if: no else",
          "if 1\nprint(\"ok\")\nend\n",
          "ok\n" },



        { "while: sum 0..4",
          "i = 0\ns = 0\nwhile i < 5\ns = s + i\ni = i + 1\nend\nprint(s)\n",
          "10\n" },



        { "array: index and assign",
          "a = [10, 20, 30]\nprint(a[1])\na[1] = 99\nprint(a[1])\n",
          "20\n99\n" },

        { "array: multiple index and assign",
          "a = [[10, 20], [20, 30], [30, 40]]\nprint(a[1][1])\na[1][1] = 99\nprint(a[1][1])\n",
          "30\n99\n" },

        { "array: len / push / pop",
          "a = [1,2]\na = push(a, 3)\nprint(len(a))\na = pop(a)\nprint(len(a))\n",
          "3\n2\n" },



        { "map: set and get",
          "m = {x: 10}\nprint(m[\"x\"])\nm[\"y\"] = 20\nprint(m[\"y\"])\n",
          "10\n20\n" },
        { "map: multiple index and assign",
          "m = {x: 10}\nprint(m[\"x\"])\nm[\"y\"] = 20\nprint(m[\"y\"])\n",
          "10\n20\n" },



        { "type conversions",
          "print(num(\"3.14\"))\nprint(str(42))\nprint(type(1))\nprint(type(\"a\"))\n",
          "3.14\n42\nnumber\nstring\n" },



        { "boolean: not",
          "print(!0)\nprint(!1)\nprint(!\"\")\nprint(!\"hi\")\n",
          "1\n0\n1\n0\n" },



        { "globals: includes vars",
          "x = 42\ng = globals()\nprint(g[\"x\"])\n",
          "42\n" },

        { "globals: includes fn",
          "foo() = 1\ng = globals()\nprint(g[\"foo\"])\n",
          "<fn foo>\n" },



        { "hof: iif true",
          "if_do(1, fn() print(\"y\") end, fn() print(\"n\") end)\n",
          "y\n" },

        { "hof: iif false",
          "if_do(0, fn() print(\"y\") end, fn() print(\"n\") end)\n",
          "n\n" },

        { "hof: iloop",
          "i = 0\nwhile_do(fn() return i < 3 end, fn() print(i)\ni = i + 1\nend)\n",
          "0\n1\n2\n" },

        { "hof: call",
          "double(x) = x * 2\nprint(call(double, [7]))\n",
          "14\n" },



        { "modulo: basic",
          "print(10 % 3)\nprint(7 % 7)\nprint(1 % 5)\n",
          "1\n0\n1\n" },

        { "modulo: fizzbuzz snippet",
          "i = 1\nwhile i <= 6\n"
          "  if i % 3 == 0\n    print(\"fizz\")\n"
          "  else\n    print(i)\n  end\n"
          "  i += 1\nend\n",
          "1\n2\nfizz\n4\n5\nfizz\n" },



        { "compound: +=",
          "x = 5\nx += 3\nprint(x)\n",
          "8\n" },

        { "compound: -=",
          "x = 10\nx -= 4\nprint(x)\n",
          "6\n" },

        { "compound: *=",
          "x = 3\nx *= 7\nprint(x)\n",
          "21\n" },

        { "compound: /=",
          "x = 20\nx /= 4\nprint(x)\n",
          "5\n" },

        { "compound: loop counter",
          "i = 0\nwhile i < 4\n  i += 1\nend\nprint(i)\n",
          "4\n" },

        
        { "and: both true",
          "print(1 and 1)\n",
          "1\n" },

        { "and: left false short-circuits",
          "print(0 and 1)\nprint(0 and 0)\n",
          "0\n0\n" },

        { "or: left true short-circuits",
          "print(1 or 0)\nprint(1 or 1)\n",
          "1\n1\n" },

        { "or: both false",
          "print(0 or 0)\n",
          "0\n" },

        { "and/or precedence: and binds tighter than or",
          "print(0 and 1 or 1)\n",
          "1\n" },

        { "and/or: used in if condition",
          "x = 5\nif x > 3 and x < 10\n  print(\"in range\")\nend\n",
          "in range\n" },



        { "multi-statement source block",
          "fn greet(n)\nprint(\"hello \" + n)\nend\ngreet(\"world\")\n",
          "hello world\n" },


        
        { "did-you-mean: typo in fn name",
          "x = [1,2,3]\nlen(x)\nprint(\"ok\")\n",
          "ok\n" },

        { "did-you-mean: typo recovered after",
          "pint(\"hello\")\nprint(\"after\")\n",
          "after\n" },



        { "fn_name: native returns plain name",
          "print(fn_name(len))\n",
          "len\n" },

        { "fn_name: script fn includes provenance suffix",
          // fn_name returns  name@filename:line  for script fns
          "foo() = 1\n"
          "n = fn_name(foo)\n"
          "if n == \"foo\"\n"
          "  print(\"no provenance\")\n"
          "else\n"
          "  print(\"has provenance\")\n"
          "end\n",
          "has provenance\n" },

        { "fn_arity: script fn and native",
          "foo(a, b) = a + b\nprint(fn_arity(foo))\nprint(fn_arity(print))\n",
          "2\n0\n" },

        { "fn_sig: unannotated",
          "add(x, y) = x + y\nprint(fn_sig(add))\n",
          "add(x, y)\n" },

        { "fn_sig: annotated params and return",
          "double(x: num) -> num = x * 2\nprint(fn_sig(double))\n",
          "double(x: number) -> number\n" },
        { "is_native",
          "print(is_native(print))\nbaz() = 0\nprint(is_native(baz))\n",
          "1\n0\n" },



        { "annotation: param type enforced",
          "double(x: num) -> num = x * 2\ndouble(\"bad\")\nprint(\"ok\")\n",
          "ok\n" },

        { "annotation: correct types pass through",
          "greet(name: str) -> str = \"hi \" + name\nprint(greet(\"world\"))\n",
          "hi world\n" },

        { "annotation: return type mismatch to stderr, recovery continues",
          "fn bad() -> num\nreturn \"oops\"\nend\nbad()\nprint(\"ok\")\n",
          "ok\n" },

        { "assert: passes",
          "assert(1)\nprint(\"ok\")\n",
          "ok\n" },



        { "error recovery: undefined var",
          "print(bad_undefined_name)\nprint(\"after\")\n",
          "after\n" },



        { "closure: lambda captures enclosing local",
          "fn make_adder(n)\n"
          "  return fn(x) return x + n end\n"
          "end\n"
          "add5 = make_adder(5)\n"
          "print(add5(3))\n"
          "print(add5(10))\n",
          "8\n15\n" },

        { "closure: named fn inside fn captures locals",
          "fn make_counter(start)\n"
          "  local count = start\n"
          "  fn get()\n"
          "    return count\n"
          "  end\n"
          "  return get\n"
          "end\n"
          "c = make_counter(7)\n"
          "print(c())\n",
          "7\n" },

        { "closure: multiple closures from same call are independent",
          "fn make_adder(n)\n"
          "  return fn(x) return x + n end\n"
          "end\n"
          "add1 = make_adder(1)\n"
          "add100 = make_adder(100)\n"
          "print(add1(0))\n"
          "print(add100(0))\n",
          "1\n100\n" },

        { "closure: lambda in if captures outer local",
          "fn check(x)\n"
          "  local msg = \"big\"\n"
          "  if x < 5\n"
          "    msg = \"small\"\n"
          "  end\n"
          "  return fn() return msg end\n"
          "end\n"
          "f3 = check(3)\n"
          "f9 = check(9)\n"
          "print(f3())\n"
          "print(f9())\n",
          "small\nbig\n" },

        { "closure: deeply nested capture",
          "fn outer(a)\n"
          "  fn inner(b)\n"
          "    return fn(c) return a + b + c end\n"
          "  end\n"
          "  return inner\n"
          "end\n"
          "mid = outer(1)\n"
          "f = mid(2)\n"
          "print(f(3))\n",
          "6\n" },
    };
    int ret = embr_test::runAll(tests);
    return ret;
}
