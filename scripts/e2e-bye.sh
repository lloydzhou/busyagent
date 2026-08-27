#!/bin/sh
export BB_AGENT_HOME=/tmp/bbbye
rm -rf /tmp/bbbye
python3 /src/scripts/e2e-repl-pty.py > /tmp/bye.txt 2>&1
echo "rc=$?"
tail -5 /tmp/bye.txt | cut -c1-95
