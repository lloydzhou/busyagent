#!/bin/sh
# resume replay test: turn1 (echo token), then fresh interactive process
# must replay history before prompting.
export BB_AGENT_HOME=/tmp/bbreplay
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbreplay

# turn 1: single-turn pipe call (creates session)
printf 'reply with exactly: mango-first\n' | $BB busyagent -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna > /dev/null 2>&1
echo "turn1 rc=$?"

# turn 2: interactive mode, cwd already bound to same session -> should replay
python3 - <<'EOF'
import os, pty, select, subprocess, sys, time
master, slave = pty.openpty()
p = subprocess.Popen(["/src/busybox", "busyagent", "-u", "http://host.docker.internal:8317/v1",
                      "-k", "sk-lloyd-1", "-m", "gpt-5.6-luna"],
                     stdin=slave, stdout=slave, stderr=slave,
                     env=dict(os.environ), close_fds=True)
os.close(slave)
buf = b""
end = time.time() + 45
while time.time() < end:
    r,_,_ = select.select([master],[],[],0.3)
    if master in r:
        try: d = os.read(master, 65536)
        except OSError: break
        if not d: break
        buf += d
        if buf.count(b"busyagent> ") >= 2:
            break
os.write(master, b"what did i ask before? quote it.\n")
end = time.time() + 50
while time.time() < end:
    r,_,_ = select.select([master],[],[],0.3)
    if master in r:
        try: d = os.read(master, 65536)
        except OSError: break
        if not d: break
        buf += d
        if b"mango" in buf and buf.count(b"busyagent> ") >= 3:
            break
try: p.kill()
except Exception: pass
sys.stderr.write("\n----- TRANSCRIPT -----\n")
sys.stderr.write(buf.decode("utf8","replace")[:2500])
has_replay = b"> reply with exactly: mango-first" in buf
ok = p.poll()
print("\nreplayed-history:", has_replay, "| prompt2 seen:", buf.count(b"busyagent> ")>=2)
EOF
