#!/usr/bin/env python3
"""HTTPS stub LLM on 28447: full TLS + immediate SSE reply (no recv-timeout
delay). Scenario: turn 1 answers directly, proving handshake + request +
streamed response over the busybox TLS stack."""
import http.server, json, ssl, socket, sys, threading

PORT = 28447
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("/tmp/c.pem", "/tmp/k.pem")

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        self.rfile.read(n)
        out = ('data: {"id":"tls","object":"chat.completion.chunk","model":"stub",'
               '"choices":[{"index":0,"delta":{"content":"tls-handshake-ok"},'
               '"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out.encode())
        sys.stderr.write("srv: POST handled\n"); sys.stderr.flush()
    def log_message(self, *a):
        pass

class Srv(http.server.ThreadingHTTPServer):
    daemon_threads = True

srv = Srv(("0.0.0.0", PORT), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(f"https stub on :{PORT}", flush=True)
srv.serve_forever()
