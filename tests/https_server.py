#!/usr/bin/env python3
"""throwaway HTTPS server for tests/https_smoke.sh. prints its port on the first line of stdout.

  GET  /hello      -> 200 "hi"
  POST /echo       -> 200, body = "<method>|<X-Test header>|<request body>"
  GET  /redir/N    -> 302 to /redir/N-1 ... /redir/0 -> 200 "end"
"""
import http.server, ssl, sys

class H(http.server.BaseHTTPRequestHandler):
    def _send(self, code, body=b"", extra=None):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/hello":
            return self._send(200, b"hi")
        if self.path.startswith("/redir/"):
            n = int(self.path.split("/")[-1])
            if n <= 0:
                return self._send(200, b"end")
            return self._send(302, b"", {"Location": "/redir/%d" % (n - 1)})
        self._send(404, b"nope")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        out = "%s|%s|%s" % (self.command, self.headers.get("X-Test", ""), body)
        self._send(200, out.encode())

    def log_message(self, *a):
        pass

cert, key = sys.argv[1], sys.argv[2]
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(srv.server_address[1], flush=True)
srv.serve_forever()
