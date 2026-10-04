// embrmath.cpp
// math plugin for embr: <cmath> wrappers, min/max/clamp, and a seedable PRNG
//
// usage
//   import "embrmath"
//   print(math_sqrt(16))
//   print(math_clamp(12, 0, 10))
//   math_seed(42)
//   print(math_randint(1, 6))
//
// naming follows str.cpp's str_* convention: everything is math_* (not bare sqrt, min, ...) so it can't collide with
// a script's own variables

#include <embr/embr.h>
#include <cmath>
#include <random>

using namespace embr;

static Param pNum(std::string n) { return Param::req(std::move(n), TS::Num); }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[embrmath:" + fn + "]", msg);
}

// PRNG state, seeded from a real random source by default. math_seed() reseeds it deterministically (for
// reproducible tests)
//
// thread_local, not a plain static: a static would be one lock-free generator shared by every Interpreter/thread
// that loads this .so, which is a data race. thread_local gives each thread (embr's model is one isolated
// Interpreter per thread, see plugins/parallel) its own generator. the catch: math_seed() reseeds only the calling
// thread's stream, which is what you want for reproducible worker tasks
static thread_local std::mt19937_64 g_rng{std::random_device{}()};

Value math_sqrt (const std::vector<Value>& args) { return Value(std::sqrt (args[0].asNumber())); }
Value math_pow  (const std::vector<Value>& args) { return Value(std::pow  (args[0].asNumber(), args[1].asNumber())); }
Value math_abs  (const std::vector<Value>& args) { return Value(std::fabs (args[0].asNumber())); }
Value math_floor(const std::vector<Value>& args) { return Value(std::floor(args[0].asNumber())); }
Value math_ceil (const std::vector<Value>& args) { return Value(std::ceil (args[0].asNumber())); }
Value math_round(const std::vector<Value>& args) { return Value(std::round(args[0].asNumber())); }

Value math_sin(const std::vector<Value>& args) { return Value(std::sin(args[0].asNumber())); }
Value math_cos(const std::vector<Value>& args) { return Value(std::cos(args[0].asNumber())); }
Value math_tan(const std::vector<Value>& args) { return Value(std::tan(args[0].asNumber())); }

Value math_asin(const std::vector<Value>& args) { return Value(std::asin(args[0].asNumber())); }
Value math_acos(const std::vector<Value>& args) { return Value(std::acos(args[0].asNumber())); }
Value math_atan(const std::vector<Value>& args) { return Value(std::atan(args[0].asNumber())); }
// math_atan2(y, x): same argument order as std::atan2 and every other language's atan2
Value math_atan2(const std::vector<Value>& args) { return Value(std::atan2(args[0].asNumber(), args[1].asNumber())); }

Value math_log  (const std::vector<Value>& args) { return Value(std::log  (args[0].asNumber())); } // natural log
Value math_log2 (const std::vector<Value>& args) { return Value(std::log2 (args[0].asNumber())); }
Value math_log10(const std::vector<Value>& args) { return Value(std::log10(args[0].asNumber())); }
Value math_exp  (const std::vector<Value>& args) { return Value(std::exp  (args[0].asNumber())); }

Value math_min(const std::vector<Value>& args) { return Value(std::min(args[0].asNumber(), args[1].asNumber())); }
Value math_max(const std::vector<Value>& args) { return Value(std::max(args[0].asNumber(), args[1].asNumber())); }

Value math_clamp(const std::vector<Value>& args) {
    double x = args[0].asNumber(), lo = args[1].asNumber(), hi = args[2].asNumber();
    if (lo > hi) throwError("math_clamp", "lo (" + formatNumber(lo) + ") must not exceed hi (" + formatNumber(hi) + ")");
    return Value(std::min(std::max(x, lo), hi));
}

// math_seed(n): reseeds the shared PRNG deterministically. call this at
// the top of a script before math_random()/math_randint() for reproducible
// output (e.g. in a test).
Value math_seed(const std::vector<Value>& args) {
    g_rng.seed((uint64_t)numToInt64(args[0], "math_seed", "seed"));
    return Value(0.0);
}

// math_random() -> float in [0, 1)
Value math_random(const std::vector<Value>&) {
    return Value(std::uniform_real_distribution<double>(0.0, 1.0)(g_rng));
}

// math_randint(lo, hi) -> integer in [lo, hi], inclusive both ends
Value math_randint(const std::vector<Value>& args) {
    int64_t lo = numToInt64(args[0], "math_randint", "lo"), hi = numToInt64(args[1], "math_randint", "hi");
    if (lo > hi) throwError("math_randint", "lo (" + std::to_string(lo) + ") must not exceed hi (" + std::to_string(hi) + ")");
    return Value(std::uniform_int_distribution<int64_t>(lo, hi)(g_rng));
}

EMBR_PLUGIN {
    interp->define("PI", Value(3.14159265358979323846));
    interp->define("E",  Value(2.71828182845904523536));

    interp->bindSig("math_sqrt",  {pNum("x")}, math_sqrt);
    interp->bindSig("math_pow",   {pNum("x"), pNum("y")}, math_pow);
    interp->bindSig("math_abs",   {pNum("x")}, math_abs);
    interp->bindSig("math_floor", {pNum("x")}, math_floor);
    interp->bindSig("math_ceil",  {pNum("x")}, math_ceil);
    interp->bindSig("math_round", {pNum("x")}, math_round);

    interp->bindSig("math_sin", {pNum("x")}, math_sin);
    interp->bindSig("math_cos", {pNum("x")}, math_cos);
    interp->bindSig("math_tan", {pNum("x")}, math_tan);
    interp->bindSig("math_asin", {pNum("x")}, math_asin);
    interp->bindSig("math_acos", {pNum("x")}, math_acos);
    interp->bindSig("math_atan", {pNum("x")}, math_atan);
    interp->bindSig("math_atan2", {pNum("y"), pNum("x")}, math_atan2);

    interp->bindSig("math_log",   {pNum("x")}, math_log);
    interp->bindSig("math_log2",  {pNum("x")}, math_log2);
    interp->bindSig("math_log10", {pNum("x")}, math_log10);
    interp->bindSig("math_exp",   {pNum("x")}, math_exp);

    interp->bindSig("math_min",   {pNum("a"), pNum("b")}, math_min);
    interp->bindSig("math_max",   {pNum("a"), pNum("b")}, math_max);
    interp->bindSig("math_clamp", {pNum("x"), pNum("lo"), pNum("hi")}, math_clamp);

    interp->bindSig("math_seed",    {pNum("n")}, math_seed);
    interp->bindSig("math_random",  {}, math_random);
    interp->bindSig("math_randint", {pNum("lo"), pNum("hi")}, math_randint);
}
