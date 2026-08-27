#!/bin/sh
# N2 forensic: after a fork=true delegation, is the parent history physically
# present in the sub session's conversation.jsonl?
export BB_AGENT_HOME=/tmp/bbfork
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
MODEL=gpt-5.6-luna
rm -rf /tmp/bbfork
cd /src

$BB busyagent -n "The magic word is zebra999. Just acknowledge briefly." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL >/dev/null 2>&1
$BB busyagent "Use the SubAgent tool: ask the child 'what is the magic word? answer with the word only' with fork=true. Then output only the child's reply." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL 2>/dev/null

echo "--- latest sub conversation ---"
SUB=$(ls -t /tmp/bbfork/projects/-src/sub_* 2>/dev/null | head -1)
echo "$SUB"
grep -c zebra999 "$SUB/conversation.jsonl" 2>/dev/null || echo "0 occurrences (inheritance broken)"
head -c 300 "$SUB/conversation.jsonl" 2>/dev/null
