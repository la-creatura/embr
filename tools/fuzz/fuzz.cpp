// fuzz.cpp
// a dependency-free fuzz driver for embr (works with gcc, no libFuzzer needed)
//
//   frontend  mutated source bytes -> Lexer -> Parser -> VM compiler. arbitrary bytes may only raise an EmbrError:
//             never crash, hang, leak another exception type, or overflow the stack
//   fmt       mutated source bytes -> the `embr fmt` formatter. it must refuse (the source doesn't lex) or return
//             text that formats to itself. FormatError, or a second pass that changes anything, is a finding
//   json      (only in the -DFUZZ_JSON build) mutated bytes -> json_parse, plus a round trip: whatever json_parse
//             accepts, json_stringify -> json_parse must accept too. json_stringify may refuse invalid UTF-8,
//             then the {"lossy": 1} form must round-trip
//   <plugin>  (only in a -DFUZZ_PLUGIN_SRC/-DFUZZ_PLUGIN_NAME build: csv encoding regex str unicode time vec)
//             mutated bytes -> that plugin's functions, called from a small embr script. the input is split at the
//             first \x01 into two strings, and a number is derived from the bytes. a script may raise (fine).
//             crashes, hangs, sanitizer reports and a printed ROUNDTRIP-FAIL (csv, encoding, vec) are findings
//   diff      small generated valid programs run on the tree-walker and the VM in separate Interpreters.
//             stdout and "did it error" must match
//
// build + run with tools/fuzz/run.sh (scratch build dir, never touches bin/linux/).
//
//   embr_fuzz frontend [-n N] [-seed S] [-t SECONDS] [corpus files...]
//   embr_fuzz diff     [-n N] [-seed S]
//   embr_fuzz replay FILE        (re-run one saved crash-*/hang-* input)
//
// every failure saves its input as crash-*.bin / hang-*.bin / diff-*.embr in the cwd.
// also exports LLVMFuzzerTestOneInput so a clang build can use real libFuzzer:
//   clang++ -fsanitize=fuzzer,address,undefined -DFUZZ_LIBFUZZER ...

#include <embr/embr.h>
#include "../../src/fmt.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <unistd.h>

#ifdef FUZZ_JSON
#  include "../../plugins/json/json.cpp"
#endif
#ifdef FUZZ_PLUGIN_SRC
#  include FUZZ_PLUGIN_SRC
#endif

using namespace embr;

// ---- crash / hang capture -------------------------------------------------
static std::string g_current;      // the input being run right now
static const char* g_kind = "crash";
static int g_saveCounter = 0;

static void saveInput(const char* kind, const std::string& data, const char* ext = "bin") {
    char name[128];
    std::snprintf(name, sizeof name, "%s-%d-%d.%s", kind, (int)getpid(), g_saveCounter++, ext);
    std::ofstream(name, std::ios::binary) << data;
    std::fprintf(stderr, "fuzz: saved %s (%zu bytes)\n", name, data.size());
}
static void onFatal(int sig) {
    saveInput(sig == SIGALRM ? "hang" : "crash", g_current);
    std::fprintf(stderr, "fuzz: fatal signal %d while running the saved input\n", sig);
    _exit(sig == SIGALRM ? 124 : 134);
}
// ASan and UBSan terminate through their own exit path, not abort(), so SIGABRT above never fires for
// their reports; the sanitizer runtime calls this death callback right after printing one instead
extern "C" void __sanitizer_set_death_callback(void (*)(void));
static void onSanitizerDeath() { saveInput("crash", g_current); }

static void installHandlers() {
    __sanitizer_set_death_callback(onSanitizerDeath);
    for (int s : {SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL, SIGALRM}) std::signal(s, onFatal);
}

// ---- output capture -------------------------------------------------------
struct Capture {
    std::ostringstream out, err;
    std::streambuf *so, *se;
    Capture()  { so = std::cout.rdbuf(out.rdbuf()); se = std::cerr.rdbuf(err.rdbuf()); }
    ~Capture() { std::cout.rdbuf(so); std::cerr.rdbuf(se); }
};

// ---- frontend target ------------------------------------------------------
// returns false on a contract violation (unexpected exception type)
static bool fuzzFrontend(const std::string& src) {
    Capture cap;
    Interpreter interp;
    try {
        vm::compileSource(src, interp, "<fuzz>");
    } catch (const EmbrError&) {
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: non-EmbrError exception: %s\n", e.what());
        return false;
    }
    return true;
}

