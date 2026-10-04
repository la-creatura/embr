#!/usr/bin/env python3
"""drives `embr lsp` over pipes: initialize, open a broken file, fix it, close it, shut down.

  lsp_smoke.py <embr binary>
"""
import json, subprocess, sys

proc = subprocess.Popen([sys.argv[1], "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL)

def send(obj):
    body = json.dumps(obj).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()

def recv():
    length = None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise SystemExit("FAIL: server closed its output")
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(proc.stdout.read(length))

def check(cond, what):
    if not cond:
        proc.kill()
        raise SystemExit("FAIL: " + what)

uri = "file:///tmp/t.embr"

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
r = recv()
check(r["id"] == 1 and r["result"]["capabilities"]["textDocumentSync"]["change"] == 1, "initialize result")
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

# a lexer error after a two-byte character: the byte column 10 must become UTF-16 column 8
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": uri, "languageId": "embr", "version": 1, "text": 's = "é" @\nprint(1)\n'}}})
r = recv()
check(r["method"] == "textDocument/publishDiagnostics" and r["params"]["uri"] == uri, "publishDiagnostics after open")
d = r["params"]["diagnostics"]
check(len(d) == 1, "one diagnostic, got %r" % d)
check(d[0]["range"]["start"] == {"line": 0, "character": 8}, "range start %r" % d[0]["range"])
check("unexpected character" in d[0]["message"], "message %r" % d[0]["message"])

# a parse error on a later line, with the earlier lines fine
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 2},
                 "contentChanges": [{"text": "x = 1\nif x\nprint(x)\n"}]}})
r = recv()
d = r["params"]["diagnostics"]
check(len(d) >= 1 and d[0]["range"]["start"]["line"] >= 1, "parse error on a later line: %r" % d)

# fixed: no diagnostics
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 3},
                 "contentChanges": [{"text": "x = 1\nprint(x)\n"}]}})
check(recv()["params"]["diagnostics"] == [], "clean file has no diagnostics")

# unknown requests get an error, unknown notifications are ignored
send({"jsonrpc": "2.0", "id": 7, "method": "textDocument/hover", "params": {}})
r = recv()
check(r["id"] == 7 and r["error"]["code"] == -32601, "unknown request answers method-not-found")
send({"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 7}})

send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": uri}}})
check(recv()["params"]["diagnostics"] == [], "close clears diagnostics")

send({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
check(recv()["id"] == 2, "shutdown reply")
send({"jsonrpc": "2.0", "method": "exit"})
check(proc.wait(timeout=10) == 0, "exit code 0 after shutdown")
print("lsp smoke OK")
