#!/bin/sh
# REPL smoke: prompt string must exist; pty test via script(1)
export BB_AGENT_HOME=/tmp/bbrepl
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbrepl

strings "$BB" | grep -q "busyagent> " && echo "REPL prompt compiled in: yes" || echo "MISSING"

# simulate a tty via script(1); feed two lines then Ctrl-D
printf 'reply with exactly: repl-ok\nquit\n' | script -qec \
  "$BB busyagent -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna" \
  /dev/null > /tmp/repl.out 2>&1
grep -o "repl-ok" /tmp/repl.out | head -1
grep -c "busyagent> " /tmp/repl.out
