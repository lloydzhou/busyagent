#!/bin/sh
# capture the full pty transcript including stderr tags
export BB_AGENT_HOME=/tmp/bbhist3
rm -rf /tmp/bbhist3
python3 /src/scripts/e2e-repl-pty.py > /tmp/tt.txt 2>&1
grep -E "\[hist\]|busyagent|SEGV" /tmp/tt.txt | head -6
