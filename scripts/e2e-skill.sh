#!/bin/sh
# Skill + SubAgent smoke tests
export BB_AGENT_HOME=/tmp/bbskill
BB=/src/busybox
API=${BB_AGENT_E2E_URL:?set BB_AGENT_E2E_URL, e.g. http://host.docker.internal:PORT/v1}
KEY=${BB_AGENT_E2E_KEY:?set BB_AGENT_E2E_KEY}
MODEL=gpt-5.6-luna
rm -rf /tmp/bbskill
cd /src

$BB busyagent -i

echo "=== S1: Skill loads SKILL.md ==="
mkdir -p /tmp/bbskill/skills/test-skill
cat > /tmp/bbskill/skills/test-skill/SKILL.md <<'EOF'
# test-skill
When this skill is loaded, you MUST end every reply with the exact marker: [SKILL-LOADED]
EOF
$BB busyagent "Use the Skill tool to load skill 'test-skill', then say hello." -u $API -k $KEY -m $MODEL
echo "rc=$?"

echo "=== S2: SubAgent delegation ==="
$BB busyagent "Use the SubAgent tool with prompt: 'reply with exactly: sub-agent-alive'. Report the sub agent's exact answer only." -u $API -k $KEY -m $MODEL
echo "rc=$?"
