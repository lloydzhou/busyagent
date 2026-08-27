#!/bin/sh
# T3 isolated rerun under a clean home
export BB_AGENT_HOME=/tmp/bbt3
BB=/src/busybox
rm -rf /tmp/bbt3
cd /src
$BB busyagent "my favorite number is 42. just acknowledge." -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna >/dev/null 2>&1
echo "--- second call (default continue) ---"
$BB busyagent "what is my favorite number? reply with the number only" -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna
echo "--- third call (-n new) ---"
$BB busyagent -n "what is my favorite number? reply with the number only" -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna
