#!/bin/sh
# correct-path verification: py driver pins home to /tmp/bbpty itself
rm -rf /tmp/bbpty
python3 /src/scripts/e2e-repl-pty.py > /tmp/tt3.txt 2>&1
echo "driver rc=$?"
echo "--- history ---"
cat /tmp/bbpty/history 2>/dev/null || echo "(missing)"
grep -E "prompt shown|second prompt|memory|exited" /tmp/tt3.txt
