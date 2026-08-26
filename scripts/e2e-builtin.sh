#!/bin/sh
# builtin-11 + dynamic zone + state tools (TodoWrite/Plan) smoke
export BB_AGENT_HOME=/tmp/bb11
BB=/src/busybox
API=${BB_AGENT_E2E_URL:?set BB_AGENT_E2E_URL, e.g. http://host.docker.internal:PORT/v1}
KEY=${BB_AGENT_E2E_KEY:?set BB_AGENT_E2E_KEY}
MODEL=gpt-5.6-luna
rm -rf /tmp/bb11
cd /src

echo "=== B1: -i exports dynamic-zone starter (non-overlapping) ==="
$BB busyagent -i && grep -c '"name"' /tmp/bb11/tools.json

echo "=== B2: builtin tools work with NO tools.json (11 always present) ==="
rm -f /tmp/bb11/tools.json
$BB busyagent -n "Use the Read tool to read /src/agentutils/ba_tools.h and tell me: how many function declarations does it declare? Answer with the number only." -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== B3: TodoWrite returns checklist as tool_result (no file) ==="
$BB busyagent -n "Create a todo list with two items: 'step one' (in_progress) and 'step two' (pending), using TodoWrite. Then reply done." -u $API -k $KEY -m $MODEL >/dev/null 2>&1
grep -o "\"name\":\"TodoWrite\"[^\"]*.\"content\":\"[^\"]*" /tmp/bb11/.bash-agent/projects/-src/*/events.jsonl | tail -1 | head -c 120; echo
test ! -f /tmp/bb11/.bash-agent/projects/-src/*/todo.md && echo "(no todo.md file — correct, history is the state)"

echo "=== B4: Edit precision ==="
$BB busyagent -n "Use Write to create /tmp/edittest.txt with content 'hello alpha'. Then use Edit to replace 'alpha' with 'beta'. Then use Read to show the file. Reply with the final file content only." -u $API -k $KEY -m $MODEL
echo; echo "file: $(cat /tmp/edittest.txt 2>/dev/null)"
