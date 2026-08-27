#!/usr/bin/env python3
"""Reproduce the user's exact invocation (with -e LANG, no HOME)."""
import os, pty, select, subprocess, sys, time

master, slave = pty.openpty()
cmd = ["docker", "run", "--rm", "-i", "-t",
       "-e", "LANG=C.UTF-8",
       "-e", "BB_AGENT_BASE_URL=http://host.docker.internal:8317/v1",
       "-e", "BB_AGENT_API_KEY=sk-lloyd-1",
       "-e", "BB_AGENT_MODEL=gpt-5.6-luna",
       "busyagent-scratch", "busyagent"]
shf = tempfile = None
import tempfile as _tf
f = _tf.NamedTemporaryFile("w", suffix=".sh", delete=False)
f.write("#!/bin/sh\nexec " + " ".join(cmd) + "\n")
f.close()
os.chmod(f.name, 0o755)
p = subprocess.Popen(["/usr/bin/script", "-q", "/dev/null", f.name],
                     stdin=slave, stdout=slave, stderr=slave)
os.close(slave)

buf = b""
def drain(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.25)
        if master in r:
            try:
                d = os.read(master, 65536)
            except OSError:
                return
            if not d:
                return
            buf += d

drain(6.0)
os.write(master, "你好世界\n".encode())
drain(10.0)
os.write(master, "quit\r".encode())
end = time.time() + 6
while p.poll() is None and time.time() < end:
    r, _, _ = select.select([master], [], [], 0.3)
    if master in r:
        try:
            buf += os.read(master, 65536)
        except OSError:
            break
try:
    p.kill()
except Exception:
    pass

idx = buf.find(b"> ")
seg = buf[idx:idx + 140] if idx >= 0 else buf[:140]
sys.stderr.write("region: " + repr(seg) + "\n")
print("chinese echoed as utf8:", "你好世界".encode() in seg,
      "| question marks:", b"??" in seg or b"????" in seg)
