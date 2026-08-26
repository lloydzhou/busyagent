#!/bin/sh
# tool-loop + json output acceptance
export BB_AGENT_HOME=/tmp/bbtools
BB=/src/busybox
API=${BB_AGENT_E2E_URL:?set BB_AGENT_E2E_URL, e.g. http://host.docker.internal:PORT/v1}
KEY=${BB_AGENT_E2E_KEY:?set BB_AGENT_E2E_KEY}
MODEL=gpt-5.6-luna
rm -rf /tmp/bbtools
cd /src
echo "=== T0: -i exports starter tools.json ==="
$BB busyagent -i
echo "rc=$?"

echo "=== T5: tool convergence (needs Grep/Read on real file) ==="
$BB busyagent -n "What is the value of BA_MAX_TOKENS in the file agentutils/busyagent.c under the current directory? Answer with the number only, no other text." -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== T6: json output mode ==="
$BB busyagent -n "say ok" -u $API -k $KEY -m $MODEL -o json | head -5
echo "rc=$?"

echo "=== T7: events trace ==="
head -4 /tmp/bbtools/.bash-agent/projects/-src/*/events.jsonl
