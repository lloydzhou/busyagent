#!/bin/sh
# A1: async task completion pushes result back automatically (no Read needed)
export BB_AGENT_HOME=/tmp/bbasyn
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbasyn
cd /src

$BB busyagent -n "Use the Bash tool with background=true and command 'sleep 2; echo async-marker-31337'. Reply only STARTED after the tool returns." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna 2>/tmp/a.err
echo "EXIT=$?"
echo "--- stderr diag ---"
grep -E "bg-bash|async" /tmp/a.err | head -4
echo "--- events async ---"
grep "async_task_result" /tmp/bbasyn/projects/-src/*/events.jsonl | head -2
