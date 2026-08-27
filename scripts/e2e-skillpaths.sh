#!/bin/sh
# skill multi-path search + new $BB_AGENT_HOME/projects layout
export BB_AGENT_HOME=/tmp/bbskill2
export BB_AGENT_E2E_URL=${BB_AGENT_E2E_URL:?}
export BB_AGENT_E2E_KEY=${BB_AGENT_E2E_KEY:?}
BB=/src/busybox
MODEL=gpt-5.6-luna
rm -rf /tmp/bbskill2 /tmp/proj-skills
cd /src

echo "=== K1: project-level skill (cwd/skills) ==="
mkdir -p /tmp/proj-skills/skills/proj-skill
echo "# proj-skill" > /tmp/proj-skills/skills/proj-skill/SKILL.md
echo "When loaded, always end with [PROJ]" >> /tmp/proj-skills/skills/proj-skill/SKILL.md
(cd /tmp/proj-skills && $BB busyagent -n "Load skill proj-skill, then say hi." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL)
echo "rc=$?"

echo "=== K2: busyagent-home skill fallback ==="
mkdir -p /tmp/bbskill2/skills/home-skill
echo "# home-skill" > /tmp/bbskill2/skills/home-skill/SKILL.md
echo "When loaded, always end with [HOME]" >> /tmp/bbskill2/skills/home-skill/SKILL.md
$BB busyagent -n "Load skill home-skill, then say hi." -u "$BB_AGENT_E2E_URL" -k "$BB_AGENT_E2E_KEY" -m $MODEL
echo "rc=$?"

echo "=== K3: new projects layout ==="
ls -d /tmp/bbskill2/projects/-tmp-proj-skills 2>/dev/null || ls /tmp/bbskill2/projects/ | head -3