static bool fuzzFmt(const std::string& src) {
    try {
        std::string once = embr_fmt::formatSource(src);
        if (embr_fmt::formatSource(once) != once) { std::fprintf(stderr, "fuzz: fmt is not idempotent\n"); return false; }
    } catch (const EmbrError&) {
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: fmt: %s\n", e.what());
        return false;
    }
    return true;
}

#ifdef FUZZ_JSON
static bool fuzzJson(const std::string& src) {
    Capture cap;
    Interpreter interp;
    embr_register(&interp);
    interp.bind("fuzz_input", [&src](const std::vector<Value>&) -> Value { return Value(src); });
    const char* prog =
        "try\n"
        "  v = json_parse(fuzz_input())\n"
        "  try\n"
        "    v2 = json_parse(json_stringify(v))\n"
        "  catch e\n"
        // json_stringify refuses a string that is not valid UTF-8 (documented), and json_parse lets such bytes
        // through, so the strict form may fail. the lossy form has to round-trip then
        "    try\n"
        "      v2 = json_parse(json_stringify(v, 0, {\"lossy\": 1}))\n"
        "    catch e2\n"
        "      print(\"ROUNDTRIP-FAIL\")\n"
        "    end\n"
        "  end\n"
        "catch e\n"
        "end\n";
    try { runSource(prog, interp, "<fuzz>"); }
    catch (const std::exception& e) { std::fprintf(stderr, "fuzz: exception %s\n", e.what()); return false; }
    return cap.out.str().find("ROUNDTRIP-FAIL") == std::string::npos;
}
#endif


#ifdef FUZZ_PLUGIN_SRC
// ---- plugin targets -------------------------------------------------------
// every statement runs inside its own try, so one raising call doesn't hide the next one. a, b are the two
// input strings, n is a number picked from the bytes (see fuzzPlugin).
static std::string tryEach(std::initializer_list<const char*> exprs) {
    std::string out;
    for (const char* e : exprs) out += std::string("try\n  v = ") + e + "\ncatch e\nend\n";
    return out;
}

