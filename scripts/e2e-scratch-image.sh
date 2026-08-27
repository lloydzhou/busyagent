#!/bin/sh
# smoke test for the scratch image
echo "=== shell & applets ==="
docker run --rm busyagent-scratch sh -c 'echo shell-ok; ls /bin | wc -l; busybox | head -1; ls -la /bin/busyagent'
echo "=== busyagent entry (no URL -> usage path) ==="
docker run --rm -e HOME=/root busyagent-scratch sh -c 'HOME=/root /bin/busyagent "hi" 2>&1 | head -2'
echo "=== live LLM call inside scratch ==="
docker run --rm --network host \
  -e BB_AGENT_HOME=/tmp/bbs -e HOME=/tmp/bbs \
  busyagent-scratch busyagent -n "say scratch-ok" \
  -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna 2>/dev/null
