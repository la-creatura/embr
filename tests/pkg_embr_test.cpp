// unit tests for modules/pkg.embr
//
// like ffi_embr_test.cpp, this covers what works without a live registry (CTest has no guaranteed network): version
// resolution and comparison against a hand-built index map, manifest save/load through fs, and the errors for an
// unconfigured registry, an unknown package, and removing something never installed. the real download/install/
// cache path (http_download + http_sha256 + index fetch) is checked by hand against a local server
//
// tests that touch disk (the manifest round trip) use their own fixture name under the shared CTest working
// directory and clean up, like fs_test.cpp
// named fixture under the shared CTest working directory and cleans up
// after itself, same convention as fs_test.cpp.

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "pkg_versions: returns versions in ascending semver order",
          "idx = {\"foo\": {\"versions\": {\"2.0.0\": {}, \"1.0.0\": {}, \"1.5.2\": {}}}}\n"
          "print(pkg_versions(idx, \"foo\"))\n",
          "[\"1.0.0\", \"1.5.2\", \"2.0.0\"]\n" },

        { "pkg_versions: raises on an unknown package",
          "idx = {}\n"
          "try\npkg_versions(idx, \"nope\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_resolve: \"latest\" prefers the index's own latest field when present",
          "idx = {\"foo\": {\"latest\": \"1.0.0\", \"versions\": {\"1.0.0\": {}, \"2.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"latest\"))\n",
          "1.0.0\n" },

        { "pkg_resolve: \"latest\" falls back to the highest semver version without a latest field",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}, \"1.9.0\": {}, \"1.10.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"latest\"))\n",
          "1.10.0\n" },

        { "pkg_resolve: an empty spec behaves like \"latest\"",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"\"))\n",
          "1.0.0\n" },

        { "pkg_resolve: an exact version spec is returned as-is when present",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}, \"2.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"1.0.0\"))\n",
          "1.0.0\n" },

        { "pkg_resolve: raises on a version spec the registry doesn't have",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}}}}\n"
          "try\npkg_resolve(idx, \"foo\", \"9.9.9\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_resolve: \">=X.Y.Z\" picks the lowest satisfying version, not the highest",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}, \"1.5.0\": {}, \"2.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \">=1.2.0\"))\n",
          "1.5.0\n" },

        { "pkg_resolve: \">=X.Y.Z\" raises when nothing satisfies it",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}}}}\n"
          "try\npkg_resolve(idx, \"foo\", \">=5.0.0\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_fetch_index: raises without a configured registry",
          "try\npkg_fetch_index()\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_remove: raises on a package that was never installed",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest_empty.json\"})\n"
          "try\npkg_remove(\"nope\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_list / manifest round-trip: reads back what pkg_save_manifest wrote",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest1.json\"})\n"
          "pkg_save_manifest({\"foo\": {\"version\": \"1.0.0\", \"type\": \"embr\", \"path\": \"modules/foo.embr\"}})\n"
          "m = pkg_list()\n"
          "print(m[\"foo\"][\"version\"])\n"
          "print(m[\"foo\"][\"type\"])\n"
          "fs_remove(\"test_pkg_manifest1.json\")\n",
          "1.0.0\nembr\n" },

        { "__pkg_install_dir: routes plugin/embr/anything-else to plugins_dir/modules_dir/vendor_dir",
          "print(__pkg_install_dir(\"plugin\"))\n"
          "print(__pkg_install_dir(\"embr\"))\n"
          "print(__pkg_install_dir(\"asset\"))\n",
          "plugins\nmodules\nvendor\n" },

        { "__pkg_extname: keeps double-barrelled archive suffixes intact instead of truncating to the last dot",
          "print(__pkg_extname(\"https://github.com/o/r/archive/refs/tags/v1.0.0.tar.gz\"))\n"
          "print(__pkg_extname(\"https://example.com/x.tar.bz2\"))\n"
          "print(__pkg_extname(\"https://example.com/x.tar.xz\"))\n"
          "print(__pkg_extname(\"https://example.com/x.TAR.GZ\"))\n"
          "print(__pkg_extname(\"https://example.com/foo-1.0.0.so\"))\n"
          "print(__pkg_extname(\"https://example.com/foo-1.0.0.embr\"))\n"
          "print(__pkg_extname(\"https://example.com/noext\"))\n",
          ".tar.gz\n.tar.bz2\n.tar.xz\n.TAR.GZ\n.so\n.embr\n\n" },

        { "pkg_list: an unwritten manifest reads back as empty",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest_missing.json\"})\n"
          "m = pkg_list()\n"
          "print(len(keys(m)))\n",
          "0\n" },

        { "pkg_main: \"list\" with nothing installed",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest_main_empty.json\"})\n"
          "pkg_main([\"list\"])\n",
          "no packages installed\n" },

        { "pkg_main: unknown command prints usage and returns 1",
          "r = pkg_main([\"bogus\"])\n"
          "print(r)\n",
          "usage: embr pkg [--registry <url>] <command> [args]\n"
          "commands:\n"
          "  install <name> [version]   install a package (default: latest)\n"
          "  list                       list installed packages\n"
          "  remove <name>              uninstall a package\n"
          "  versions <name>            list a package's available versions\n"
          "1\n" },

        { "pkg_main: install without a registry configured reports a caught error, not a crash",
          "pkg_configure({\"registry_url\": \"\"})\n"
          "r = pkg_main([\"install\", \"foo\"])\n"
          "print(r)\n",
          "pkg error: pkg: no registry configured -- call pkg_configure({\"registry_url\": \"http://host:port\"}) first\n"
          "1\n" },
    };
    return embr_test::runSuite("pkg.embr module test suite", tests, {"pkg.embr"});
}
