#!/bin/sh
# tool-loop + json output acceptance
export BB_AGENT_HOME=/tmp/bbtools
BB=/src/busybox
API=$BB_AGENT_E2E_URL
KEY=$BB_AGENT_E2E_KEY
MODEL=gpt-5.6-sol
rm -rf /tmp/bbtools
cd /src

echo "=== T5: tool convergence (needs Grep/Read on real file) ==="
$BB busyagent -n "What is the value of BA_MAX_TOKENS in the file agentutils/busyagent.c under the current directory? Answer with the number only, no other text." -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== T6: json output mode ==="
$BB busyagent -n "say ok" -u $API -k $KEY -m $MODEL -o json | head -5
echo "rc=$?"

echo "=== T7: events trace ==="
head -4 /tmp/bbtools/.bash-agent/projects/-src/*/events.jsonl
