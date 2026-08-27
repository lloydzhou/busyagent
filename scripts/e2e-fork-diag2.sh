#!/bin/sh
export BB_AGENT_HOME=/tmp/bbf4
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbf4
cd /src
$BB busyagent -n "The magic word is zebra999. Just acknowledge briefly." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna >/dev/null 2>&1
echo "T1_EXIT=$?"
$BB busyagent "Use the SubAgent tool now. child prompt: what is the magic word? answer with the word only. description: ask-child. fork=true. Then print only the reply you got from the child." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna > /tmp/t2.out 2> /tmp/t2.err
echo "T2_EXIT=$?"
echo "--- t2.stdout ---"; cat /tmp/t2.out
echo "--- t2.stderr ---"; cat /tmp/t2.err
