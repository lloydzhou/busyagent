#!/bin/sh
# replay with sub_agent_result in history
export BB_AGENT_HOME=/tmp/bbreplay3
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbreplay3

printf 'use the SubAgent tool with prompt "say sub-marker-ok" and reply DONE\n' | \
  $BB busyagent -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna >/dev/null 2>&1
echo "subgen rc=$?"
grep -c "sub_agent_result" /tmp/bbreplay3/projects/-src/*/events.jsonl
sh /src/scripts/e2e-replay.sh 2>&1 | grep -E "sub-agent|replayed|prompt2"
