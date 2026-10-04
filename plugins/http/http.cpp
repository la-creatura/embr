// http.cpp
// HTTP(S) client plugin for embr, built on the vendored single-header cpp-httplib (include/httplib/httplib.h,
// MIT, v0.58.0). TLS is security-sensitive, so we wrap a well-reviewed library instead of writing sockets by hand
//
// usage
//   import "http"
//   r = http_get("http://localhost:8000/pkgs/index.json")
//   print(r["status"])   # 200
//   print(r["body"])
//
//   r = http_get("https://api.github.com/repos/owner/repo/releases/latest")
//
//   r = http_post("http://localhost:8000/echo", "hello", {"Content-Type": "text/plain"})
//
//   r = http_request("PUT", "http://localhost:8000/thing", {}, "payload")
//
//   r = http_download("https://github.com/owner/repo/releases/download/v1.0.0/asset.so", "cache/asset.so")
//   print(r["bytes"])
//
//   print(http_sha256("hello"))   # hex digest, to check a downloaded file against a published checksum (see modules/pkg.embr)
//
// every call returns a map {"status": num, "ok": num, "headers": map<str,str>, "body": str}
// (http_download returns the same without the body). "ok" is 1 for 2xx/3xx and 0 otherwise, so look at "status"
// for exact handling. connection failures, TLS failures and bad responses raise, like every other plugin
//
// good to know
//   - redirects (3xx + Location) are followed, up to 10 hops. this matters for GitHub release downloads, which
//     302 to a signed S3 url. pass {"follow": 0} in opts to get the redirect response itself (the target is in
//     its "headers"["location"])
//   - https:// needs a build where OpenSSL was found (see plugins/CMakeLists.txt; windows uses the copy in include/openssl). without it, https:// raises a clear error
//   - https:// checks the certificate and hostname against the system CA store. opts for http_request/http_download:
//       {"verify": 0}            accept any certificate (local testing only, the connection can be intercepted)
//       {"ca_file": "path.pem"}  trust this PEM bundle instead (pin a private CA without turning verification off)
//     (http_get/http_post take no opts map, use http_request)
//   - 10 second socket timeout for connect/read/write. override it with {"timeout": seconds} in http_request opts
//   - every call blocks the calling thread until it finishes. plugins don't depend on each other, so this one doesn't use async

#include <embr/embr.h>

#ifdef _WIN32
#  define CPPHTTPLIB_NO_EXCEPTIONS 0
#endif
// the redirect cap this file's header documents ("up to 10 hops"); httplib's own default is 20
#ifndef CPPHTTPLIB_REDIRECT_MAX_COUNT
#  define CPPHTTPLIB_REDIRECT_MAX_COUNT 10
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOptStr(std::string n)                    { return Param::opt(std::move(n), TS::Str); }
static Param pOptMap(std::string n)                    { return Param::opt(std::move(n), TS::Map); }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[http:" + fn + "]", msg);
}

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct UrlParts {
    std::string scheme;
    std::string schemeHostPort; // e.g. "https://api.github.com" or "http://localhost:8931"
    std::string target;         // path + query, always starts with '/'
};

// splits "scheme://host[:port][/path[?query]]" into what httplib::Client wants (scheme + authority) and what
// Get/Post/send want (path + query). httplib validates the authority itself, so this only finds the boundary
UrlParts splitUrl(const std::string& fn, const std::string& raw) {
    auto schemeEnd = raw.find("://");
    if (schemeEnd == std::string::npos)
        throwError(fn, "invalid URL (missing scheme): " + raw);

    UrlParts u;
    u.scheme = toLower(raw.substr(0, schemeEnd));

    std::string rest = raw.substr(schemeEnd + 3);
    auto pathStart = rest.find('/');
    std::string authority = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    u.target = (pathStart == std::string::npos) ? "/" : rest.substr(pathStart);
    if (u.target.empty()) u.target = "/";

    if (authority.empty())
        throwError(fn, "invalid URL (missing host): " + raw);

    u.schemeHostPort = u.scheme + "://" + authority;
    return u;
}

double timeoutFromOpts(const Value* opts) {
    if (opts && opts->isMap()) {
        auto it = opts->asMap().find("timeout");
        if (it != opts->asMap().end()) return it->second.asNumber();
    }
    return 10.0;
}

struct TlsOpts {
    bool        verify = true;
    std::string caFile;
};

