#!/usr/bin/env python3
"""Reproduce Chinese echo in busyagent REPL through a real pty.
Dump raw bytes of the echoed input line to classify:
  raw utf8 bytes echoed back => lineedit fine (width issue at most)
  '?' or dropped bytes       => decode/display replacement active"""
import os, pty, re, select, subprocess, sys, time

BB = "/src/busybox"

master, slave = pty.openpty()
p = subprocess.Popen(
    [BB, "busyagent", "-u", "http://127.0.0.1:1/v1", "-k", "k", "-m", "m"],
    stdin=slave, stdout=slave, stderr=slave,
    env=dict(os.environ, BB_AGENT_HOME="/tmp/bbzh", BB_AGENT_E2E_URL="x",
             LC_ALL="C.UTF-8"),
    close_fds=True)
os.close(slave)

buf = b""
def drain(t=3.0):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.2)
        if master in r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d: break
            buf += d
drain(2.0)

# type chinese + latin mix
os.write(master, "你好世界hello\n".encode())
drain(2.5)

try:
    p.kill()
except Exception:
    pass

sys.stderr.write("=== RAW ECHO BYTES ===\n")
idx = buf.find(b"\x1b[32m> ")
seg = buf[idx:] if idx >= 0 else buf
for label, chunk in (("input+echo region", seg[:200]),):
    sys.stderr.write(label + ": ")
    sys.stderr.write(repr(chunk) + "\n")

has_cn = "你好世界".encode() in seg
has_q = b"??" in seg
print("echo contains original utf8:", has_cn)
print("double-questionmark seen:", has_q)
