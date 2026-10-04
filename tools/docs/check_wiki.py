#!/usr/bin/env python3
"""checks that the docs (the wiki) still tell the truth. CTest runs this (docs_examples).

  python3 tools/docs/check_wiki.py --embr bin/linux/embr --include build/linux-vm-debug/generated

the docs live in the GitHub wiki (la-creatura/embr-lang.wiki), not in this repo, so this needs a clone of it:
pass --wiki DIR, set EMBR_WIKI_DIR, or just init the submodule (git submodule update --init), which is
found at wiki/. CMake passes it along (-DEMBR_WIKI_DIR=DIR to use another clone). without a wiki it prints a
note and exits 77, which CTest reports as skipped.

what it checks
  - ```embr-run blocks in the wiki pages are run on every backend the embr binary has (embr-run-vm: vm only).
    if the next block is ```output, stdout must match it exactly. a block with no output block just has to
    run without an error.
  - ```cpp-compile blocks are compiled (-fsyntax-only) against the generated embr.h.
  - ```cpp-check blocks are fragments of examples/hello/hello.cpp: every line must still appear there.
  - a few numbers the wiki states (keyword count, call depth, nesting depth) are compared to the source.
  - every plugin in plugins/ and every module in modules/ has a wiki page, listed in plugin-reference
    (or script-modules) and in import-name-index.
"""
import argparse, pathlib, re, subprocess, sys, tempfile, os

root = pathlib.Path(__file__).resolve().parents[2]
failures = []

def fail(where, msg):
    failures.append(f"{where}: {msg}")

def blocks(path):
    """yields (lang, text, first_line) for every fenced block in a file."""
    return blocks_of(path.read_text())

def blocks_of(source):
    """yields (lang, text, first_line) for every fenced block in a string."""
    lines = source.split("\n")
    i = 0
    while i < len(lines):
        if lines[i].startswith("```"):
            lang, start, body = lines[i][3:].strip(), i + 1, []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i]); i += 1
            yield lang, "\n".join(body) + "\n", start
        i += 1

def norm(s):
    return re.sub(r"\s+", " ", s).strip()

def run_embr(embr, flags, script, workdir):
    f = pathlib.Path(workdir) / "block.embr"
    f.write_text(script)
    env = dict(os.environ, EMBR_PATH=str(pathlib.Path(embr).parent))
    return subprocess.run([embr, *flags, str(f)], capture_output=True, text=True, cwd=workdir, env=env, timeout=60)

def check_blocks(doc, embr, include, backends):
    hello = (root / "examples/hello/hello.cpp").read_text()
    hello_norm = {norm(l) for l in hello.split("\n")}
    items = list(blocks(doc))
    for n, (lang, text, line) in enumerate(items):
        where = f"{doc.name}:{line}"
        if lang in ("embr-run", "embr-run-vm"):
            expect = items[n + 1][1] if n + 1 < len(items) and items[n + 1][0] == "output" else None
            for name, flags in backends:
                if lang == "embr-run-vm" and name != "vm":
                    continue
                with tempfile.TemporaryDirectory() as d:
                    r = run_embr(embr, flags, text, d)
                if r.returncode != 0:
                    fail(where, f"[{name}] exited {r.returncode}: {(r.stderr or r.stdout).strip()[:300]}")
                elif expect is not None and r.stdout != expect:
                    fail(where, f"[{name}] output differs\n--- expected\n{expect}--- got\n{r.stdout}")
        elif lang == "cpp-compile":
            with tempfile.TemporaryDirectory() as d:
                f = pathlib.Path(d) / "block.cpp"
                f.write_text(text)
                r = subprocess.run(["g++", "-std=c++20", "-fsyntax-only", "-I", include, str(f)],
                                   capture_output=True, text=True)
            if r.returncode != 0:
                fail(where, "does not compile:\n" + r.stderr[:600])
        elif lang == "cpp-check":
            for l in text.split("\n"):
                if norm(l) and norm(l) not in hello_norm:
                    fail(where, f"line not found in examples/hello/hello.cpp: {norm(l)}")

