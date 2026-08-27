#!/usr/bin/env python3
"""TLS stub LLM for https e2e: single scripted turn over SSL.
Run on the host; bb-build's busyagent connects to https://localhost:8443.

Turn 1: returns a text answer directly (verifies handshake + request
parse + streamed response over busybox TLS)."""
import http.server, json, ssl, sys

CERT, KEY = "/tmp/tlscert.pem", "/tmp/tlskey.pem"

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n) or b"{}")
        answer = "tls-handshake-ok"
        obj = {"id": "tlsstub", "object": "chat.completion.chunk", "model": "stub",
               "choices": [{"index": 0, "delta": {"content": answer},
                            "finish_reason": "stop"}]}
        out = "data: " + json.dumps(obj) + "\n\ndata: [DONE]\n\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out.encode())

    def do_GET(self):
        sys.stderr.write("srv: GET %s\n" % self.path)
        self.send_response(200); self.send_header("Content-Length","2"); self.end_headers(); self.wfile.write(b"ok")

    def handle_one_request(self):
        try:
            sys.stderr.write("srv: request begin\n"); sys.stderr.flush()
            http.server.BaseHTTPRequestHandler.handle_one_request(self)
        except Exception as e:
            sys.stderr.write("srv: handler exc: %r\n" % (e,))

    def log_message(self, *a):
        sys.stderr.write("srv: " + " ".join(str(x) for x in a) + "\n")

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
class T(http.server.ThreadingHTTPServer):
    daemon_threads = True
    def handle_error(self, request, client_address):
        import traceback
        traceback.print_exc()
srv = T(("0.0.0.0", 8443), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("tls stub on :8443", flush=True)
srv.serve_forever()
