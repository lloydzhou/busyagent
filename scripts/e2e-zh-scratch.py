#!/usr/bin/env python3
"""CJK echo test against the busyagent-scratch image.
docker run allocates a tty via our pty slave; type Chinese; the echoed
bytes on the master tell us whether lineedit's unicode path survives
the static build."""
import os, pty, select, subprocess, sys, time

master, slave = pty.openpty()
cmd = ["docker", "run", "--rm", "-i", "-t",
       "-e", "BB_AGENT_HOME=/tmp/s", "-e", "HOME=/tmp/s",
       "-e", "BB_AGENT_BASE_URL=http://host.docker.internal:8317/v1",
       "-e", "BB_AGENT_API_KEY=sk-lloyd-1",
       "-e", "BB_AGENT_MODEL=gpt-5.6-luna",
       "busyagent-scratch", "busyagent"]   # REPL (no prompt arg)
if sys.platform == "darwin":
    # docker cli refuses -t when its own stdin isn't a tty;
    # wrap via script(1) running a temp script (avoids arg re-splitting)
    import tempfile
    shf = tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False)
    shf.write("#!/bin/sh\nexec " + " ".join(cmd) + "\n")
    shf.close()
    os.chmod(shf.name, 0o755)
    cmd = ["/usr/bin/script", "-q", "/dev/null", shf.name]
p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave)
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

drain(6.0)                       # wait for banner + first prompt
sys.stderr.write("early: " + repr(buf[:300]) + " rc=" + str(p.poll()) + "\n")
os.write(master, "你好世界\n".encode())
drain(8.0)                       # model turn
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
seg = buf[idx:idx + 120] if idx >= 0 else buf[:120]
sys.stderr.write("region: " + repr(seg) + "\n")
print("chinese echoed as utf8:", "你好世界".encode() in seg)
