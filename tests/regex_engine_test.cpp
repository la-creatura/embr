// differential test for plugins/regex/pike.h: on random patterns and subjects the Pike VM must agree with std::regex
// (ECMAScript grammar) on whether there is a match, where, and what every capture group holds. std::regex is only a
// reliable oracle on short subjects (it overflows the stack on long ones, which is why pike.h exists), so subjects
// here are tiny and long inputs are covered by tests/regex_test.cpp
//
// deterministic (fixed seed), so a failure can be reproduced. prints the first mismatches and exits nonzero

#include "../plugins/regex/pike.h"
#include <cstdio>
#include <random>
#include <regex>

static std::mt19937_64 rng(12345);
static size_t rnd(size_t n) { return rng() % n; }

static std::string genAtom(int depth);
static std::string genAlt(int depth, bool nonEmpty);

static std::string genAtom(int depth) {
    switch (rnd(depth > 0 ? 13 : 10)) {
        case 0: case 1: case 2: return std::string(1, "abc"[rnd(3)]);
        case 3: return ".";
        case 4: return "[ab]";
        case 5: return "[^a]";
        case 6: return "\\d";
        case 7: return "\\w";
        case 8: return "\\s";
        case 9: return std::string(1, "^$"[rnd(2)]);
        case 10: return "\\b";
        // group bodies always consume at least one byte (genAlt(..., true)): libstdc++ does not follow the
        // ECMAScript rule that rejects an empty loop iteration, so quantified NULLABLE groups like (a?)+
        // give different capture/extent results there even though this engine follows the spec. the
        // oracle is only trustworthy on loop bodies that cannot match empty.
        case 11: return "(" + genAlt(depth - 1, true) + ")";
        default: return "(?:" + genAlt(depth - 1, true) + ")";
    }
}
static std::string genTerm(int depth) {
    std::string a = genAtom(depth);
    if (a == "^" || a == "$" || a == "\\b") return a;               // don't quantify assertions (std differs)
    static const char* q[] = {"", "", "", "*", "+", "?", "{2}", "{1,2}", "{0,1}", "*?", "+?", "??", "{1,3}?"};
    return a + q[rnd(13)];
}
static std::string genCat(int depth, bool nonEmpty) {
    std::string s;
    if (nonEmpty) {                                   // a mandatory, unquantified, byte-consuming first atom
        static const char* must[] = {"a", "b", "c", ".", "[ab]", "[^a]", "\\d", "\\w", "\\s"};
        s += must[rnd(9)];
    }
    size_t n = nonEmpty ? rnd(3) : 1 + rnd(3);
    for (size_t i = 0; i < n; ++i) s += genTerm(depth);
    return s;
}
static std::string genAlt(int depth, bool nonEmpty) {
    std::string s = genCat(depth, nonEmpty);
    if (rnd(4) == 0) s += "|" + genCat(depth, nonEmpty);
    return s;
}
static std::string genSubject() {
    static const char al[] = "aabbc 1_\n";
    std::string s;
    size_t n = rnd(9);
    for (size_t i = 0; i < n; ++i) s += al[rnd(sizeof al - 1)];
    return s;
}

static std::string show(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\n') o += "\\n"; else o += c; }
    return o;
}

int main() {
#ifndef __GLIBCXX__
    // the oracle is std::regex. libc++ (macos) handles \b differently, e.g. it finds one match of /\b/ in "bb"
    // where ECMAScript has two, so on that library a mismatch says nothing about the Pike engine
    std::puts("skipped: std::regex here is not libstdc++, so it can't act as the oracle");
    return 77;
#endif
#ifdef __SANITIZE_ADDRESS__
    const int N = 8000;    // the comparison is statistical, and sanitizer builds are several times slower
#else
    const int N = 40000;
#endif
    int compared = 0, skipped = 0, mismatches = 0, caps_only = 0;
    for (int it = 0; it < N; ++it) {
        std::string pat = genAlt(2, false), subj = genSubject();
        std::regex ref;
        try { ref = std::regex(pat); } catch (const std::regex_error&) { ++skipped; continue; }
        rx::Prog prog;
        try { prog = rx::compile(pat); }
        catch (const rx::Unsupported&) { ++skipped; continue; }
        catch (const rx::SyntaxError& e) {
            std::printf("MISMATCH (rx rejects a pattern std::regex accepts) /%s/: %s\n", show(pat).c_str(), e.what());
            if (++mismatches >= 25) break;
            continue;
        }
        rx::Matcher m(prog, subj);
        std::vector<int> caps;
        bool got = m.search(0, caps);
        std::smatch sm;
        bool want = std::regex_search(subj, sm, ref);
        ++compared;
        bool ok = (got == want);
        bool capsOk = true;
        if (ok && got) {
            for (size_t g = 0; g < sm.size() && g * 2 + 1 < caps.size(); ++g) {
                int s = sm[g].matched ? (int)(sm[g].first - subj.begin()) : -1;
                int e = sm[g].matched ? (int)(sm[g].second - subj.begin()) : -1;
                if (caps[2 * g] != s || caps[2 * g + 1] != e) capsOk = false;
            }
        }
        // the iteration the plugin's re_find_all / re_replace use (every non-overlapping match; an empty
        // match advances one byte) must give the same list of matches as std::sregex_iterator
        {
            std::vector<std::pair<int,int>> mine, theirs;
            size_t pos = 0;
            std::vector<int> c2;
            bool prevEmpty = false;
            // same loop as plugins/regex/regex.cpp allMatches()
            while (pos <= subj.size()) {
                bool found;
                if (prevEmpty) {
                    found = m.search(pos, c2, true);
                    if (!found) { ++pos; prevEmpty = false; continue; }
                } else found = m.search(pos, c2);
                if (!found) break;
                mine.push_back({c2[0], c2[1]});
                pos = (size_t)c2[1];
                prevEmpty = (c2[1] == c2[0]);
            }
            for (auto i2 = std::sregex_iterator(subj.begin(), subj.end(), ref), e2 = std::sregex_iterator(); i2 != e2; ++i2)
                theirs.push_back({(int)((*i2)[0].first - subj.begin()), (int)((*i2)[0].second - subj.begin())});
            if (mine != theirs) {
                std::printf("MISMATCH (find_all iteration) /%s/ on \"%s\": rx %zu matches, std %zu\n",
                            show(pat).c_str(), show(subj).c_str(), mine.size(), theirs.size());
                if (++mismatches >= 25) break;
            }
        }
        if (!ok || !capsOk) {
            if (ok) ++caps_only;
            std::printf("MISMATCH%s /%s/ on \"%s\": rx=%s std=%s\n", ok ? " (captures)" : "", show(pat).c_str(),
                        show(subj).c_str(), got ? "match" : "none", want ? "match" : "none");
            if (got && want) {
                std::printf("   rx :");
                for (size_t g = 0; g * 2 + 1 < caps.size(); ++g) std::printf(" [%d,%d)", caps[2*g], caps[2*g+1]);
                std::printf("\n   std:");
                for (size_t g = 0; g < sm.size(); ++g)
                    std::printf(" [%d,%d)", sm[g].matched ? (int)(sm[g].first - subj.begin()) : -1,
                                sm[g].matched ? (int)(sm[g].second - subj.begin()) : -1);
                std::printf("\n");
            }
            if (++mismatches >= 25) break;
        }
    }
    std::printf("regex engine differential: %d compared, %d skipped (invalid/unsupported), %d mismatches (%d captures-only)\n",
                compared, skipped, mismatches, caps_only);
    return mismatches ? 1 : 0;
}