static std::string pluginScript(const std::string& name) {
    std::string pre = "a = fuzz_input()\nb = fuzz_input2()\nn = fuzz_num()\n";
    if (name == "csv")
        return pre + tryEach({"csv_parse(a)", "csv_parse(a, b)", "csv_parse_dicts(a)", "csv_parse_dicts(a, b)",
                              "csv_stringify([[a, b], [n]])", "csv_stringify([[a]], b)"}) +
            // whatever csv_parse accepts, csv_stringify must write back as something that parses the same
            "ok = 0\ntry\n  r = csv_parse(a)\n  ok = 1\ncatch e\nend\n"
            "if ok\n  try\n    if csv_parse(csv_stringify(r)) != r\n      print(\"ROUNDTRIP-FAIL\")\n    end\n"
            "  catch e\n    print(\"ROUNDTRIP-FAIL\")\n  end\nend\n";
    if (name == "encoding")
        return pre + tryEach({"base64_decode(a)", "base64_decode(a, 1)", "base64_encode(a)", "base64_encode(a, 1, 1)",
                              "hex_decode(a)", "hex_encode(a)", "crc32(a)", "sha256(a)", "sha256_raw(a)"}) +
            "try\n  if base64_decode(base64_encode(a)) != a\n    print(\"ROUNDTRIP-FAIL\")\n  end\ncatch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n"
            "try\n  if base64_decode(base64_encode(a, 1, 1), 1) != a\n    print(\"ROUNDTRIP-FAIL\")\n  end\ncatch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n"
            "try\n  if hex_decode(hex_encode(a)) != a\n    print(\"ROUNDTRIP-FAIL\")\n  end\ncatch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n"
            // a decoded value must re-encode to something that decodes to the same bytes
            "ok = 0\ntry\n  d = base64_decode(a)\n  ok = 1\ncatch e\nend\n"
            "if ok\n  try\n    if base64_decode(base64_encode(d)) != d\n      print(\"ROUNDTRIP-FAIL\")\n    end\n"
            "  catch e\n    print(\"ROUNDTRIP-FAIL\")\n  end\nend\n";
    if (name == "regex")
        return pre + tryEach({"re_test(a, b)", "re_test(b, a)", "re_match(a, b)", "re_match(b, a)",
                              "re_find_all(a, b)", "re_find_all(b, a)", "re_replace(a, b, a)", "re_replace(a, b, \"$1$0$9\\\\\")",
                              "re_replace(b, a, b)", "re_find_all(\"aaaaaaaaaaaaaaaaaaaaaaaaaaaa!\", a)"});
    if (name == "str")
        return pre + tryEach({"str_split(a, b)", "str_replace(a, b, a)", "str_strip(a)", "str_ltrim(a)", "str_rtrim(a)",
                              "str_lower(a)", "str_upper(a)", "str_find(a, b)", "str_find(a, b, n)", "str_startswith(a, b)",
                              "str_endswith(a, b)", "str_isdigit(a)", "str_isalpha(a)", "str_isalnum(a)", "str_count(a, b)",
                              "str_ord(a)", "str_chr(n)", "str_repeat(a, n)", "str_reverse(a)", "str_pad_left(a, n)",
                              "str_pad_left(a, n, b)", "str_pad_right(a, n, b)", "str_format(a, b, n)", "str_format(a, a, b)",
                              "str_format(a)", "num_fixed(n, n)"});
    if (name == "unicode")
        return pre + tryEach({"unicode_len(a)", "unicode_ord(a)", "unicode_chr(n)", "unicode_chars(a)", "unicode_valid(a)",
                              "unicode_isalpha(a)", "unicode_isdigit(a)", "unicode_isalnum(a)"});
    if (name == "time")
        return pre + tryEach({"time_format(n, a)", "time_format(0, a)", "time_format(n, \"%Y-%m-%d %H:%M:%S\")",
                              "time_parse(a, b)", "time_parse(a, \"%Y-%m-%d %H:%M:%S\")", "time_parse(\"2024-02-29 12:00:00\", a)"});
    if (name == "vec")
        // indices come from the number picked out of the bytes (-1, 0, 5, 1e18, ...). the callbacks change the vec
        // while it is being walked. no doubling loops: the element cap is far above what memory can hold
        return pre + "v = vec_from([a, b, n])\n" + tryEach({
                "vec_get(v, n)", "vec_set(v, n, a)", "vec_insert(v, n, b)", "vec_remove(v, n)", "vec_swap(v, n, 0)",
                "vec_swap(v, 0, n)", "vec_slice(v, n)", "vec_slice(v, 0, n)", "vec_slice(v, n, n)", "vec_pop(v)",
                "vec_extend(v, [a, b])", "vec_extend(v, v)", "vec_extend(v, a)", "vec_get(a, 0)", "vec_len(n)",
                "vec_each_do(v, fn(x, i) vec_push(v, x) return i > 5 end)",
                "vec_each_do(v, fn(x, i) vec_clear(v) end)",
                "vec_each_do(v, fn(x, i) vec_remove(v, 0) end)",
                "vec_each_do(v, fn(x, i) vec_assign(v, [a]) end)",
                "vec_each_do(v, fn(x, i) error(a) end)",
                "vec_each_do(v, a)",
                "vec_map_do(v, fn(x, i) vec_clear(v) return x end)",
                "vec_map_do(v, fn(x, i) vec_insert(v, 0, x) return i end)",
                "vec_map_do(v, fn(x, i) return v end)",
                "vec_assign(v, v)", "vec_assign(v, [])", "vec_copy(v)", "vec_to_array(v)", "vec_reverse(v)",
                "vec_clear(v)", "vec_push(v, v)", "is_vec(v)"}) +
            // building from an array and reading it back must give the same array, and reversing twice is a no-op
            "w = vec_from([a, b, n, [a, b], {\"k\": a}])\n"
            "try\n  if vec_to_array(w) != [a, b, n, [a, b], {\"k\": a}]\n    print(\"ROUNDTRIP-FAIL\")\n  end\n"
            "  vec_reverse(w)\n  vec_reverse(w)\n"
            "  if vec_to_array(w) != [a, b, n, [a, b], {\"k\": a}]\n    print(\"ROUNDTRIP-FAIL\")\n  end\n"
            "  if vec_to_array(vec_copy(w)) != vec_to_array(w)\n    print(\"ROUNDTRIP-FAIL\")\n  end\n"
            "catch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n";
    return pre;
}

