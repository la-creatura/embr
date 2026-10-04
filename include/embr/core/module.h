#ifndef EMBR_CORE_MODULE_H
#define EMBR_CORE_MODULE_H

// shared file lookup for anything that loads a file relative to the running script: embr script modules
// (`import "foo.embr"`, load_module()) and native plugins (`import "foo"`) all search the same way, next to
// the script, in a subdirectory, or in the current working directory
// it lives here so the tree-walker, the VM and load_module() can't drift apart
// it only builds and searches candidate lists. each caller formats its own errors

#include "registry.h"
#ifdef _WIN32
#  include <windows.h>
#endif

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace embr {

inline std::filesystem::path withFileExtension(std::filesystem::path base, const std::string& ext) {
    if (base.extension() == ext) return base;
    return std::filesystem::path(base.string() + ext);
}

// directory containing the running executable, or empty if it can't be determined
inline std::filesystem::path executableDir() {
#if defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = std::filesystem::canonical(buf, ec);        // resolves symlinks and ".." in the raw path
        return (ec ? std::filesystem::path(buf) : p).parent_path();
    }
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
#endif
    return {};
}

// entries of the colon-separated (';' on Windows) EMBR_PATH environment variable
inline std::vector<std::filesystem::path> embrPathDirs() {
    std::vector<std::filesystem::path> dirs;
    const char* env = std::getenv("EMBR_PATH");
    if (!env) return dirs;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string s = env, cur;
    for (char c : s + sep) {
        if (c == sep) { if (!cur.empty()) dirs.emplace_back(cur); cur.clear(); }
        else cur += c;
    }
    return dirs;
}

// ordered candidate list for a raw import/load path:
//   absolute path          -> just that path (with ext appended if missing)
//   relative w/ a parent   -> scriptDir/<path>, exeDir/<path>, EMBR_PATH dirs/<path>, cwd/<path>
//   bare name              -> scriptDir/<name>, scriptDir/<subdir>/<name>, then the same two
//                             under exeDir and each EMBR_PATH dir, cwd/<name>, cwd/<subdir>/<name>
// cwd is searched last, kept only for back-compat
// (scriptDir candidates are skipped when scriptDir is empty, e.g. REPL input)
// rawPath is whatever the import path evaluated to at runtime, a literal or a computed string, both searched the same
inline std::vector<std::filesystem::path> extraDirs() {
    std::vector<std::filesystem::path> dirs;
    auto exe = executableDir();
    if (!exe.empty()) {
        dirs.push_back(exe);
        dirs.push_back((exe / ".." / "lib" / "embr").lexically_normal());   // an installed prefix: bin/embr next to lib/embr/{plugins,modules}
    }
    for (auto& d : embrPathDirs()) dirs.push_back(d);
    return dirs;
}

inline std::vector<std::filesystem::path> fileCandidates(
    const std::string& rawPath, const std::string& scriptDir,
    const std::string& ext, const std::string& subdir)
{
    namespace fs = std::filesystem;
    fs::path p(rawPath);
    std::vector<fs::path> cands;
    if (p.is_absolute()) {
        cands.push_back(withFileExtension(p, ext));
    } else if (p.has_parent_path()) {
        fs::path withE = withFileExtension(p, ext);
        if (!scriptDir.empty())
            cands.push_back((fs::path(scriptDir) / withE).lexically_normal());
        for (const auto& d : extraDirs()) cands.push_back((d / withE).lexically_normal());
        cands.push_back((fs::current_path() / withE).lexically_normal());
    } else {
        fs::path name = withFileExtension(p, ext);
        if (!scriptDir.empty()) {
            fs::path base(scriptDir);
            cands.push_back(base / name);
            cands.push_back(base / subdir / name);
        }
        for (const auto& d : extraDirs()) {
            cands.push_back(d / name);
            cands.push_back(d / subdir / name);
        }
        cands.push_back(fs::current_path() / name);
        cands.push_back(fs::current_path() / subdir / name);
    }
    return cands;
}

