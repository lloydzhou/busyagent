#!/usr/bin/env python3
"""raw byte logger on 18444: dump exactly what busyagent sends, then
reply with a valid SSE response so the client proceeds."""
import socket, sys, time

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", 18445))
srv.listen(5)
print("raw logger on :18445", flush=True)

resp = (b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        b"Content-Length: 78\r\nConnection: close\r\n\r\n"
        b'data: {"choices":[{"delta":{"content":"raw-ok"},"finish_reason":"stop"}]}\n\n'
        b"data: [DONE]\n\n")

while True:
    conn, addr = srv.accept()
    print(f"=== accept from {addr} ===", flush=True)
    conn.settimeout(6)
    total = b""
    try:
        while True:
            d = conn.recv(65536)
            if not d:
                print("=== peer closed ===", flush=True)
                break
            total += d
            print(f"recv {len(d)}B: {d[:120]!r}", flush=True)
            if len(total) > 20000:
                break
    except socket.timeout:
        print("=== recv timeout ===", flush=True)
    print(f"total={len(total)}B; replying", flush=True)
    conn.sendall(resp)
    conn.close()