TlsOpts tlsFromOpts(const std::string& fn, const Value* opts) {
    TlsOpts t;
    if (opts && opts->isMap()) {
        const auto& m = opts->asMap();
        auto v = m.find("verify");
        if (v != m.end()) t.verify = v->second.truthy();
        auto c = m.find("ca_file");
        if (c != m.end()) {
            if (!c->second.isString()) throwError(fn, "opts.ca_file must be a string path");
            t.caFile = c->second.asString();
        }
    }
    return t;
}

bool followFromOpts(const Value* opts) {
    if (opts && opts->isMap()) {
        auto it = opts->asMap().find("follow");
        if (it != opts->asMap().end()) return it->second.truthy();
    }
    return true;
}

std::filesystem::path resolvePath(Interpreter& interp, const std::string& raw) {
    namespace fs = std::filesystem;
    fs::path p(raw);
    if (p.is_absolute()) return p;
    return interp.scriptDir.empty() ? (fs::current_path() / p) : (fs::path(interp.scriptDir) / p);
}

// does one request/response round trip, following redirects unless disabled. headers/body are optional (nullptr
// if not needed). if downloadTo is set, the body is streamed to that file instead of kept in memory
Value performRequest(const std::string& fn, const std::string& method, const std::string& rawUrl,
                     const Value::map_type* headerArgs, const std::string* body,
                     double timeoutSec, bool follow, const std::filesystem::path* downloadTo,
                     const TlsOpts& tls = {}) {
    UrlParts u = splitUrl(fn, rawUrl);

    if (u.scheme != "http" && u.scheme != "https")
        throwError(fn, "unsupported URL scheme '" + u.scheme + "' (only http:// and https:// are supported): " + rawUrl);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (u.scheme == "https")
        throwError(fn, "https:// is not supported -- this build has no TLS library linked in (OpenSSL was not found at configure time); use plain http://");
#endif

    httplib::Client cli(u.schemeHostPort);
    if (!cli.is_valid())
        throwError(fn, "invalid URL: " + rawUrl);

    long sec  = (long)timeoutSec;
    long usec = (long)((timeoutSec - (double)sec) * 1e6);
    cli.set_connection_timeout(sec, usec);
    cli.set_read_timeout(sec, usec);
    cli.set_write_timeout(sec, usec);
    cli.set_follow_location(follow);
    cli.set_keep_alive(false);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (u.scheme == "https") {
        if (!tls.caFile.empty()) {
            if (!std::filesystem::exists(tls.caFile))
                throwError(fn, "opts.ca_file does not exist: " + tls.caFile);
            cli.set_ca_cert_path(tls.caFile);
        }
        cli.enable_server_certificate_verification(tls.verify);
        cli.enable_server_hostname_verification(tls.verify);
    }
#else
    (void)tls;
#endif

    httplib::Request req;
    req.method = method;
    req.path   = u.target;

    bool hasContentType = false;
    if (headerArgs) {
        for (auto& [k, v] : *headerArgs) {
            if (toLower(k) == "content-type") hasContentType = true;
            req.headers.emplace(k, v.asString());
        }
    }
    if (body) {
        req.body = *body;
        if (!hasContentType) req.headers.emplace("Content-Type", "application/octet-stream");
    }

    httplib::Response res;
    httplib::Error err = httplib::Error::Success;
    bool sent = cli.send(req, res, err);
    if (!sent || err != httplib::Error::Success)
        throwError(fn, "request to " + rawUrl + " failed: " + httplib::to_string(err));

    Value::map_type out;
    out["status"] = Value((double)res.status);
    out["ok"]     = Value((res.status >= 200 && res.status < 400) ? 1.0 : 0.0);

    Value::map_type headersOut;
    for (auto& [k, v] : res.headers) headersOut[toLower(k)] = Value(v);
    out["headers"] = Value(std::move(headersOut));

    if (downloadTo) {
        std::ofstream f(*downloadTo, std::ios::binary | std::ios::trunc);
        if (!f) throwError(fn, "cannot open '" + downloadTo->string() + "' for writing");
        f.write(res.body.data(), (std::streamsize)res.body.size());
        if (!f) throwError(fn, "failed writing response body to '" + downloadTo->string() + "'");
        out["bytes"] = Value((double)res.body.size());
    } else {
        out["body"] = Value(std::move(res.body));
    }
    return Value(std::move(out));
}

