#!/bin/sh
# trailing-newline check: answer must end with exactly one \n
export BB_AGENT_HOME=/tmp/bbnl
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
export BB_AGENT_E2E_KEY=sk-lloyd-1
BB=/src/busybox
rm -rf /tmp/bbnl
printf 'reply with exactly one line: alpha-bravo' | \
  $BB busyagent -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m gpt-5.6-luna 2>/dev/null | od -c | tail -3
