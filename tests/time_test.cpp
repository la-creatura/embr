// unit tests for plugins/time/time.cpp (imported once by main() below)
//
// time_now()/time_now_ms()/time_monotonic() are live clock reads, so the tests only check that they are positive,
// increasing and roughly consistent with each other. time_format()/time_parse() are checked against exact UTC
// calendar results, which don't depend on when or where the test runs

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "time_now / time_now_ms: both positive and roughly consistent with each other",
          "diff = time_now_ms() - time_now() * 1000\n"
          "print(time_now() > 0)\n"
          "print(time_now_ms() > 0)\n"
          "print(diff > -60000 and diff < 60000)\n",
          "1\n1\n1\n" },

        { "time_monotonic: advances by roughly the sleep duration",
          "t0 = time_monotonic()\ntime_sleep(0.05)\nt1 = time_monotonic()\nprint(t1 - t0 >= 0.04)\n",
          "1\n" },

        { "time_format: epoch 0 is the Unix epoch (UTC)",
          "print(time_format(0, \"%Y-%m-%d %H:%M:%S\"))\n",
          "1970-01-01 00:00:00\n" },

        { "time_format: a later date",
          "print(time_format(1704067200, \"%Y-%m-%d\"))\n", // 2024-01-01T00:00:00Z
          "2024-01-01\n" },

        { "time_parse: round-trips with time_format",
          "e = time_parse(\"2024-01-02 03:04:05\", \"%Y-%m-%d %H:%M:%S\")\n"
          "print(time_format(e, \"%Y-%m-%d %H:%M:%S\"))\n",
          "2024-01-02 03:04:05\n" },

        { "time_parse: the epoch round-trips to 0",
          "print(time_parse(\"1970-01-01\", \"%Y-%m-%d\"))\n",
          "0\n" },

        { "time_parse: raises on a string that doesn't match the format",
          "try\ntime_parse(\"not a date\", \"%Y-%m-%d\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "time_format: offset shifts the shown time, %z prints it",
          "print(time_format(0, \"%Y-%m-%d %H:%M %z\", 5400))\n"
          "print(time_format(0, \"%H:%M %z\", -18000))\n"
          "print(time_format(0, \"%H:%M %z\"))\n"
          "print(time_format(0, \"100%%z\"))\n",
          "1970-01-01 01:30 +0130\n19:00 -0500\n00:00 +0000\n100%z\n" },

        { "time_parse: offset argument and %z both move the result",
          "print(time_parse(\"1970-01-01 01:30\", \"%Y-%m-%d %H:%M\", 5400))\n"
          "print(time_parse(\"1970-01-01 01:30 +0130\", \"%Y-%m-%d %H:%M %z\"))\n"
          "print(time_parse(\"1970-01-01 01:30 +01:30\", \"%Y-%m-%d %H:%M %z\"))\n"
          "print(time_parse(\"1970-01-01 00:00 Z\", \"%Y-%m-%d %H:%M %z\", 3600))\n"
          "print(time_parse(\"1970-01-01 00:00 -0500\", \"%Y-%m-%d %H:%M %z\"))\n",
          "0\n0\n0\n0\n18000\n" },

        { "time_parse / time_format: bad %z and out-of-range offsets raise",
          "try\ntime_parse(\"1970-01-01 xx\", \"%Y-%m-%d %z\")\nprint(\"unreachable\")\ncatch e\nprint(\"bad z\")\nend\n"
          "try\ntime_format(0, \"%H\", 90000)\nprint(\"unreachable\")\ncatch e\nprint(\"bad offset\")\nend\n",
          "bad z\nbad offset\n" },

        { "time_utc_offset: a whole number of seconds in a sane range, now or at a given time",
          "o = time_utc_offset()\nprint(o > -86400 and o < 86400)\n"
          "print(time_utc_offset(0) < 86400)\n",
          "1\n1\n" },

        { "time_sleep: raises on a negative duration",
          "try\ntime_sleep(-1)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "time: out-of-range epochs, absurd sleeps and oversized formats raise instead of UB / silent garbage",
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "try\n  time_format(big, \"%Y\")\n  print(\"unreachable\")\ncatch e\n  print(\"epoch rejected\")\nend\n"
          "try\n  time_sleep(big)\n  print(\"unreachable\")\ncatch e\n  print(\"sleep rejected\")\nend\n"
          "print(len(time_format(0, str_repeat(\"%Y\", 2000))))\n"
          "print(time_format(86400, \"%Y-%m-%d\"))\n",
          "epoch rejected\nsleep rejected\n8000\n1970-01-02\n" },
    };
    return embr_test::runSuite("time plugin test suite", tests, {"embrlib", "str", "time"});
}