// SHA-256, single-shot. it lives here (not in its own plugin) because the package manager built on this plugin
// needs to check downloaded artifacts against a registry checksum. independent of httplib/OpenSSL, so
// http_sha256 works the same with or without TLS
std::string sha256Hex(const std::string& msg) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };

    std::string data = msg;
    uint64_t bitLen = (uint64_t)msg.size() * 8;
    data += (char)0x80;
    while (data.size() % 64 != 56) data += (char)0x00;
    for (int i = 7; i >= 0; --i) data += (char)((bitLen >> (i * 8)) & 0xFF);

    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)(unsigned char)data[chunk + i * 4] << 24) |
                   ((uint32_t)(unsigned char)data[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)(unsigned char)data[chunk + i * 4 + 2] << 8) |
                   ((uint32_t)(unsigned char)data[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint32_t v : h)
        for (int shift = 28; shift >= 0; shift -= 4)
            out += hex[(v >> shift) & 0xF];
    return out;
}

} // namespace

EMBR_PLUGIN {

    // http_request(method: str, url: str, headers?: map, body?: str, opts?: map) -> map
    // opts: {"timeout": num seconds (default 10), "follow": num (default 1,
    // truthy = follow redirects, falsy = return the 3xx response itself),
    //        "verify": num (default 1; 0 = accept any TLS certificate),
    //        "ca_file": str (PEM bundle to trust instead of the system CA store)}
    interp->bindSig("http_request",
        {pStr("method"), pStr("url"), pOptMap("headers"), pOptStr("body"), pOptMap("opts")},
    [](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = nullptr;
        std::string bodyStorage;
        const std::string* body = nullptr;
        const Value* opts = nullptr;

        if (args.size() >= 3 && args[2].isMap()) headers = &args[2].asMap();
        if (args.size() >= 4) { bodyStorage = args[3].asString(); body = &bodyStorage; }
        if (args.size() >= 5) opts = &args[4];

        return performRequest("http_request", args[0].asString(), args[1].asString(),
                              headers, body, timeoutFromOpts(opts), followFromOpts(opts), nullptr,
                              tlsFromOpts("http_request", opts));
    });

    // http_get(url: str, headers?: map) -> map
    interp->bindSig("http_get", {pStr("url"), pOptMap("headers")},
    [](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = (args.size() >= 2 && args[1].isMap()) ? &args[1].asMap() : nullptr;
        return performRequest("http_get", "GET", args[0].asString(), headers, nullptr, 10.0, true, nullptr);
    });

    // http_post(url: str, body: str, headers?: map) -> map
    interp->bindSig("http_post", {pStr("url"), pStr("body"), pOptMap("headers")},
    [](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = (args.size() >= 3 && args[2].isMap()) ? &args[2].asMap() : nullptr;
        std::string body = args[1].asString();
        return performRequest("http_post", "POST", args[0].asString(), headers, &body, 10.0, true, nullptr);
    });

    // http_download(url: str, path: str, headers?: map, opts?: map) -> map
    // {"status", "ok", "headers", "bytes"}. writes the body straight to path (a relative path resolves against the
    // running script's directory, or cwd if there isn't one) instead of returning it as a string
    interp->bindSig("http_download", {pStr("url"), pStr("path"), pOptMap("headers"), pOptMap("opts")},
    [interp](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = (args.size() >= 3 && args[2].isMap()) ? &args[2].asMap() : nullptr;
        const Value* opts = (args.size() >= 4) ? &args[3] : nullptr;
        std::filesystem::path dest = resolvePath(*interp, args[1].asString());
        std::error_code ec;
        if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path(), ec);
        return performRequest("http_download", "GET", args[0].asString(), headers, nullptr,
                              timeoutFromOpts(opts), followFromOpts(opts), &dest,
                              tlsFromOpts("http_download", opts));
    });

    // http_url_encode(s: str) -> str  (percent-encodes everything outside [A-Za-z0-9-_.~])
    interp->bindSig("http_url_encode", {pStr("s")},
    [](const std::vector<Value>& args) -> Value {
        static const char* hex = "0123456789ABCDEF";
        const std::string& s = args[0].asString();
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += (char)c;
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return Value(std::move(out));
    });

    // http_sha256(data: str) -> str  (lowercase hex digest)
    interp->bindSig("http_sha256", {pStr("data")},
    [](const std::vector<Value>& args) -> Value {
        return Value(sha256Hex(args[0].asString()));
    });

    // http_tls_available() -> num  (1 if this build was compiled with TLS
    // support, i.e. https:// works; 0 otherwise). lets script code (e.g.
    // pkg.embr) give a clear "rebuild with OpenSSL available" message
    // itself instead of only surfacing http_get's raised error.
    interp->bind("http_tls_available",
    [](const std::vector<Value>&) -> Value {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        return Value(1.0);
#else
        return Value(0.0);
#endif
    });
}
