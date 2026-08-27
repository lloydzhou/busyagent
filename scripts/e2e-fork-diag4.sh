#!/bin/sh
export BB_AGENT_HOME=/tmp/bbf6
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbf6 /tmp/wire_*
cd /src
$BB busyagent -n "The magic word is zebra999. Just acknowledge briefly." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna >/dev/null 2>&1
$BB busyagent -v "Use the SubAgent tool now. child prompt: what is the magic word? answer with the word only. description: ask-child. fork=true. Then print only the reply you got from the child." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna >/tmp/t2.out 2>/tmp/t2.err
echo "T2_EXIT=$?"
grep -E "^\[tag\]|^\[diag\]|^busyagent: \[diag\]" /tmp/t2.err
echo "--- child events tail ---"
SUB=$(ls -d /tmp/bbf6/projects/-src/sub_* | tail -1)
tail -3 "$SUB/events.jsonl"
