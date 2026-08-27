#!/bin/sh
# deterministic async-bash e2e against stub LLM
export BB_AGENT_HOME=/tmp/bbasync2
BB=/src/busybox
rm -rf /tmp/bbasync2
cd /src

(python3 /src/scripts/stub-llm.py 18997 &) 
sleep 0.6
$BB busyagent -n "start the background job" -u http://127.0.0.1:18997/v1/chat/completions -k stub -m stub
echo "EXIT=$?"
sleep 0.3
pkill -f stub-llm.py 2>/dev/null
echo "--- events async_task_result ---"
grep "async_task_result" /tmp/bbasync2/projects/-src/*/events.jsonl | head -1