// first candidate that exists on disk, or an empty path if none do
inline std::filesystem::path findExistingCandidate(const std::vector<std::filesystem::path>& cands) {
    for (const auto& c : cands) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && !ec) return c;
    }
    return {};
}

// "\n    <candidate>" per entry, for a "tried:" error message suffix
inline std::string describeCandidates(const std::vector<std::filesystem::path>& cands) {
    std::string tried;
    for (const auto& c : cands) tried += "\n    " + c.string();
    return tried;
}

// canonicalizes p, falling back to p itself if canonicalization fails (e.g. a dangling symlink)
inline std::string canonicalOrSelf(const std::filesystem::path& p) {
    std::error_code ec;
    auto canon = std::filesystem::canonical(p, ec);
    return (ec ? p : canon).string();
}

// the one place a native plugin is located, policy-checked, opened and registered. both backends' `import`
// call it, so neither can bypass the sandbox policy (PluginPolicy). returns "" on success, else a message
// for the caller to raise with its own source location
// unrestricted: the usual search (script dir, exe dir, EMBR_PATH, cwd)
// restricted: only the executable's directory and EMBR_PATH, which the host controls. never the script's
// directory or the cwd, where a sandboxed script could have planted a same-named library
inline std::string importNativePlugin(Interpreter& interp, const std::string& rawPath) {
    std::string denied = interp.pluginPolicy.check(rawPath);
    if (!denied.empty()) return denied;

    std::vector<std::filesystem::path> cands;
    if (interp.pluginPolicy.restricted) {
        for (const auto& d : extraDirs()) {
            cands.push_back(d / (rawPath + PLUGIN_EXT));
            cands.push_back(d / "plugins" / (rawPath + PLUGIN_EXT));
        }
    } else {
        cands = fileCandidates(rawPath, interp.scriptDir, PLUGIN_EXT, "plugins");
    }

    PluginHandle handle = nullptr;
    std::string  loadedFrom;
    for (const auto& c : cands) {
        handle = pluginOpen(c.string().c_str());
        if (handle) { loadedFrom = c.string(); break; }
    }
    if (!handle)
        return "cannot load plugin \"" + rawPath + "\": " + pluginError() + "\n  tried:" + describeCandidates(cands);

    using RegFn = void(*)(Interpreter*);
    auto reg = reinterpret_cast<RegFn>(pluginSym(handle, "embr_register"));
    if (!reg) {
        pluginClose(handle);
        return "'embr_register' not found in \"" + loadedFrom + "\"";
    }
    try {
        reg(&interp);
        interp.pluginHandles_.push_back(handle);
    } catch (...) {
        pluginClose(handle);
        throw;
    }
    std::cerr << "[runtime] loaded plugin: " << loadedFrom << "\n";
    return "";
}

// reads resolved's full contents. caller checks the returned optional and
// raises its own "cannot open file" error on nullopt (kept out of this
// header for the same reason as the rest of it, see file header comment)
inline std::optional<std::string> tryReadFile(const std::filesystem::path& resolved) {
    std::ifstream f(resolved);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

// after a module's statements have run inside a pushed module scope (see Interpreter::pushModuleScope/
// popModuleScope), decides what the importer gets
//
// mutateCallerScope=true (the `import` statement): merges every non-local top-level name into the caller's
// current scope, a side effect like a plugin's embr_register binding functions into globals
//
// mutateCallerScope=false (load_module()): collects those names into a map and returns it, leaving the
// caller's scope alone, like ffi_open() handing back a handle
inline Value exportModuleScope(Interpreter& interp, Interpreter::ModuleScopeResult&& res,
                               bool mutateCallerScope) {
    if (mutateCallerScope) {
        for (auto& [name, val] : res.scope)
            if (!res.locals.count(name))
                interp.define(name, std::move(val));
        return Value(0.0);
    }
    Value::map_type m;
    for (auto& [name, val] : res.scope)
        if (!res.locals.count(name))
            m[name] = std::move(val);
    return Value(std::move(m));
}

} // namespace embr

#endif // EMBR_CORE_MODULE_H
