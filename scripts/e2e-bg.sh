#!/bin/sh
# Bash background=true smoke: task starts detached, output lands in temp file
export BB_AGENT_HOME=/tmp/bbbg
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
MODEL=gpt-5.6-luna
rm -rf /tmp/bbbg /tmp/ba_async_bash_*
cd /src

$BB busyagent -n "Use the Bash tool with background=true and command 'sleep 1; echo bg-marker-4242'. Then reply STARTED." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL
echo rc=$?
sleep 2
echo "--- async temp files ---"
ls /tmp/ba_async_bash_* 2>/dev/null || echo "(none)"
for f in /tmp/ba_async_bash_*; do [ -f "$f" ] && echo "--- content of $f ---" && cat "$f"; done 2>/dev/null