static bool fuzzPlugin(const std::string& src) {
    Capture cap;
    Interpreter interp;
    embr_register(&interp);
    size_t sep = src.find('\x01');
    std::string a = sep == std::string::npos ? src : src.substr(0, sep);
    std::string b = sep == std::string::npos ? src : src.substr(sep + 1);
    static const double nums[] = {-1, 0, 1, 5, 255, 256, 2147483647.0, 4294967296.0, 1e12, 1e18, 1e300, -1e18,
                                  0.5, 1700000000, 253402300800.0, -62135596800.0};
    double n = nums[std::hash<std::string>{}(src) % (sizeof nums / sizeof *nums)];
    interp.bind("fuzz_input",  [a](const std::vector<Value>&) -> Value { return Value(a); });
    interp.bind("fuzz_input2", [b](const std::vector<Value>&) -> Value { return Value(b); });
    interp.bind("fuzz_num",    [n](const std::vector<Value>&) -> Value { return Value(n); });
    static const std::string script = pluginScript(FUZZ_PLUGIN_NAME);
    try { runSource(script, interp, "<fuzz>"); }
    catch (const EmbrError&) {}
    catch (const std::exception& e) { std::fprintf(stderr, "fuzz: exception %s\n", e.what()); return false; }
    return cap.out.str().find("ROUNDTRIP-FAIL") == std::string::npos;
}
#endif

// ---- mutation -------------------------------------------------------------
static std::mt19937_64 rng;
static size_t rnd(size_t n) { return n ? rng() % n : 0; }

static const char* kTokens[] = {
    "fn ", "end\n", "if ", "elif ", "else\n", "while ", "for ", " in ", "try\n", "catch e\n",
    "return ", "break\n", "continue\n", "local ", "import ", "and ", "or ", "!", "-", "==", "!=",
    "<=", ">=", "(", ")", "[", "]", "{", "}", ",", ":", ".", "...", "->", "\"", "\\", "#", "#[", "]#",
    "\n", "  ", "0", "1.5", "99999999999999999999", "1e5", "0x", "..", "=", "+=", "int ", "auto ",
    "\"\\n\"", "\"\\x\"", "\r\n", "\t", "\xff", "\xc3\x28", "\0",
    // plugin inputs: the a/b separator, regex, strftime and format-string pieces, utf-8 edge cases
    "\x01", "*", "+", "?", "|", "(?:", "(?=", "\\1", "{2,}", "{0,99999}", "[^", "[a-", "^", "$", "\\d", "\\b",
    "%Y", "%S", "%z", "%", "{}", "{0}", "{:.2f}", "{:", "}}", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc0\x80", "==",
};

static std::string mutate(std::string s, const std::vector<std::string>& pool) {
    int rounds = 1 + (int)rnd(4);
    for (int i = 0; i < rounds; ++i) {
        switch (rnd(9)) {
            case 0: if (!s.empty()) s[rnd(s.size())] ^= (char)(1u << rnd(8)); break;
            case 1: if (!s.empty()) s[rnd(s.size())] = (char)rnd(256); break;
            case 2: s.insert(rnd(s.size() + 1), kTokens[rnd(sizeof kTokens / sizeof *kTokens)]); break;
            case 3: if (!s.empty()) { size_t a = rnd(s.size()); s.erase(a, 1 + rnd(std::min<size_t>(16, s.size() - a))); } break;
            case 4: if (!s.empty()) { size_t a = rnd(s.size()), n = 1 + rnd(std::min<size_t>(64, s.size() - a));
                        s.insert(rnd(s.size() + 1), s.substr(a, n)); } break;
            case 5: { const std::string& o = pool[rnd(pool.size())];
                      if (!o.empty()) { size_t a = rnd(o.size()), n = 1 + rnd(std::min<size_t>(200, o.size() - a));
                          s.insert(rnd(s.size() + 1), o.substr(a, n)); } } break;
            case 6: { // deep nesting of one opener : probes the recursive-descent stack
                      static const char* op[] = {"(", "[", "{", "-", "!", "fn(x) return ", "if 1\n", "while 1\n", "try\n", "a["};
                      std::string rep; size_t n = 1 + rnd(rnd(3) == 0 ? 200000 : 3000);
                      const char* o = op[rnd(sizeof op / sizeof *op)];
                      for (size_t k = 0; k < n; ++k) rep += o;
                      s.insert(rnd(s.size() + 1), rep); } break;
            case 7: if (s.size() > 1) s.resize(1 + rnd(s.size() - 1)); break;   // truncate
            default: if (!s.empty()) { size_t a = rnd(s.size()), b = rnd(s.size()); std::swap(s[a], s[b]); } break;
        }
        if (s.size() > (1u << 20)) s.resize(1u << 20);
    }
    return s;
}

