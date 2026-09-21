// embrffi.cpp
// foreign function interface plugin for embr
//
// usage
//   import "embrffi"
//

#include <embr/embr.h>
#include <ffi.h>
#include <cstring>
#include <cstdlib>

using namespace embr;

static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pMap(std::string n)                      { return Param::req(std::move(n), TS::Map); }
static Param pFn (std::string n)                      { return Param::req(std::move(n), TS::Fn);  }
static Param pPtr(std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

// allocated once at registration time, captured by shared_ptr into every bound lambda
// lives as long as global scope

struct FFIState {

    struct LibEntry {
        PluginHandle handle;
        std::string  path;
    };

    std::unordered_map<uintptr_t, LibEntry> libs;

    // ffi_ref out-parameter buffers
    struct RefEntry {
        std::vector<uint8_t> buf;
        Value                desc;  // type descriptor
    };
    std::unordered_map<uint64_t, std::unique_ptr<RefEntry>> refs;
    uint64_t nextRefKey = 1;

    // libffi callback closure fwd declare
    struct CallbackEntry;
    std::unordered_map<uint64_t, std::unique_ptr<CallbackEntry>> callbacks;
    uint64_t nextCallbackKey = 1;

    // cached (prepared) calls see ffi_prepare/ffi_invoke below
    // a ffi_cif built once via ffi_prep_cif and reused across many ffi_invoke() calls instead of ffi_call() which rebuilds one every time
    struct PreparedCall;
    std::unordered_map<uint64_t, std::unique_ptr<PreparedCall>> prepared;
    uint64_t nextPreparedKey = 1;

    // destructor is defined after CallbackEntry/PreparedCall are complete
    ~FFIState();
};

// type descriptors
//   __type_desc  "yes"     detect misuse
//   name         string    readable name
//   size         number    sizeof in bytes; 0 for void
//   signed       0|1
//   float        0|1
//   pointer      0|1       true for both ptr and cstr
//   string       0|1       true only for cstr (triggers char* auto-marshal)

static Value makeTypeDesc(const std::string& name,
                          size_t sz,
                          bool isSigned,
                          bool isFloat,
                          bool isPointer,
                          bool isString)
{
    Value::map_type m;
    m["__type_desc"] = Value(std::string("yes"));
    m["name"]        = Value(name);
    m["size"]        = Value((double)sz);
    m["signed"]      = Value(isSigned  ? 1.0 : 0.0);
    m["float"]       = Value(isFloat   ? 1.0 : 0.0);
    m["pointer"]     = Value(isPointer ? 1.0 : 0.0);
    m["string"]      = Value(isString  ? 1.0 : 0.0);
    return Value(std::move(m));
}

static void registerTypeConstants(Interpreter& interp) {
    interp.define("ffi_i8",    makeTypeDesc("i8",   1, true,  false, false, false));
    interp.define("ffi_i16",   makeTypeDesc("i16",  2, true,  false, false, false));
    interp.define("ffi_i32",   makeTypeDesc("i32",  4, true,  false, false, false));
    interp.define("ffi_i64",   makeTypeDesc("i64",  8, true,  false, false, false));
    interp.define("ffi_u8",    makeTypeDesc("u8",   1, false, false, false, false));
    interp.define("ffi_u16",   makeTypeDesc("u16",  2, false, false, false, false));
    interp.define("ffi_u32",   makeTypeDesc("u32",  4, false, false, false, false));
    interp.define("ffi_u64",   makeTypeDesc("u64",  8, false, false, false, false));

    interp.define("ffi_size_t",  makeTypeDesc("size_t",  sizeof(size_t),        false, false, false, false));
    interp.define("ffi_ssize_t", makeTypeDesc("ssize_t", sizeof(ptrdiff_t),     true,  false, false, false));
    interp.define("ffi_long",    makeTypeDesc("long",    sizeof(long),           true,  false, false, false));
    interp.define("ffi_ulong",   makeTypeDesc("ulong",   sizeof(unsigned long),  false, false, false, false));
    interp.define("ffi_intptr",  makeTypeDesc("intptr",  sizeof(intptr_t),       true,  false, false, false));
    interp.define("ffi_uintptr", makeTypeDesc("uintptr", sizeof(uintptr_t),      false, false, false, false));

    interp.define("ffi_f32",   makeTypeDesc("f32",  4, true, true,  false, false));
    interp.define("ffi_f64",   makeTypeDesc("f64",  8, true, true,  false, false));

    interp.define("ffi_ptr",   makeTypeDesc("ptr",  sizeof(void*), false, false, true,  false));
    interp.define("ffi_cstr",  makeTypeDesc("cstr", sizeof(void*), false, false, true,  true ));

    interp.define("ffi_void",  makeTypeDesc("void", 0, false, false, false, false));
}

static void requireTypeDesc(const Value& v,
                             const std::string& caller,
                             const std::string& argname)
{
    if (!v.isMap() || !v.asMap().count("__type_desc"))
        raiseError(caller, "'" + argname + "' must be a type descriptor "
                   "(ffi_i32, ffi_ptr, ffi_void, ...) got " + v.typeName());
}

// resolved, POD-only view of a type descriptor.
// the embr-level type descriptors above are plain maps and reading them is five string-keyed hash lookups every time
// TypeInfo is resolved from a descriptor exactly once and from then on marshalArg/unmarshalReturn only touch plain struct fields.
struct TypeInfo {
    std::string name;
    size_t      size      = 0;
    bool        isSigned  = false;
    bool        isFloat   = false;
    bool        isPointer = false;
    bool        isString  = false;
};

static TypeInfo resolveTypeInfo(const Value::map_type& desc) {
    return TypeInfo{
        desc.at("name").asString(),
        (size_t)desc.at("size").asNumber(),
        desc.at("signed").truthy(),
        desc.at("float").truthy(),
        desc.at("pointer").truthy(),
        desc.at("string").truthy()
    };
}

static ffi_type* descToFFIType(const TypeInfo& info) {
    size_t sz     = info.size;
    bool isFloat  = info.isFloat;
    bool isSigned = info.isSigned;
    bool isPtr    = info.isPointer;

    if (sz == 0)            return &ffi_type_void;
    if (isPtr)              return &ffi_type_pointer;
    if (isFloat && sz == 4) return &ffi_type_float;
    if (isFloat && sz == 8) return &ffi_type_double;

    if (isSigned) {
        switch (sz) {
            case 1: return &ffi_type_sint8;
            case 2: return &ffi_type_sint16;
            case 4: return &ffi_type_sint32;
            case 8: return &ffi_type_sint64;
        }
    } else {
        switch (sz) {
            case 1: return &ffi_type_uint8;
            case 2: return &ffi_type_uint16;
            case 4: return &ffi_type_uint32;
            case 8: return &ffi_type_uint64;
        }
    }
    raiseError("ffi", "no libffi type for descriptor '" + info.name + "'");
}

// raw memory -> Value
// used for ffi_call/ffi_invoke return values, ffi_deref, ptr_read, and
// unmarshalling callback arguments
//
// libffi promotes integer return values smaller than ffi_arg (register width) into full ffi_arg-sized slot
// must read ffi_arg and then narrow rather than reading only sz bytes
// otherwise get garbage on big-endian platforms and potential reads of uninitialised memory on little-endian ones
static Value unmarshalReturn(const TypeInfo& info, const void* buf) {
    size_t sz     = info.size;
    bool isFloat  = info.isFloat;
    bool isSigned = info.isSigned;
    bool isPtr    = info.isPointer;
    bool isStr    = info.isString;

    if (sz == 0)  return Value(0.0);  // void

    if (isStr) {
        const char* s; memcpy(&s, buf, sizeof(char*));
        return Value(s ? std::string(s) : std::string(""));
    }

    if (isPtr) {
        void* p; memcpy(&p, buf, sizeof(void*));
        return Value(p);
    }

    if (isFloat) {
        if (sz == 4) { float  f; memcpy(&f, buf, 4); return Value((double)f); }
        else         { double f; memcpy(&f, buf, 8); return Value(f); }
    }

    if (isSigned) {
        switch (sz) {
            case 1: { ffi_sarg v; memcpy(&v, buf, sizeof(v)); return Value((double)(int8_t)v);  }
            case 2: { ffi_sarg v; memcpy(&v, buf, sizeof(v)); return Value((double)(int16_t)v); }
            case 4: { int32_t  v; memcpy(&v, buf, 4);         return Value((double)v); }
            case 8: { int64_t  v; memcpy(&v, buf, 8);         return Value((double)v); }
        }
    } else {
        switch (sz) {
            case 1: { ffi_arg  v; memcpy(&v, buf, sizeof(v)); return Value((double)(uint8_t)v);  }
            case 2: { ffi_arg  v; memcpy(&v, buf, sizeof(v)); return Value((double)(uint16_t)v); }
            case 4: { uint32_t v; memcpy(&v, buf, 4);         return Value((double)v); }
            case 8: { uint64_t v; memcpy(&v, buf, 8);         return Value((double)v); }
        }
    }
    raiseError("ffi", "unmarshal: unsupported size " + std::to_string(sz));
}

// Value -> raw memory slot
//
// slot points to kSlotSize bytes of pre-zeroed storage owned by the caller's argStorage vector
// the vector must not reallocate between this call and ffi_call
// keepalive receives any heap allocations (cstrings) that must outlive the call and the caller frees them afterwards
//
// context is the calling native fn's name ("ffi_call" / "ffi_invoke"), used
// only to label error messages correctly for whichever path is calling in
static constexpr size_t kSlotSize = 8;  // sizeof largest scalar aligns to pointer

static void marshalArg(const std::string&     context,
                       const Value&            val,
                       const TypeInfo&         info,
                       void*                   slot,
                       std::vector<void*>&     keepalive,
                       const FFIState&         ffi_state,
                       size_t                  argIdx)
{
    const std::string  argLabel = "arg[" + std::to_string(argIdx) + "]";
    const std::string& tname    = info.name;
    size_t sz      = info.size;
    bool isFloat   = info.isFloat;
    bool isSigned  = info.isSigned;
    bool isPtr     = info.isPointer;
    bool isStr     = info.isString;

    // ref passed as out-parameter pointer
    // must be checked before the generic pointer path so can look up the actual buffer address
    if (val.isMap()) {
        const auto& vm = val.asMap();
        if (vm.count("__type") && vm.at("__type").asString() == "ref") {
            if (!isPtr)
                raiseError(context, argLabel + ": passing a ref requires a pointer "
                           "type descriptor, got " + tname);
            uint64_t key = (uint64_t)vm.at("__key").asNumber();
            auto it = ffi_state.refs.find(key);
            if (it == ffi_state.refs.end())
                raiseError(context, argLabel + ": ref has been freed");
            void* bufptr = it->second->buf.data();
            memcpy(slot, &bufptr, sizeof(void*));
            return;
        }
    }

    // str -> malloc'd null-terminated buffer
    if (isStr) {
        if (!val.isString())
            raiseError(context, argLabel + ": expects cstr, got " + val.typeName());
        const std::string& s = val.asString();
        char* buf = (char*)malloc(s.size() + 1);
        if (!buf) raiseError(context, argLabel + ": malloc failed for cstr argument");
        memcpy(buf, s.data(), s.size() + 1);
        keepalive.push_back(buf);
        memcpy(slot, &buf, sizeof(void*));
        return;
    }

    if (isPtr) {
        if (!val.isPointer())
            raiseError(context, argLabel + ": expects ptr, got " + val.typeName());
        void* p = val.asPointer();
        memcpy(slot, &p, sizeof(void*));
        return;
    }

    if (isFloat) {
        if (!val.isNumber())
            raiseError(context, argLabel + ": expects " + tname + ", got " + val.typeName());
        double d = val.asNumber();
        if (sz == 4) { float f = (float)d; memcpy(slot, &f, 4); }
        else         {                     memcpy(slot, &d, 8); }
        return;
    }

    // integer
    if (!val.isNumber())
        raiseError(context, argLabel + ": expects " + tname + ", got " + val.typeName());
    double d  = val.asNumber();
    int64_t iv = (int64_t)d;
    if ((double)iv != d)
        raiseError(context, argLabel + ": type " + tname +
                   " is integer but got non-integer value " + std::to_string(d));
    if (!isSigned && d < 0.0)
        raiseError(context, argLabel + ": type " + tname +
                   " is unsigned but got negative value");
    switch (sz) {
        case 1: { uint8_t  v=(uint8_t) iv; memcpy(slot,&v,1); } break;
        case 2: { uint16_t v=(uint16_t)iv; memcpy(slot,&v,2); } break;
        case 4: { uint32_t v=(uint32_t)iv; memcpy(slot,&v,4); } break;
        case 8: { uint64_t v=(uint64_t)iv; memcpy(slot,&v,8); } break;
        default: raiseError(context, argLabel + ": unsupported integer size " +
                            std::to_string(sz));
    }
}

// shared marshal -> ffi_call -> unmarshal sequence used by both the one-shot ffi_call() and the cached ffi_invoke() paths
//
// ffi_call() builds a fresh cif + TypeInfo array right before calling this
// ffi_invoke() passes in the cif + TypeInfo array cached by ffi_prepare()
static Value performCall(const std::string&           context,
                         ffi_cif&                      cif,
                         void*                          fnptr,
                         const std::vector<TypeInfo>&   argInfo,
                         const TypeInfo&                retInfo,
                         const Value::array_type&       argVals,
                         const FFIState&                ffi_state)
{
    size_t nargs = argInfo.size();
    if (argVals.size() != nargs)
        raiseError(context, "expected " + std::to_string(nargs) +
                   " args, got " + std::to_string(argVals.size()));

    // argStorage is allocated up front so it never reallocates
    // argptrs array holds pointers into it and must stay valid until after ffi_call returns
    std::vector<uint8_t> argStorage(nargs * kSlotSize, 0);
    std::vector<void*>   argptrs(nargs);
    std::vector<void*>   keepalive;

    for (size_t i = 0; i < nargs; ++i) {
        argptrs[i] = argStorage.data() + i * kSlotSize;
        marshalArg(context, argVals[i], argInfo[i], argptrs[i], keepalive, ffi_state, i);
    }

    // the return buffer must be at least sizeof(ffi_arg) bytes because libffi promotes small integer returns into a register-sized slot
    // 16 bytes covers all scalars including double and pointer
    uint8_t retbuf[16] = {};
    ffi_call(&cif, FFI_FN(fnptr), retbuf, nargs > 0 ? argptrs.data() : nullptr);

    for (void* p : keepalive) free(p);

    return unmarshalReturn(retInfo, retbuf);
}

// placed after marshalArg/unmarshalReturn so the trampoline below can call them
// stored as unique_ptr<CallbackEntry> so its address is stable across map rehashes
// the trampoline receives a raw pointer to it as userdata
struct FFIState::CallbackEntry {
    ffi_cif                cif;
    ffi_closure*           closure  = nullptr;  // writable page
    void*                  fnptr    = nullptr;  // executable page
    std::vector<ffi_type*> argFFITypes;
    std::vector<TypeInfo>  argInfo;             // resolved once at ffi_callback() time
    TypeInfo                retInfo;
    Value                  embrFn;              // keeps the callable alive
    Interpreter*           interp   = nullptr;
};

// a symbol + cif prepared once by ffi_prepare(), invoked repeatedly by ffi_invoke() without re-resolving descriptors or calling ffi_prep_cif again
// argFFITypes must stay alive and at a stable address for as long as cif does
// ffi_prep_cif stores a pointer into it — which is guaranteed here since both live inside the same heap-allocated PreparedCall
struct FFIState::PreparedCall {
    ffi_cif                cif{};
    void*                  fnptr = nullptr;
    std::vector<ffi_type*> argFFITypes;
    std::vector<TypeInfo>  argInfo;
    ffi_type*              retFFIType = nullptr;
    TypeInfo                retInfo;
    std::string             symName;   // for error messages
};

// out of line destructor
FFIState::~FFIState() {
    for (auto& [k, e] : libs)
        pluginClose(e.handle);
    for (auto& [k, e] : callbacks)
        if (e && e->closure) ffi_closure_free(e->closure);
    // refs and prepared calls clean up automatically via unique_ptr + map/vector destructors
    // their ffi_type* members point at libffi's static built-in types, nothing to free
}

// the actual C function pointer libffi installs
//
// must be a plain extern C function
// userdata is the heap-stable raw pointer to CallbackEntry
static void ffiClosureTrampoline(ffi_cif*  /*cif*/,
                                 void*     retbuf,
                                 void**    argptrs,
                                 void*     userdata)
{
    auto* entry = static_cast<FFIState::CallbackEntry*>(userdata);

    std::vector<Value> embrArgs;
    embrArgs.reserve(entry->argInfo.size());
    for (size_t i = 0; i < entry->argInfo.size(); ++i)
        embrArgs.push_back(unmarshalReturn(entry->argInfo[i], argptrs[i]));

    Runner r(*entry->interp);
    Value result = r.invoke(entry->embrFn, embrArgs);

    // marshal the return value into retbuf.
    size_t rsz = entry->retInfo.size;
    if (rsz == 0)   return;   // void

    bool isPtr   = entry->retInfo.isPointer;
    bool isFloat = entry->retInfo.isFloat;

    if (isPtr) {
        void* p = result.isPointer() ? result.asPointer() : nullptr;
        memcpy(retbuf, &p, sizeof(void*));
    } else if (isFloat && rsz == 4) {
        float f = (float)result.asNumber(); memcpy(retbuf, &f, 4);
    } else if (isFloat) {
        double f = result.asNumber();       memcpy(retbuf, &f, 8);
    } else {
        // integer. write into an ffi_arg slot. libffi reads the right width.
        int64_t iv = (int64_t)result.asNumber();
        memcpy(retbuf, &iv, sizeof(iv));
    }
}

static PluginHandle unwrapLib(const Value&       v,
                              const FFIState&    st,
                              const std::string& caller)
{
    if (!v.isMap())
        raiseError(caller, "expected a library handle (from ffi_open), got " + v.typeName());
    const auto& m = v.asMap();
    auto t = m.find("__type");
    if (t == m.end() || t->second.asString() != "lib")
        raiseError(caller, "expected a library handle — got a different map "
                   "(sym handle? plain map?)");
    uintptr_t key = (uintptr_t)m.at("__key").asNumber();
    auto it = st.libs.find(key);
    if (it == st.libs.end())
        raiseError(caller, "library '" + m.at("__path").asString() +
                   "' has already been closed");
    return it->second.handle;
}

static void* unwrapSym(const Value& v, const std::string& caller) {
    if (!v.isMap())
        raiseError(caller, "expected a symbol handle (from ffi_sym), got " + v.typeName());
    const auto& m = v.asMap();
    auto t = m.find("__type");
    if (t == m.end() || t->second.asString() != "sym")
        raiseError(caller, "expected a symbol handle — got a different map "
                   "(lib handle? plain map?)");
    return m.at("__fnptr").asPointer();
}


EMBR_PLUGIN {

    auto ffi = std::make_shared<FFIState>();

    registerTypeConstants(*interp);

    // ffi_open(path: str) -> map
    //
    // opens a shared library via pluginOpen()
    // LoadLibraryA on windows dlopen(RTLD_NOW|RTLD_LOCAL) on POSIX
    // RTLD_NOW resolves all symbols immediately so missing-symbol errors surface at load time
    interp->bindSig("ffi_open", {pStr("path")},
    [ffi](const std::vector<Value>& args) -> Value {
        const std::string& path = args[0].asString();

        PluginHandle handle = pluginOpen(path.c_str());
        if (!handle)
            raiseError("ffi_open", "cannot open '" + path + "': " + pluginError());

        uintptr_t key = reinterpret_cast<uintptr_t>(handle);
        ffi->libs[key] = { handle, path };

        Value::map_type m;
        m["__type"] = Value(std::string("lib"));
        m["__key"]  = Value((double)key);
        m["__path"] = Value(path);
        return Value(std::move(m));
    });

    // ffi_close(lib: map)
    //
    // explicitly closes a library
    // sym handles derived from it become dangling
    // the caller must not use them afterwards
    // libraries not explicitly closed are closed in ~FFIState
    interp->bindSig("ffi_close", {pMap("lib")},
    [ffi](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        if (!m.count("__type") || m.at("__type").asString() != "lib")
            raiseError("ffi_close", "argument must be a library handle (from ffi_open)");
        uintptr_t key = (uintptr_t)m.at("__key").asNumber();
        auto it = ffi->libs.find(key);
        if (it == ffi->libs.end())
            raiseError("ffi_close", "library '" + m.at("__path").asString() +
                       "' is not open (already closed?)");
        pluginClose(it->second.handle);
        ffi->libs.erase(it);
        return Value(0.0);
    });

    // ffi_sym(lib: map, name: str) -> map
    //
    // looks up a symbol via pluginSym()
    // GetProcAddress on windows dlsym on POSIX
    interp->bindSig("ffi_sym", {pMap("lib"), pStr("name")},
    [ffi](const std::vector<Value>& args) -> Value {
        PluginHandle handle = unwrapLib(args[0], *ffi, "ffi_sym");
        const std::string& name = args[1].asString();
        const std::string& path = args[0].asMap().at("__path").asString();

        void* sym = nullptr;

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
        dlerror();  // clear any previous error
        sym = pluginSym(handle, name.c_str());
        const char* err = dlerror();
        if (err)
            raiseError("ffi_sym", "symbol '" + name + "' not found in '" +
                       path + "': " + err);
#else
        sym = pluginSym(handle, name.c_str());
        if (!sym)
            raiseError("ffi_sym", "symbol '" + name + "' not found in '" +
                       path + "': " + pluginError());
#endif

        Value::map_type m;
        m["__type"]  = Value(std::string("sym"));
        m["__name"]  = Value(name);
        m["__lib"]   = Value(path);
        m["__fnptr"] = Value(sym);
        return Value(std::move(m));
    });

    // ffi_call(sym: map, ret: map, arg_types: arr, args: arr) -> any
    //
    // marshals args, builds a libffi CIF, calls the function, and unmarshals the return value
    // this rebuilds the cif and re-resolves every type descriptor on EVERY call. that is fine for one-off calls, wasteful in a loop
    // for repeated calls to the same symbol use ffi_prepare()/ffi_invoke() instead, which does this work once
    // a ref map may appear in args wherever a pointer type descriptor is given
    // its buffer address is passed as the out-parameter
    interp->bindSig("ffi_call",
        {pMap("sym"), pMap("ret"), pArr("arg_types"), pArr("args")},
    [ffi](const std::vector<Value>& args) -> Value {
        void* fnptr = unwrapSym(args[0], "ffi_call");
        requireTypeDesc(args[1], "ffi_call", "ret");

        const auto& argTypeVals = args[2].asArray();
        const auto& argVals     = args[3].asArray();

        if (argTypeVals.size() != argVals.size())
            raiseError("ffi_call",
                "arg_types has " + std::to_string(argTypeVals.size()) +
                " entries but args has " + std::to_string(argVals.size()));

        size_t nargs = argTypeVals.size();

        // resolve descriptors + build the libffi type array
        std::vector<TypeInfo>  argInfo(nargs);
        std::vector<ffi_type*> ffiArgTypes(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            requireTypeDesc(argTypeVals[i], "ffi_call",
                            "arg_types[" + std::to_string(i) + "]");
            argInfo[i]     = resolveTypeInfo(argTypeVals[i].asMap());
            ffiArgTypes[i] = descToFFIType(argInfo[i]);
        }
        TypeInfo  retInfo = resolveTypeInfo(args[1].asMap());
        ffi_type* ffiRet  = descToFFIType(retInfo);

        // prepare the call interface
        // this plus the descriptor resolution above is exactly the per-call cost ffi_prepare/ffi_invoke avoid
        ffi_cif cif;
        ffi_status status = ffi_prep_cif(
            &cif,
            FFI_DEFAULT_ABI,
            (unsigned int)nargs,
            ffiRet,
            nargs > 0 ? ffiArgTypes.data() : nullptr
        );
        if (status != FFI_OK)
            raiseError("ffi_call", "ffi_prep_cif failed (status=" +
                       std::to_string((int)status) + ")");

        return performCall("ffi_call", cif, fnptr, argInfo, retInfo, argVals, *ffi);
    });

    // ffi_prepare(sym: map, ret: map, arg_types: arr) -> map
    //
    // resolves the return + argument type descriptors and calls ffi_prep_cif ONCE, caching the resulting cif and the resolved TypeInfo array in FFIState behind an opaque handle
    // pass that handle to ffi_invoke() to run the call without repeating any of this setup
    // use this instead of ffi_call() whenever the same symbol is invoked repeatedly since ffi_call() redoes the descriptor resolution and ffi_prep_cif on every single invocation
    interp->bindSig("ffi_prepare",
        {pMap("sym"), pMap("ret"), pArr("arg_types")},
    [ffi](const std::vector<Value>& args) -> Value {
        void* fnptr = unwrapSym(args[0], "ffi_prepare");
        requireTypeDesc(args[1], "ffi_prepare", "ret");

        const auto& argTypeVals = args[2].asArray();
        size_t nargs = argTypeVals.size();

        auto prep = std::make_unique<FFIState::PreparedCall>();
        prep->fnptr   = fnptr;
        prep->symName = args[0].asMap().at("__name").asString();

        prep->argInfo.resize(nargs);
        prep->argFFITypes.resize(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            requireTypeDesc(argTypeVals[i], "ffi_prepare",
                            "arg_types[" + std::to_string(i) + "]");
            prep->argInfo[i]     = resolveTypeInfo(argTypeVals[i].asMap());
            prep->argFFITypes[i] = descToFFIType(prep->argInfo[i]);
        }
        prep->retInfo    = resolveTypeInfo(args[1].asMap());
        prep->retFFIType = descToFFIType(prep->retInfo);

        ffi_status status = ffi_prep_cif(
            &prep->cif,
            FFI_DEFAULT_ABI,
            (unsigned int)nargs,
            prep->retFFIType,
            nargs > 0 ? prep->argFFITypes.data() : nullptr
        );
        if (status != FFI_OK)
            raiseError("ffi_prepare", "ffi_prep_cif failed (status=" +
                       std::to_string((int)status) + ")");

        uint64_t key = ffi->nextPreparedKey++;
        ffi->prepared[key] = std::move(prep);

        Value::map_type m;
        m["__type"] = Value(std::string("prepared"));
        m["__key"]  = Value((double)key);
        m["__sym"]  = args[0].asMap().at("__name");
        return Value(std::move(m));
    });

    // ffi_invoke(prepared: map, args: arr) -> any
    //
    // runs a call built by ffi_prepare()
    // only marshals args and calls ffi_call() itself. no cif prep, no descriptor hash lookups
    // this is the fast path for repeated calls to the same symbol
    interp->bindSig("ffi_invoke", {pMap("prepared"), pArr("args")},
    [ffi](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        if (!m.count("__type") || m.at("__type").asString() != "prepared")
            raiseError("ffi_invoke", "argument must be a prepared call (from ffi_prepare)");
        uint64_t key = (uint64_t)m.at("__key").asNumber();
        auto it = ffi->prepared.find(key);
        if (it == ffi->prepared.end())
            raiseError("ffi_invoke", "prepared call '" + m.at("__sym").asString() +
                       "' has been freed");
        auto& prep = *it->second;

        return performCall("ffi_invoke", prep.cif, prep.fnptr,
                           prep.argInfo, prep.retInfo, args[1].asArray(), *ffi);
    });

    // ffi_prepared_free(prepared: map)
    //
    // releases a prepared call.
    // does not touch the underlying symbol or library. those are independent handles and can outlive this one
    // not required at process exit (~FFIState cleans up automatically) but useful for long-running scripts that build many prepared calls
    interp->bindSig("ffi_prepared_free", {pMap("prepared")},
    [ffi](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        if (!m.count("__type") || m.at("__type").asString() != "prepared")
            raiseError("ffi_prepared_free", "argument must be a prepared call (from ffi_prepare)");
        uint64_t key = (uint64_t)m.at("__key").asNumber();
        if (!ffi->prepared.erase(key))
            raiseError("ffi_prepared_free", "prepared call has already been freed");
        return Value(0.0);
    });

    // ffi_ref(desc: map) -> map
    //
    // allocates a zeroed buffer sized for the given C type and registers it
    // pass the returned map to ffi_call wherever a pointer type descriptor appears to use it as an out-parameter
    // read the written value back with ffi_deref
    // release with ffi_ref_free when done
    interp->bindSig("ffi_ref", {pMap("desc")},
    [ffi](const std::vector<Value>& args) -> Value {
        requireTypeDesc(args[0], "ffi_ref", "desc");
        size_t sz = (size_t)args[0].asMap().at("size").asNumber();
        if (sz == 0)
            raiseError("ffi_ref", "cannot create a ref to void");

        uint64_t key = ffi->nextRefKey++;
        ffi->refs[key] = std::make_unique<FFIState::RefEntry>(
            FFIState::RefEntry{ std::vector<uint8_t>(sz, 0), args[0] }
        );

        Value::map_type m;
        m["__type"] = Value(std::string("ref"));
        m["__key"]  = Value((double)key);
        m["__desc"] = args[0];
        return Value(std::move(m));
    });

    // ffi_deref(ref: map) -> any
    //
    // reads the current value of a ref buffer and returns it as a Value
    interp->bindSig("ffi_deref", {pMap("ref")},
    [ffi](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        if (!m.count("__type") || m.at("__type").asString() != "ref")
            raiseError("ffi_deref", "argument must be a ref (from ffi_ref)");
        uint64_t key = (uint64_t)m.at("__key").asNumber();
        auto it = ffi->refs.find(key);
        if (it == ffi->refs.end())
            raiseError("ffi_deref", "ref has been freed");
        return unmarshalReturn(resolveTypeInfo(it->second->desc.asMap()), it->second->buf.data());
    });

    // ffi_ref_free(ref: map)
    //
    // explicitly releases a ref buffer
    // subsequent ffi_deref or use as an ffi_call argument will raise an error
    interp->bindSig("ffi_ref_free", {pMap("ref")},
    [ffi](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        if (!m.count("__type") || m.at("__type").asString() != "ref")
            raiseError("ffi_ref_free", "argument must be a ref (from ffi_ref)");
        uint64_t key = (uint64_t)m.at("__key").asNumber();
        if (!ffi->refs.erase(key))
            raiseError("ffi_ref_free", "ref has already been freed");
        return Value(0.0);
    });

    // ffi_callback(fn: fn, ret: map, arg_types: arr) -> map
    //
    // synthesises a real C function pointer that when called by C code invokes fn with arguments unmarshalled from C
    //
    // libffi closures use a two-pointer pattern for w^x page protection
    //
    // ffi_closure_alloc     returns a writable address for setup
    // ffi_prep_closure_loc  fills in an executable address for passing to C
    //
    // the closure entry is owned by FFIState::callbacks as a unique_ptr so its is stable for the trampoline's userdata pointer even if the map rehashes
    // release with ffi_callback_free when the C library no longer holds the pointer
    interp->bindSig("ffi_callback",
        {pFn("fn"), pMap("ret"), pArr("arg_types")},
    [ffi, interp](const std::vector<Value>& args) -> Value {
        requireTypeDesc(args[1], "ffi_callback", "ret");

        const auto& argTypeVals = args[2].asArray();
        size_t nargs = argTypeVals.size();
        for (size_t i = 0; i < nargs; ++i)
            requireTypeDesc(argTypeVals[i], "ffi_callback",
                            "arg_types[" + std::to_string(i) + "]");

        uint64_t key   = ffi->nextCallbackKey++;
        auto     entry = std::make_unique<FFIState::CallbackEntry>();

        entry->embrFn  = args[0];
        entry->retInfo = resolveTypeInfo(args[1].asMap());
        entry->interp  = interp;
        entry->argInfo.resize(nargs);
        entry->argFFITypes.resize(nargs);

        for (size_t i = 0; i < nargs; ++i) {
            entry->argInfo[i]     = resolveTypeInfo(argTypeVals[i].asMap());
            entry->argFFITypes[i] = descToFFIType(entry->argInfo[i]);
        }

        ffi_type* retType = descToFFIType(entry->retInfo);

        // allocate writable page. execPtr is the matching executable address
        void* execPtr = nullptr;
        entry->closure = static_cast<ffi_closure*>(
            ffi_closure_alloc(sizeof(ffi_closure), &execPtr)
        );
        if (!entry->closure)
            raiseError("ffi_callback", "ffi_closure_alloc failed");

        ffi_status s = ffi_prep_cif(
            &entry->cif,
            FFI_DEFAULT_ABI,
            (unsigned int)nargs,
            retType,
            nargs > 0 ? entry->argFFITypes.data() : nullptr
        );
        if (s != FFI_OK) {
            ffi_closure_free(entry->closure);
            raiseError("ffi_callback",
                "ffi_prep_cif failed for callback (status=" + std::to_string((int)s) + ")");
        }

        // entry->get() is the stable userdata pointer the trampoline receives.
        s = ffi_prep_closure_loc(
            entry->closure,
            &entry->cif,
            ffiClosureTrampoline,
            entry.get(),
            execPtr
        );
        if (s != FFI_OK) {
            ffi_closure_free(entry->closure);
            raiseError("ffi_callback",
                "ffi_prep_closure_loc failed (status=" + std::to_string((int)s) + ")");
        }

        entry->fnptr = execPtr;

        // transfer ownership before building the return map so that any exception during map construction still runs the destructor
        FFIState::CallbackEntry* raw = entry.get();
        ffi->callbacks[key] = std::move(entry);

        Value::map_type m;
        m["__type"]  = Value(std::string("callback"));
        m["__key"]   = Value((double)key);
        m["__fnptr"] = Value(raw->fnptr);  // pass this to C
        return Value(std::move(m));
    });

    // ffi_callback_free(cb: map)
    //
    // releases the executable page and the embr function reference
    // must be called once C code no longer holds the function pointer
    interp->bindSig("ffi_callback_free", {pMap("cb")},
    [ffi](const std::vector<Value>& args) -> Value {
        const auto& m = args[0].asMap();
        if (!m.count("__type") || m.at("__type").asString() != "callback")
            raiseError("ffi_callback_free", "argument must be a callback (from ffi_callback)");
        uint64_t key = (uint64_t)m.at("__key").asNumber();
        auto it = ffi->callbacks.find(key);
        if (it == ffi->callbacks.end())
            raiseError("ffi_callback_free", "callback has already been freed");
        ffi_closure_free(it->second->closure);
        it->second->closure = nullptr;  // prevent double-free in ~FFIState
        ffi->callbacks.erase(it);       // unique_ptr destructs embrFn released
        return Value(0.0);
    });

    // ptr_null() -> ptr
    //
    // returns a null pointer value
    // there is no literal syntax for pointers in embr so this is the canonical way to produce one
    interp->bind("ptr_null",
    [](const std::vector<Value>&) -> Value {
        return Value(nullptr);
    });

    // ptr_offset(p, n: num) -> ptr
    //
    // returns a pointer displaced by n bytes
    // used to access struct fields by known offset or to step through arrays of C structs
    interp->bindSig("ptr_offset", {pPtr("p"), pOpt("n")},
    [](const std::vector<Value>& args) -> Value {
        ptrdiff_t off = args.size() > 1 ? (ptrdiff_t)args[1].asNumber() : 0;
        uint8_t* base = static_cast<uint8_t*>(args[0].asPointer());
        return Value(base + off);
    });

    // ptr_read(p, desc: map) -> any
    //
    // reads a value from raw memory at p, interpreting it according to desc
    // combine with ptr_offset to walk struct fields
    interp->bindSig("ptr_read", {pPtr("p"), pMap("desc")},
    [](const std::vector<Value>& args) -> Value {
        requireTypeDesc(args[1], "ptr_read", "desc");
        void* p = args[0].asPointer();
        if (!p) raiseError("ptr_read", "cannot read from null pointer");
        return unmarshalReturn(resolveTypeInfo(args[1].asMap()), p);
    });

    // ptr_write(p, desc: map, val: any)
    //
    // writes val into raw memory at p according to desc
    interp->bindSig("ptr_write", {pPtr("p"), pMap("desc"), pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        requireTypeDesc(args[1], "ptr_write", "desc");
        void* p = args[0].asPointer();
        if (!p) raiseError("ptr_write", "cannot write to null pointer");

        const auto& desc = args[1].asMap();
        size_t sz      = (size_t)desc.at("size").asNumber();
        bool isFloat   = desc.at("float"  ).truthy();
        bool isPtr     = desc.at("pointer").truthy();
        const Value& v = args[2];

        if (isPtr) {
            if (!v.isPointer())
                raiseError("ptr_write", "value must be a pointer for ptr type");
            void* q = v.asPointer();
            memcpy(p, &q, sizeof(void*));
        } else if (isFloat) {
            if (!v.isNumber())
                raiseError("ptr_write", "value must be a number for float type");
            double d = v.asNumber();
            if (sz == 4) { float f = (float)d; memcpy(p, &f, 4); }
            else          {                      memcpy(p, &d, 8); }
        } else {
            if (!v.isNumber())
                raiseError("ptr_write", "value must be a number for integer type");
            int64_t iv = (int64_t)v.asNumber();
            switch (sz) {
                case 1: { uint8_t  x=(uint8_t) iv; memcpy(p,&x,1); } break;
                case 2: { uint16_t x=(uint16_t)iv; memcpy(p,&x,2); } break;
                case 4: { uint32_t x=(uint32_t)iv; memcpy(p,&x,4); } break;
                case 8: { uint64_t x=(uint64_t)iv; memcpy(p,&x,8); } break;
                default: raiseError("ptr_write", "unsupported integer size " +
                                    std::to_string(sz));
            }
        }
        return Value(0.0);
    });
}