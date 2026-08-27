#!/bin/sh
# volume persistence: two runs against the same named volume must share
# session history (turn2 recalls turn1's token).
export E2E_URL=http://host.docker.internal:8317/v1
export E2E_KEY=sk-lloyd-1
export E2E_MODEL=gpt-5.6-luna
docker volume rm bbvol-test >/dev/null 2>&1 || true

echo "--- run 1: create token ---"
docker run --rm --network host -v bbvol-test:/root/.busyagent \
  -e BB_AGENT_BASE_URL="$E2E_URL" -e BB_AGENT_API_KEY="$E2E_KEY" -e BB_AGENT_MODEL="$E2E_MODEL" \
  lloydzhou/busyagent:vol-test busyagent "remember the token vol-kiwi-42 and just say ok" 2>&1 | tail -1

echo "--- run 2: same volume, fresh container ---"
docker run --rm --network host -v bbvol-test:/root/.busyagent \
  -e BB_AGENT_BASE_URL="$E2E_URL" -e BB_AGENT_API_KEY="$E2E_KEY" -e BB_AGENT_MODEL="$E2E_MODEL" \
  lloydzhou/busyagent:vol-test busyagent "what token did I tell you? answer with the token only" 2>&1 | tail -1

echo "--- volume contents ---"
docker run --rm -v bbvol-test:/root/.busyagent busyagent-scratch sh -c 'find /root/.busyagent -type f | head -4'
docker volume rm bbvol-test >/dev/null 2>&1 || true
