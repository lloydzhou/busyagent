#!/bin/sh
cd /src/testsuite
rm -rf ba.tl
BB_AGENT_HOME=$PWD/ba.tl /src/busybox busyagent -i; echo "rc=$?"
ls -la ba.tl/ 2>&1 | head -3
echo "== grep test =="
for t in Bash Read Grep SubAgent; do
  printf '%s: ' "$t"
  grep -m1 -o "\"$t\"" ba.tl/tools.json 2>&1 | head -1
done
echo "== head of file =="
head -c 200 ba.tl/tools.json 2>/dev/null
echo