// ---- program generator for the differential target ------------------------
struct Gen {
    std::ostringstream o;
    int ind = 0;
    static const int kVars = 5;

    void line(const std::string& s) { o << std::string(ind * 2, ' ') << s << "\n"; }
    bool inF0 = false;   // f0's own body must not call f0 (unbounded recursion just burns time on depth errors)
    std::string var() { return "x" + std::to_string(rnd(kVars)); }

    std::string lit() {
        switch (rnd(6)) {
            case 0: return std::to_string(rnd(10));
            case 1: return std::to_string((int)rnd(2000) - 1000);
            case 2: return std::to_string(rnd(100)) + "." + std::to_string(rnd(100));
            case 3: return "\"" + std::string(1 + rnd(3), (char)('a' + rnd(3))) + "\"";
            case 4: return std::to_string(9223372036854775000LL + (long long)rnd(800));
            default: return std::to_string(rnd(5));
        }
    }
    std::string expr(int d) {
        if (d <= 0 || rnd(4) == 0) return rnd(2) ? lit() : var();
        switch (rnd(10)) {
            case 0: case 1: case 2: case 3: {
                static const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "and", "or"};
                return "(" + expr(d - 1) + " " + ops[rnd(13)] + " " + expr(d - 1) + ")";
            }
            case 4: return std::string(rnd(2) ? "-" : "!") + expr(d - 1);
            case 5: return "[" + expr(d - 1) + ", " + expr(d - 1) + "]";
            case 6: return "{\"k\": " + expr(d - 1) + ", \"j\": " + expr(d - 1) + "}";
            case 7: return expr(d - 1) + "[" + std::to_string(rnd(3)) + "]";
            case 8: return "f" + std::to_string(inF0 ? 1 : rnd(2)) + "(" + expr(d - 1) + ", " + expr(d - 1) + ")";
            default: return "(fn(a) return a + " + expr(d - 1) + " end)(" + expr(d - 1) + ")";
        }
    }
    // every generated statement is wrapped in try/catch printing only "E": error *text* may
    // legitimately differ between backends, error-vs-no-error may not.
    void guarded(const std::string& stmt) {
        line("try"); ++ind; line(stmt); --ind;
        line("catch e"); ++ind; line("print(\"E\")"); --ind; line("end");
    }
    // index writes and push on plain variables, with copies taken first. these are the places the VM changes
    // a variable where it lives (INDEX_SET_VAR, APPEND_ARR), so a copy made earlier must not change
    void indexWrites() {
        switch (rnd(10)) {
            case 0: guarded("arr = [" + expr(1) + ", " + expr(1) + "]\narr[" + std::to_string(rnd(3)) + "] = " + expr(1) + "\nprint(arr)"); break;
            case 1: guarded("pa = [" + expr(1) + ", " + expr(1) + "]\npb = pa\npa = push(pa, " + expr(1) + ")\npa[" +
                            std::to_string(rnd(3)) + "] = " + expr(1) + "\nprint(pa)\nprint(pb)"); break;
            case 2: guarded("pm = {\"k\": 1}\npn = pm\npm[\"k\" + str(" + expr(1) + ")] = " + expr(1) + "\nprint(pm)\nprint(pn)"); break;
            case 3: guarded("pa = [1, 2]\npb = pa\npa[f1(" + expr(1) + ", " + expr(1) + ")] = " + expr(1) + "\nprint(pa)\nprint(pb)"); break;
            case 4: guarded("pa = [1, 2]\npa = push(pa, f1(" + expr(1) + ", " + expr(1) + "))\nprint(pa)"); break;
            case 5: guarded("fn pf(a, b)\n  l = [a, b]\n  k = l\n  l = push(l, f1(a, b))\n  l[f1(0, 1)] = l[0] + b\n"
                            "  l[1] = [l[0], l[1]]\n  l[1][0] = a\n  return [l, k]\nend\nprint(pf(" + expr(1) + ", " + expr(1) + "))"); break;
            case 6: guarded("pa = [1, 2]\npb = pa\nappend(&pa, " + expr(1) + ")\ninsert(&pa, 0, " + expr(1) + ")\nprint(pa)\nprint(pb)"); break;
            case 7: guarded("pg = [[1], [2]]\nph = pg\nappend(&pg[1], " + expr(1) + ")\nprint(remove_last(&pg[0]))\nprint(pg)\nprint(ph)"); break;
            case 8: guarded("pm = {\"k\": [1]}\nappend(&pm[\"k\"], f1(" + expr(1) + ", " + expr(1) + "))\nremove_key(&pm, \"j\")\nprint(pm)"); break;
            default: guarded("pg = [[1, 2], {\"k\": [3]}]\nph = pg\npg[1][\"k\"][0] = " + expr(1) + "\npg[0][1] = pg\nprint(pg)\nprint(ph)"); break;
        }
    }
    void stmt(int d, int loopDepth) {
        switch (rnd(d > 0 ? 8 : 4)) {
            case 0: case 1: guarded(var() + " = " + expr(2)); break;
            case 2: case 3: guarded("print(" + expr(3) + ")"); break;
            case 4: { line("if " + expr(2)); ++ind; stmt(d - 1, loopDepth); --ind;
                      if (rnd(2)) { line("else"); ++ind; stmt(d - 1, loopDepth); --ind; }
                      line("end"); } break;
            case 5: { std::string lv = "lc" + std::to_string(loopDepth);
                      line("for " + lv + " in [" + lit() + ", " + lit() + ", " + lit() + "]"); ++ind;
                      guarded(var() + " = " + lv + " + " + expr(1));
                      stmt(d - 1, loopDepth + 1); --ind; line("end"); } break;
            case 6: { std::string c = "wc" + std::to_string(loopDepth);
                      line(c + " = 0"); line("while " + c + " < 3"); ++ind;
                      stmt(d - 1, loopDepth + 1); line(c + " += 1"); --ind; line("end"); } break;
            default: indexWrites(); break;
        }
    }
    std::string program() {
        for (int i = 0; i < kVars; ++i) line("x" + std::to_string(i) + " = " + std::to_string(rnd(9)));
        // f0 assigns fresh names (t0, t1), once directly and once first inside an if/else block, so the differential run
        // covers an undeclared assignment inside a function (local to the call on both backends). the names are then
        // probed at top level, where they must be undefined on both
        line("fn f0(a, b)"); ++ind; inF0 = true;
        line("t0 = " + expr(1));
        line("if " + expr(1)); ++ind; line("t1 = " + expr(1)); --ind;
        line("else"); ++ind; line("t1 = " + expr(1)); --ind; line("end");
        line("return " + expr(2) + " == t1");
        inF0 = false; --ind; line("end");
        line("f1 = fn(a, b) return a + b end");
        int n = 4 + (int)rnd(8);
        for (int i = 0; i < n; ++i) stmt(2, 0);
        for (int i = 0; i < kVars; ++i) guarded("print(x" + std::to_string(i) + ")");
        guarded("print(t0)");   // must raise on both backends: function-locals don't leak
        guarded("print(t1)");
        return o.str();
    }
};

struct RunResult { std::string out; bool errored; };
template <typename RunFn>
static RunResult runOn(const std::string& src, RunFn run) {
    Capture cap;
    Interpreter interp;
    bool errored = false;
    try { run(src, interp, "<fuzz>"); } catch (const EmbrError&) { errored = true; }
    if (!cap.err.str().empty()) errored = true;
    return {cap.out.str(), errored};
}

// ---- main -----------------------------------------------------------------
static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

#ifdef FUZZ_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    std::string s((const char*)d, n);
#  ifdef FUZZ_JSON
    return fuzzJson(s) ? 0 : (std::abort(), 1);
#  elif defined(FUZZ_PLUGIN_SRC)
    return fuzzPlugin(s) ? 0 : (std::abort(), 1);
#  else
    return fuzzFrontend(s) ? 0 : (std::abort(), 1);
#  endif
}
#else
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s frontend|fmt|json|diff|<plugin> [-n N] [-seed S] [-t SECONDS] [files...]\n", argv[0]); return 2; }
    std::string mode = argv[1];
    if (mode == "replay" && argc >= 3) {          // embr_fuzz replay FILE: run one saved input once
        installHandlers();
        g_current = readFile(argv[2]);
        alarm(20);
#ifdef FUZZ_JSON
        bool ok = fuzzJson(g_current);
#elif defined(FUZZ_PLUGIN_SRC)
        bool ok = fuzzPlugin(g_current);
#else
        bool ok = fuzzFrontend(g_current) && fuzzFmt(g_current);
#endif
        std::printf("replay: %s\n", ok ? "ok" : "CONTRACT VIOLATION");
        return ok ? 0 : 1;
    }
    long n = 2000; unsigned long seed = std::random_device{}(); int timeout = 10;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-n" && i + 1 < argc) n = std::atol(argv[++i]);
        else if (a == "-seed" && i + 1 < argc) seed = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "-t" && i + 1 < argc) timeout = std::atoi(argv[++i]);
        else files.push_back(a);
    }
    rng.seed(seed);
    std::printf("fuzz: mode=%s n=%ld seed=%lu\n", mode.c_str(), n, seed);
    installHandlers();

    std::vector<std::string> pool = {
        "x = 1\nprint(x)\n", "fn f(a, b)\n  return a + b\nend\nprint(f(1, 2))\n",
        "for i in [1, 2, 3]\n  print(i)\nend\n", "try\n  error(\"x\")\ncatch e\n  print(e[\"message\"])\nend\n",
        "m = {\"a\": [1, 2, {\"b\": 3}]}\nprint(m.a[2].b)\n", "g = fn(n) return fn(x) return x + n end end\nprint(g(1)(2))\n",
        "local int x = 5\nx += 1\nwhile x < 9\n  x += 1\n  if x == 7\n    continue\n  end\nend\n",
        "a, b = [1, 2]\nprint(\"s\" + \"t\")\n#[ block ]#\n# line\n",
        "{\"a\": [1, 2.5, \"s\\u00e9\", null, true], \"b\": {\"c\": -1e5}}",
    };
