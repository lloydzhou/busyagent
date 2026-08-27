#!/bin/sh
export BB_AGENT_HOME=/tmp/bbpty
export BB_AGENT_E2E_URL=http://host.docker.internal:8317/v1
rm -rf /tmp/bbpty
python3 /src/scripts/e2e-repl-pty.py
echo "PTY_EXIT=$?"
