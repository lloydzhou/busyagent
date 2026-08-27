#!/bin/sh
# full cycle via the standard pty driver, then inspect history file
export BB_AGENT_HOME=/tmp/bbhist
rm -rf /tmp/bbhist
BB_AGENT_HOME=/tmp/bbhist python3 /src/scripts/e2e-repl-pty.py >/dev/null 2>&1
echo "driver rc=$?"
echo "--- history file ---"
cat /tmp/bbhist/history 2>/dev/null || echo "(missing)"