def check_numbers(wiki):
    def page(name):
        f = wiki / f"{name}.md"
        if not f.exists():
            fail(name, "wiki page not found")
            return ""
        return f.read_text()
    lex, overview = page("lexical-grammar"), page("overview")
    scoping, types = page("scoping-and-closures"), page("type-system")
    lexer = (root / "include/embr/core/lexer.h").read_text()
    kw = re.findall(r'\{"(\w+)",TokenType::', lexer[lexer.index("Token lexIdent"):lexer.index("Token lexNum")])
    m = re.search(r"### reserved keywords \((\d+)\)", lex)
    if not m or int(m.group(1)) != len(kw):
        fail("lexical-grammar", f"says {m.group(1) if m else '?'} reserved keywords, lexer has {len(kw)}")
    if re.search(r"18 reserved keywords", overview) and len(kw) != 18:
        fail("overview", f"says 18 keywords, lexer has {len(kw)}")
    block = next((t for lang, t, _ in blocks_of(lex) if t.startswith("if  elif")), "")
    if set(block.split()) != set(kw):
        fail("lexical-grammar", f"keyword list differs from the lexer: {sorted(set(block.split()) ^ set(kw))}")
    registry = (root / "include/embr/core/registry.h").read_text()
    depth = re.search(r"maxCallDepth_\s*=\s*(\d+)", registry).group(1)
    if f"max call depth ({depth})" not in scoping:
        fail("scoping-and-closures", f"call depth message does not mention the real default {depth}")
    value = (root / "include/embr/core/value.h").read_text()
    nest = re.search(r"kMaxValueDepth\s*=\s*(\d+)", value).group(1)
    if f"max depth {nest})" not in types:
        fail("type-system", f"nesting limit does not mention the real cap {nest}")

def check_pages(wiki):
    """every plugin and script module has a page, a list entry and an index row."""
    def text(name):
        f = wiki / f"{name}.md"
        return f.read_text() if f.exists() else ""
    reference, modules, index = text("plugin-reference"), text("script-modules"), text("import-name-index")
    for d in sorted((root / "plugins").iterdir()):
        if not (d / f"{d.name}.cpp").exists():
            continue
        n = d.name
        if n == "hello":
            continue
        if not (wiki / f"plugin-{n}.md").exists():
            fail(f"plugin-{n}", "plugins/%s has no wiki page" % n)
        if f"](plugin-{n})" not in reference:
            fail("plugin-reference", f"does not list `{n}`")
        if f"| `{n}` |" not in index:
            fail("import-name-index", f"has no row for `{n}`")
    for f in sorted((root / "modules").glob("*.embr")):
        n = f.stem
        if not (wiki / f"module-{n}-embr.md").exists():
            fail(f"module-{n}-embr", f"modules/{f.name} has no wiki page")
        if f"](module-{n}-embr)" not in modules:
            fail("script-modules", f"does not list `{n}.embr`")
        if f"| `{n}.embr` |" not in index:
            fail("import-name-index", f"has no row for `{n}.embr`")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--embr", required=True)
    ap.add_argument("--include", required=True, help="directory that holds embr/embr.h")
    default_wiki = os.environ.get("EMBR_WIKI_DIR", "") or (str(root / "wiki") if (root / "wiki" / "home.md").exists() else "")
    ap.add_argument("--wiki", default=default_wiki, help="a clone of the wiki repo (default: the wiki/ submodule)")
    a = ap.parse_args()
    if not a.wiki:
        print("no wiki found (--wiki DIR, EMBR_WIKI_DIR, or git submodule update --init), skipping the docs checks")
        sys.exit(77)
    wiki = pathlib.Path(a.wiki).resolve()
    if not (wiki / "home.md").exists():
        print(f"{wiki} does not look like a wiki clone (no home.md)")
        sys.exit(1)
    if not (wiki / "plugin-reference.md").exists():
        print(f"{wiki} is an old copy of the wiki (no plugin-reference.md), pull it: git -C {wiki} pull")
        sys.exit(77)
    a.embr = str(pathlib.Path(a.embr).resolve())
    a.include = str(pathlib.Path(a.include).resolve())
    version = subprocess.run([a.embr, "--version"], capture_output=True, text=True).stdout
    backends = []
    if "tree-walker" in version: backends.append(("tree", []))
    if "vm" in version.split("backends:")[-1]: backends.append(("vm", ["--vm"]))
    for doc in sorted(wiki.glob("*.md")):
        check_blocks(doc, a.embr, a.include, backends)
    check_numbers(wiki)
    check_pages(wiki)
    if failures:
        print("\n\n".join(failures)); sys.exit(1)
    print("docs examples ok")

main()
