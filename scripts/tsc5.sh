#!/bin/sh
cd /src/testsuite
rm -rf ba.th
mkdir -p ba.th
printf '[{"type": "function", "function": {"name": "MyProbe", "description": "%s", "parameters": {"type": "object", "properties": {}}}}]' "$(head -c 600 /dev/zero | tr '\\0' x)" > ba.th/tools.json
echo "== B1 run =="
BB_AGENT_HOME=$PWD/ba.th /src/busybox busyagent hi -u ftp://127.0.0.1:9/x -k k -m m -v 2>&1 | grep -o "request body ([0-9]* bytes)"
rm ba.th/tools.json
echo "== B2 run =="
BB_AGENT_HOME=$PWD/ba.th /src/busybox busyagent hi -u ftp://127.0.0.1:9/x -k k -m m -v 2>&1 | grep -o "request body ([0-9]* bytes)"
echo "== overwrite direct =="
rm -rf ba.tw
BB_AGENT_HOME=$PWD/ba.tw /src/busybox busyagent -i >/dev/null 2>&1; echo first=$?
BB_AGENT_HOME=$PWD/ba.tw /src/busybox busyagent -i 2>&1 | sed 's|.*/tools.json|tools.json|'
