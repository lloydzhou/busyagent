#!/bin/sh
# verify zh echo + persistent history file
export BB_AGENT_HOME=/tmp/bbh2 BB_AGENT_E2E_URL=x
rm -rf /tmp/bbh2
python3 /src/scripts/e2e-zh-repl.py 2>/dev/null | grep "echo contains"
ls /tmp/bbh2/history 2>/dev/null && echo "history file exists" || echo "no history file"
