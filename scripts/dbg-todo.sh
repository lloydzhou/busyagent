#!/bin/sh
export BB_AGENT_HOME=/tmp/bb3
rm -rf /tmp/bb3
cd /src
/src/busybox busyagent -n "Use TodoWrite to create a todo list with exactly two items: step one (in_progress), step two (pending). Then reply OK." -u "${BB_AGENT_E2E_URL:?}" -k "${BB_AGENT_E2E_KEY:?}" -m gpt-5.6-luna
echo rc=$?
echo "--- todo.md ---"
cat /tmp/bb3/projects/-src/*/todo.md 2>/dev/null || echo "(missing)"
