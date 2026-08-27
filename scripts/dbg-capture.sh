#!/bin/sh
mkdir -p /tmp/proj-idx/skills/demo-skill
printf '# demo-skill\ndescription: A demo skill\n' > /tmp/proj-idx/skills/demo-skill/SKILL.md
printf '# Project instructions here\n' > /tmp/proj-idx/AGENTS.md
(nc -l -p 18999 > /tmp/captured.raw) &
LIST=$!
sleep 0.7
cd /tmp/proj-idx
/src/busybox busyagent -n "hi" -u http://127.0.0.1:18999/v1/chat/completions -k test -m gpt-5.6-luna >/dev/null 2>&1
sleep 0.5
kill $LIST 2>/dev/null
echo "--- prompt blocks in captured request ---"
for t in agent-identity environment rules using-your-tools sub-agent-guidance todo-guidance plan-lifecycle-guidance instruction-file skill-index; do
  printf "%-28s %s\n" "$t" "$(grep -c "<$t" /tmp/captured.raw)"
done
