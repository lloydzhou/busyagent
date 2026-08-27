#!/bin/sh
export BB_AGENT_HOME=/tmp/bbfork2
BB=/src/busybox
rm -rf /tmp/bbfork2
cd /src
$BB busyagent -v -n "The magic word is zebra999. Just acknowledge briefly." -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna >/dev/null 2>&1
$BB busyagent -v "Use the SubAgent tool: ask the child 'what is the magic word? answer with the word only' with fork=true. Then output only the child's reply." -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna 2>&1 | grep -E "verbose|error" | head -20
