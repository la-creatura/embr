// fs.cpp
// filesystem plugin for embr: existence/type checks, directory listing,
// mkdir/remove, and pure path-string utilities (join/basename/dirname/extname)
//
// usage
//   import "fs"
//   fs_mkdir("out")
//   write_file("out/report.txt", "...")   # embrlib, not this plugin
//   print(fs_list_dir("out"))
//   print(fs_join("out", "report.txt"))
//
// path resolution is the same as embrlib's write_file()/append_file(): an absolute path is used as-is, a relative
// one resolves against Interpreter::scriptDir when a script file is running, or the process's cwd otherwise (the
// REPL). there is no "search" like load_file() does, because most calls here (mkdir, remove) point at a path that
// may not exist yet

#include <embr/embr.h>
#include <algorithm>
#include <filesystem>

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[fs:" + fn + "]", msg);
}

static std::filesystem::path resolvePath(Interpreter& interp, const std::string& raw) {
    namespace fs = std::filesystem;
    fs::path p(raw);
    if (p.is_absolute()) return p;
    return interp.scriptDir.empty() ? (fs::current_path() / p) : (fs::path(interp.scriptDir) / p);
}

EMBR_PLUGIN {

    // fs_exists(path: str) -> num
    interp->bindSig("fs_exists", {pStr("path")},
    [interp](const std::vector<Value>& args) -> Value {
        std::error_code ec;
        bool ok = std::filesystem::exists(resolvePath(*interp, args[0].asString()), ec);
        return Value(!ec && ok ? 1.0 : 0.0);
    });

    // fs_is_dir(path: str) -> num
    interp->bindSig("fs_is_dir", {pStr("path")},
    [interp](const std::vector<Value>& args) -> Value {
        std::error_code ec;
        bool ok = std::filesystem::is_directory(resolvePath(*interp, args[0].asString()), ec);
        return Value(!ec && ok ? 1.0 : 0.0);
    });

    // fs_list_dir(path: str) -> arr of entry names (not full paths), sorted alphabetically. directory_iterator's own
    // order depends on the platform, so this sorts, like keys()/values() do
    interp->bindSig("fs_list_dir", {pStr("path")},
    [interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;
        fs::path dir = resolvePath(*interp, args[0].asString());

        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec)
            throwError("fs_list_dir", "not a directory: " + dir.string());

        std::vector<std::string> names;
        for (auto& entry : fs::directory_iterator(dir, ec))
            names.push_back(entry.path().filename().generic_string());
        if (ec) throwError("fs_list_dir", "cannot list '" + dir.string() + "': " + ec.message());

        std::sort(names.begin(), names.end());
        Value::array_type out;
        for (auto& n : names) out.push_back(Value(n));
        return Value(std::move(out));
    });

    // fs_mkdir(path: str, recursive?: num) -> num
    // creates one directory (or, if recursive is truthy, every missing
    // parent along the way too, like `mkdir -p`). returns 1 if a directory
    // was actually created, 0 if it already existed, either way the
    // directory exists once this returns without raising.
    interp->bindSig("fs_mkdir", {pStr("path"), pOpt("recursive", TS::Num)},
    [interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;
        fs::path p = resolvePath(*interp, args[0].asString());
        bool recursive = args.size() >= 2 && args[1].truthy();

        std::error_code ec;
        bool created = recursive ? fs::create_directories(p, ec) : fs::create_directory(p, ec);
        if (ec) throwError("fs_mkdir", "cannot create '" + p.string() + "': " + ec.message());
        return Value(created ? 1.0 : 0.0);
    });

    // fs_remove(path: str) -> num
    // removes a single file or empty directory (not recursive, a
    // non-empty directory raises rather than silently deleting its
    // contents). returns 1 if something was removed, 0 if the path didn't exist.
    interp->bindSig("fs_remove", {pStr("path")},
    [interp](const std::vector<Value>& args) -> Value {
        namespace fs = std::filesystem;
        fs::path p = resolvePath(*interp, args[0].asString());

        std::error_code ec;
        bool removed = fs::remove(p, ec);
        if (ec) throwError("fs_remove", "cannot remove '" + p.string() + "': " + ec.message());
        return Value(removed ? 1.0 : 0.0);
    });

    // fs_join(...parts: str) -> str
    // joins path components with the platform separator, same as
    // std::filesystem::path's own operator/=
    interp->bind("fs_join",
    [](const std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("fs_join", "expects at least one argument");
        std::filesystem::path p(args[0].asString());
        for (size_t i = 1; i < args.size(); ++i) p /= args[i].asString();
        return Value(p.generic_string());
    });

    // fs_basename(path: str) -> str  (final path component, e.g. "b.txt" from "a/b.txt")
    interp->bindSig("fs_basename", {pStr("path")},
    [](const std::vector<Value>& args) -> Value {
        return Value(std::filesystem::path(args[0].asString()).filename().generic_string());
    });

    // fs_dirname(path: str) -> str  (everything but the final component, "" if none)
    interp->bindSig("fs_dirname", {pStr("path")},
    [](const std::vector<Value>& args) -> Value {
        return Value(std::filesystem::path(args[0].asString()).parent_path().generic_string());
    });

    // fs_extname(path: str) -> str  (e.g. ".txt", "" if there's no extension)
    interp->bindSig("fs_extname", {pStr("path")},
    [](const std::vector<Value>& args) -> Value {
        return Value(std::filesystem::path(args[0].asString()).extension().generic_string());
    });
}
