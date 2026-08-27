#!/usr/bin/env python3
"""Verify sub_agent_result history is replayed in interactive mode."""
import os, pty, re, select, subprocess, sys, time

master, slave = pty.openpty()
p = subprocess.Popen(
    ["/src/busybox", "busyagent", "-u", "http://host.docker.internal:8317/v1",
     "-k", "sk-lloyd-1", "-m", "gpt-5.6-luna"],
    stdin=slave, stdout=slave, stderr=slave,
    env=dict(os.environ), close_fds=True)   # BB_AGENT_HOME=/tmp/bbreplay3
os.close(slave)

buf = b""
end = time.time() + 30
while time.time() < end and buf.count(b"busyagent> ") < 1:
    r, _, _ = select.select([master], [], [], 0.3)
    if master in r:
        try:
            d = os.read(master, 65536)
        except OSError:
            break
        if not d:
            break
        buf += d

time.sleep(0.5)
r, _, _ = select.select([master], [], [], 1.0)
if master in r:
    buf += os.read(master, 65536)
try:
    p.kill()
except Exception:
    pass

sys.stderr.write("----- TRANSCRIPT -----\n")
sys.stderr.write(buf.decode("utf8", "replace")[:1500])
print("\nsub-agent result replayed:", b"[sub-agent" in buf)
sys.exit(0 if b"[sub-agent" in buf else 1)
