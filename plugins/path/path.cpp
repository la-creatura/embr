// path.cpp
// pure path-string utilities for embr. nothing here touches the filesystem
// (see the fs plugin for exists/list/mkdir/remove), every function is a
// lexical transformation of its string arguments, so a path need not exist.
//
// usage
//   import "path"
//   print(path_join("a", "b", "c.txt"))     # a/b/c.txt
//   print(path_normalize("a/./b/../c"))     # a/c
//   print(path_basename("/x/y.tar.gz"))     # y.tar.gz
//   print(path_stem("/x/y.tar.gz"))         # y.tar
//   print(path_extname("/x/y.tar.gz"))      # .gz
//   print(path_dirname("/x/y.tar.gz"))      # /x
//   print(path_is_abs("/x"))                # 1
//
// separators are whatever std::filesystem treats as such on the host ('/' on
// POSIX, '/' and '\' on Windows). fs also exposes fs_join/fs_basename/
// fs_dirname/fs_extname; those predate this plugin and are left as they are
// (plugins have no runtime dependency on each other, so this one does not
// reuse them), prefer path_* in new code, it is the superset.

#include <embr/embr.h>
#include <filesystem>

using namespace embr;

static Param pStr (std::string n)                        { return Param::req(std::move(n), TS::Str); }
static Param pRestStr(std::string n)                     { return Param::rest(std::move(n), TS::Str); }

EMBR_PLUGIN {
    namespace fs = std::filesystem;

    // path_join(a: str, ...more: str) -> str
    // like Python's os.path.join: a later absolute component discards
    // everything before it; an empty component is skipped.
    interp->bindSig("path_join", {pStr("a"), pRestStr("more")},
    [](const std::vector<Value>& args) -> Value {
        fs::path out(args[0].asString());
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].asString().empty()) continue;
            out /= fs::path(args[i].asString());
        }
        return Value(out.generic_string());
    });

    // path_dirname(p: str) -> str
    // everything before the final component; "" when there is none, "/" for
    // a root-level entry. trailing separators are ignored ("a/b/" -> "a").
    interp->bindSig("path_dirname", {pStr("p")},
    [](const std::vector<Value>& args) -> Value {
        fs::path p(args[0].asString());
        if (p.has_filename() == false && p.has_relative_path()) p = p.parent_path(); // strip trailing '/'
        return Value(p.parent_path().generic_string());
    });

    // path_basename(p: str) -> str
    // final component; trailing separators are ignored ("a/b/" -> "b"), "" for "" or "/".
    interp->bindSig("path_basename", {pStr("p")},
    [](const std::vector<Value>& args) -> Value {
        fs::path p(args[0].asString());
        if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();
        return Value(p.filename().generic_string());
    });

    // path_extname(p: str) -> str   (".gz" for "y.tar.gz"; "" for ".bashrc" or "noext")
    interp->bindSig("path_extname", {pStr("p")},
    [](const std::vector<Value>& args) -> Value {
        return Value(fs::path(args[0].asString()).extension().generic_string());
    });

    // path_stem(p: str) -> str   (basename without its final extension)
    interp->bindSig("path_stem", {pStr("p")},
    [](const std::vector<Value>& args) -> Value {
        return Value(fs::path(args[0].asString()).stem().generic_string());
    });

    // path_normalize(p: str) -> str
    // collapses ".", "..", and repeated separators lexically (no symlink
    // resolution). "" normalizes to ".".
    interp->bindSig("path_normalize", {pStr("p")},
    [](const std::vector<Value>& args) -> Value {
        if (args[0].asString().empty()) return Value(std::string("."));
        fs::path n = fs::path(args[0].asString()).lexically_normal();
        std::string s = n.generic_string();
        // lexically_normal leaves a trailing separator for "a/b/" and "a/.." -> "a/"-style
        // results; drop it except for a bare root so the output is uniform.
        while (s.size() > 1 && (s.back() == '/' || s.back() == '\\') && n.has_relative_path())
            s.pop_back();
        if (s.empty()) s = ".";
        return Value(s);
    });

    // path_is_abs(p: str) -> num   (1/0)
    interp->bindSig("path_is_abs", {pStr("p")},
    [](const std::vector<Value>& args) -> Value {
        return Value((fs::path(args[0].asString()).is_absolute() || (!args[0].asString().empty() && (args[0].asString()[0] == '/' || args[0].asString()[0] == '\\'))) ? 1.0 : 0.0);
    });

    // path_relative(p: str, base: str) -> str
    // lexical path of p relative to base ("" if none exists, e.g. different roots)
    interp->bindSig("path_relative", {pStr("p"), pStr("base")},
    [](const std::vector<Value>& args) -> Value {
        return Value(fs::path(args[0].asString()).lexically_relative(args[1].asString()).generic_string());
    });
}
