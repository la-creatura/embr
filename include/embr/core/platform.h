#ifndef EMBR_CORE_PLATFORM_H
#define EMBR_CORE_PLATFORM_H

// plugin loading + console setup
//
// pulled out on its own so headers that don't touch dynamic loading (lexer, parser, AST, Value) don't have to drag in <windows.h> or <dlfcn.h>

#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace embr {

#if defined(_WIN32)
   inline void enableAnsi()
   {
       HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
       if (h == INVALID_HANDLE_VALUE)
           return;

       DWORD mode;
       if (!GetConsoleMode(h, &mode))
           return;

       SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
   }

   using PluginHandle = HMODULE;
   inline PluginHandle pluginOpen (const char *p)                { return LoadLibraryA(p); }
   inline void        *pluginSym  (PluginHandle h, const char *s){ return (void*)GetProcAddress(h, s); }
   inline void         pluginClose(PluginHandle h)               { FreeLibrary(h); }
   inline std::string  pluginError()                             { return "LoadLibrary error " + std::to_string(GetLastError()); }
   static constexpr const char *PLUGIN_EXT = ".dll";
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
   using PluginHandle = void*;
   inline PluginHandle pluginOpen (const char *p)                { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
   inline void        *pluginSym  (PluginHandle h, const char *s){ return dlsym(h, s); }
   inline void         pluginClose(PluginHandle h)               { dlclose(h); }
   inline std::string  pluginError()                             { const char *e = dlerror(); return e ? e : "unknown error"; }
#  if defined(__APPLE__)
   static constexpr const char *PLUGIN_EXT = ".dylib";
#  else
   static constexpr const char *PLUGIN_EXT = ".so";
#  endif
#else
#  error "embr: no plugin loading backend for this platform"
#endif

} // namespace embr

#endif // EMBR_CORE_PLATFORM_H
