#!/bin/sh
# SubAgent alignment tests: sub_ session, fork inheritance, nesting guard
export BB_AGENT_HOME=/tmp/bbsub
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
MODEL=gpt-5.6-luna
rm -rf /tmp/bbsub /tmp/sub-fork-marker.txt
cd /src

echo "=== N1: plain delegation uses sub_ session dir ==="
$BB busyagent -n "Use SubAgent with prompt 'reply with exactly: delegated-ok' and description 'smoke'. Report only the sub agent's answer." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL
echo rc=$?
ls /tmp/bbsub/projects/-src/ | head -4

echo "=== N2: fork=true inherits parent history ==="
$BB busyagent "remember the word: pineapple42. just acknowledge." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL >/dev/null 2>&1
$BB busyagent "Use the SubAgent tool to run this prompt: 'What word did the previous conversation mention? Answer with the word only.' Set fork=true so it inherits our history. Report only its answer." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL
echo rc=$?

echo "=== N3: nesting guard ==="
$BB busyagent -n "Use the SubAgent tool and instruct the child to use SubAgent again (any prompt). Report what error text the tool returned about nesting, verbatim prefix only: first 60 chars." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL 2>&1 | head -6
