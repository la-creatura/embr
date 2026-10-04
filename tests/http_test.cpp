// unit tests for plugins/http/http.cpp
//
// CTest has no guaranteed network, so this only covers what works offline: url/scheme checks, http_url_encode,
// http_sha256, and the errors for a port nothing listens on and a domain that can't resolve ("invalid." is
// reserved for this by RFC 2606). those raise with or without TLS in the build
// real requests (plain http, https against a live endpoint, headers/status, http_download following a redirect,
// checksum mismatch rejection) are checked by hand with smoke scripts

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "http_get: raises on a missing scheme",
          "try\nhttp_get(\"localhost:8931/x\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "http_get: raises on an unsupported scheme",
          "try\nhttp_get(\"ftp://example.com/\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "http_get: raises on a domain that can never resolve (RFC 2606), https or not",
          "try\nhttp_get(\"https://host.invalid/\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "http_get: raises when nothing is listening on the target port",
          "try\nhttp_request(\"GET\", \"http://127.0.0.1:1/x\", {}, \"\", {\"timeout\": 1})\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "http_tls_available: reports 0 or 1, never raises",
          "t = http_tls_available()\nprint(t == 0 or t == 1)\n",
          "1\n" },

        { "http_url_encode: percent-encodes everything outside the unreserved set",
          "print(http_url_encode(\"a b/c?d=e&f\"))\n"
          "print(http_url_encode(\"already-safe_.~123\"))\n",
          "a%20b%2Fc%3Fd%3De%26f\nalready-safe_.~123\n" },

        { "http_sha256: matches known test vectors",
          "print(http_sha256(\"\"))\n"
          "print(http_sha256(\"hello\"))\n",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
          "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\n" },
    };
    return embr_test::runSuite("http plugin test suite", tests, {"http"});
}
