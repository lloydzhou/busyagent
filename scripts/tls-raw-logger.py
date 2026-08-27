#!/usr/bin/env python3
"""TLS handshake probe on a fresh high port: log EVERYTHING at socket level."""
import socket, ssl, sys, threading, time

PORT = 28446
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("/tmp/c.pem", "/tmp/k.pem")

resp = (b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        b"Content-Length: 82\r\nConnection: close\r\n\r\n"
        b'data: {"choices":[{"delta":{"content":"tls-raw-ok"},"finish_reason":"stop"}]}\n\n'
        b"data: [DONE]\n\n")

def serve(conn, addr):
    print(f"[srv] TCP established {addr}", flush=True)
    try:
        tls = ctx.wrap_socket(conn, server_side=True)
    except Exception as e:
        print(f"[srv] HANDSHAKE FAIL: {e!r}", flush=True)
        conn.close(); return
    print("[srv] TLS handshake OK", flush=True)
    tls.settimeout(8)
    total = b""
    try:
        while True:
            d = tls.recv(65536)
            if not d:
                print("[srv] peer closed", flush=True); break
            total += d
            print(f"[srv] recv {len(d)}B", flush=True)
            if b"[DONE]" in total:
                break
    except Exception as e:
        print(f"[srv] recv err {e!r}", flush=True)
    print(f"[srv] total={len(total)}B, replying", flush=True)
    try:
        tls.sendall(resp); tls.close()
    except Exception as e:
        print("[srv] reply err", e, flush=True)

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(8)
print(f"[srv] listening :{PORT}", flush=True)
while True:
    conn, addr = srv.accept()
    threading.Thread(target=serve, args=(conn, addr), daemon=True).start()
