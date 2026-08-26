#!/bin/sh
# E2E test for busyagent inside the container (runs against host gateway)
set -e
export BB_AGENT_HOME=${BB_AGENT_HOME:-/tmp/bbhome}
BB=/src/busybox
API=$BB_AGENT_E2E_URL
KEY=$BB_AGENT_E2E_KEY
MODEL=gpt-5.6-luna

echo "=== T1: basic single-turn ==="
$BB busyagent "reply with exactly: pong" -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== T2: memory-by-default (second call sees history) ==="
rm -rf /tmp/bbhome
$BB busyagent "my favorite number is 42. just acknowledge." -u $API -k $KEY -m $MODEL >/dev/null 2>&1
$BB busyagent "what is my favorite number? reply with the number only" -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== T3: --new isolation ==="
$BB busyagent -n "what is my favorite number? reply with the number only" -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== T4: stdin pipe ==="
echo "reply with exactly: piped" | $BB busyagent -u $API -k $KEY -m $MODEL
echo "rc=$?"
