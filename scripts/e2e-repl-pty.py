#!/usr/bin/env python3
"""Drive busyagent's interactive REPL through a real pty and verify:
   1. prompt appears (busybox lineedit active)
   2. two turns share session history (memory across turns in one REPL)
   3. 'quit' exits cleanly"""
import os, pty, re, select, subprocess, sys, time

BB = "/src/busybox"
URL = "http://host.docker.internal:8317/v1"

master, slave = pty.openpty()
p = subprocess.Popen(
    [BB, "busyagent",
     "-u", URL, "-k", "sk-lloyd-1", "-m", "gpt-5.6-luna"],
    stdin=slave, stdout=slave, stderr=slave,
    env=dict(os.environ, BB_AGENT_HOME="/tmp/bbpty"),
    close_fds=True)
os.close(slave)

buf = b""
deadline = time.time() + 60

def read_until(patterns, timeout=40):
    global buf, deadline
    end = time.time() + timeout
    rx = [re.compile(p.encode()) for p in patterns]
    hit = set()
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.3)
        if master in r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
            for x in rx:
                if x.search(buf) and id(x) not in hit:
                    hit.add(id(x))
        if len(hit) == len(rx):
            return True
        if p.poll() is not None:
            break
    return len(hit) == len(rx)

def send(line):
    os.write(master, (line + "\n").encode())

try:
    ok_prompt = read_until([r"\x1b\[32m> \x1b\[0m"])
    print("prompt shown:", ok_prompt)

    send("remember the token pty-mango-77. just acknowledge briefly.")
    ok_ack = read_until([r"pty-mango-77|acknowledg"], 45)
    # wait for next prompt: proves turn loop returned to input
    ok_prompt2 = read_until([r"\x1b\[32m> \x1b\[0m"], 20)
    print("turn1 ack seen:", ok_ack, "| second prompt:", ok_prompt2)

    send("what token did I tell you? answer with the token only.")
    ok_recall = read_until([r"pty-mango-77"], 45)
    print("memory across turns:", ok_recall)
    ok_prompt3 = read_until([r"\x1b\[32m> \x1b\[0m"], 20)

    send("quit")
    # drain until child exits so stderr farewell lines (Goodbye/Resume) land in buf
    while p.poll() is None and time.time() < time.time() + 20:
        r, _, _ = select.select([master], [], [], 0.4)
        if master in r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
    alive = p.poll() is None
    print("exited on quit:", not alive)
    sys.exit(0 if (ok_prompt and ok_ack and ok_prompt2 and ok_recall
                   and ok_prompt3 and not alive) else 1)
finally:
    if p.poll() is None:
        p.kill()
    # transcript for post-mortem
    sys.stderr.write("\n----- RAW TRANSCRIPT -----\n")
    sys.stderr.write(buf.decode("utf8", "replace"))
