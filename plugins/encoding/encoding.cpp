// encoding.cpp
// byte-string encodings and digests for embr: base64, hex, sha256, crc32.
//
// usage
//   import "encoding"
//   print(base64_encode("hello"))             # aGVsbG8=
//   print(base64_decode("aGVsbG8="))          # hello
//   print(hex_encode("hi"))                   # 6869
//   print(sha256("abc"))                      # ba7816bf...
//   print(crc32("123456789"))                 # 3421780262
//
// embr strings are plain byte strings (see the str plugin's notes), so the
// decoders can return bytes that are not valid UTF-8; that's by design.
// every decoder raises on malformed input rather than returning garbage.

#include <embr/embr.h>
#include <array>
#include <cstdint>

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

[[noreturn]] static void fail(const std::string& fn, const std::string& msg) {
    raiseError("[encoding:" + fn + "]", msg);
}

// ---- base64 (RFC 4648) ----
static std::string b64Encode(const std::string& in, bool urlsafe, bool pad) {
    const char* tbl = urlsafe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                              : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8) | (uint8_t)in[i+2];
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63];
        if (pad) out += "==";
    } else if (rem == 2) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8);
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63];
        if (pad) out += '=';
    }
    return out;
}

static std::string b64Decode(const std::string& in, bool urlsafe) {
    auto val = [&](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == (urlsafe ? '-' : '+')) return 62;
        if (c == (urlsafe ? '_' : '/')) return 63;
        return -1;
    };
    size_t n = in.size();
    size_t padding = 0;
    while (n > 0 && in[n-1] == '=') { --n; ++padding; }
    if (padding > 2) fail("base64_decode", "too much '=' padding");
    // padding is optional only when absent entirely; if present it must complete the quantum
    if (padding && (n + padding) % 4 != 0) fail("base64_decode", "incorrect padding");
    if (n % 4 == 1) fail("base64_decode", "invalid length");
    std::string out;
    out.reserve(n / 4 * 3 + 2);
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        int v = val(in[i]);
        if (v < 0) fail("base64_decode", "invalid character at position " + std::to_string(i));
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
    }
    // leftover bits must be zero (canonical encoding)
    if (bits && (acc & ((1u << bits) - 1)))
        fail("base64_decode", "non-canonical trailing bits");
    return out;
}

// ---- sha256 (FIPS 180-4) ----
static std::array<uint8_t, 32> sha256Raw(const std::string& msg) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::string m = msg;
    uint64_t bitLen = (uint64_t)msg.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; --i) m += (char)((bitLen >> (i * 8)) & 0xFF);
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)(uint8_t)m[off+i*4] << 24) | ((uint32_t)(uint8_t)m[off+i*4+1] << 16) |
                   ((uint32_t)(uint8_t)m[off+i*4+2] << 8) | (uint32_t)(uint8_t)m[off+i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[i*4+j] = (uint8_t)(h[i] >> (24 - 8*j));
    return out;
}

static std::string hexOf(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

EMBR_PLUGIN {
    // base64_encode(data: str, urlsafe?: any, nopad?: any) -> str
    // urlsafe (truthy) uses the '-'/'_' alphabet; nopad (truthy) omits '=' padding.
    interp->bindSig("base64_encode", {pStr("data"), pOpt("urlsafe"), pOpt("nopad")},
    [](const std::vector<Value>& args) -> Value {
        bool url = args.size() >= 2 && args[1].truthy();
        bool nopad = args.size() >= 3 && args[2].truthy();
        return Value(b64Encode(args[0].asString(), url, !nopad));
    });

    // base64_decode(text: str, urlsafe?: any) -> str
    // strict: rejects characters outside the alphabet (including whitespace),
    // bad padding and non-canonical trailing bits. padding may be omitted.
    interp->bindSig("base64_decode", {pStr("text"), pOpt("urlsafe")},
    [](const std::vector<Value>& args) -> Value {
        return Value(b64Decode(args[0].asString(), args.size() >= 2 && args[1].truthy()));
    });

    // hex_encode(data: str) -> str   (lowercase)
    interp->bindSig("hex_encode", {pStr("data")},
    [](const std::vector<Value>& args) -> Value {
        const auto& s = args[0].asString();
        return Value(hexOf((const uint8_t*)s.data(), s.size()));
    });

    // hex_decode(text: str) -> str   (either case; even length required)
    interp->bindSig("hex_decode", {pStr("text")},
    [](const std::vector<Value>& args) -> Value {
        const auto& s = args[0].asString();
        if (s.size() % 2) fail("hex_decode", "odd number of hex digits");
        auto nib = [&](char c, size_t pos) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            fail("hex_decode", "invalid hex digit at position " + std::to_string(pos));
        };
        std::string out; out.reserve(s.size() / 2);
        for (size_t i = 0; i < s.size(); i += 2)
            out += (char)((nib(s[i], i) << 4) | nib(s[i+1], i+1));
        return Value(std::move(out));
    });

    // sha256(data: str) -> str   (64 lowercase hex chars)
    interp->bindSig("sha256", {pStr("data")},
    [](const std::vector<Value>& args) -> Value {
        auto d = sha256Raw(args[0].asString());
        return Value(hexOf(d.data(), d.size()));
    });

    // sha256_raw(data: str) -> str   (32 raw bytes)
    interp->bindSig("sha256_raw", {pStr("data")},
    [](const std::vector<Value>& args) -> Value {
        auto d = sha256Raw(args[0].asString());
        return Value(std::string((const char*)d.data(), d.size()));
    });

    // crc32(data: str) -> int   (IEEE 802.3, as used by zip/gzip/png; 0..2^32-1)
    interp->bindSig("crc32", {pStr("data")},
    [](const std::vector<Value>& args) -> Value {
        static const auto table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t c = 0xFFFFFFFFu;
        for (unsigned char ch : args[0].asString()) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
        return Value((int64_t)(c ^ 0xFFFFFFFFu));
    });
}
