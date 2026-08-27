#!/usr/bin/env python3
"""plain-HTTP stub on 18444 for loopback comparison"""
import http.server, sys

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        self.rfile.read(n)
        out = 'data: {"id":"p","choices":[{"index":0,"delta":{"content":"plain-ok"},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n'
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out.encode())
    def log_message(self, fmt, *a):
        sys.stderr.write("stub: " + fmt % a + "\n")

http.server.HTTPServer(("0.0.0.0", 18444), H).serve_forever()