#ifdef FUZZ_PLUGIN_SRC
    pool = { std::string("abc\x01" "def") };   // embr programs make poor plugin inputs; corpus files follow
#endif
    for (auto& f : files) pool.push_back(readFile(f));

    long bad = 0;
    for (long it = 0; it < n; ++it) {
        alarm(timeout);
        if (mode == "diff") {
            Gen g; std::string prog = g.program();
            g_current = prog;
            RunResult a = runOn(prog, [](const std::string& s, Interpreter& i, const std::string& f) { runSource(s, i, f); });
            RunResult b = runOn(prog, [](const std::string& s, Interpreter& i, const std::string& f) { vm::runSource(s, i, f); });
            if (a.out != b.out || a.errored != b.errored) {
                ++bad;
                saveInput("diff", prog, "embr");
                std::fprintf(stderr, "fuzz: DIVERGENCE (tree errored=%d, vm errored=%d)\n--- tree ---\n%s--- vm ---\n%s\n",
                             a.errored, b.errored, a.out.c_str(), b.out.c_str());
            }
        } else {
            std::string in = mutate(pool[rnd(pool.size())], pool);
            if (mode == "fmt" && in.size() > (256u << 10)) in.resize(256u << 10);   // output grows with input, keep runs short
#ifdef FUZZ_PLUGIN_SRC
            if (in.size() > 2048) in.resize(2048);   // regex is pattern length times subject length, so big inputs only time out
#endif
            g_current = in;
#ifdef FUZZ_JSON
            bool ok = (mode == "json") ? fuzzJson(in) : true;
#elif defined(FUZZ_PLUGIN_SRC)
            bool ok = (mode == FUZZ_PLUGIN_NAME) ? fuzzPlugin(in) : true;
#else
            bool ok = mode == "fmt" ? fuzzFmt(in) : (mode == "frontend") ? fuzzFrontend(in) : true;
#endif
            if (!ok) { ++bad; saveInput("crash", in); }
            if (in.size() < 400 && rnd(8) == 0) pool.push_back(in);   // grow the pool with small survivors
        }
        if ((it + 1) % 500 == 0) { std::printf("fuzz: %ld/%ld, findings=%ld\n", it + 1, n, bad); std::fflush(stdout); }
    }
    alarm(0);
    std::printf("fuzz: done, %ld iterations, %ld findings\n", n, bad);
    return bad ? 1 : 0;
}
#endif
