#!/bin/sh
export BB_AGENT_HOME=/tmp/bbsys
rm -rf /tmp/bbsys
cd /src
mkdir -p /tmp/proj-idx/skills/demo-skill
printf '# demo-skill\ndescription: A demo skill for the index\n' > /tmp/proj-idx/skills/demo-skill/SKILL.md
cd /tmp/proj-idx
echo "say ok" | /src/busybox busyagent -v -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna -o json >/dev/null 2>/tmp/prompt.log
grep "Request body" /tmp/prompt.log | head -1
grep -o "<[a-z-]*>" /tmp/prompt.log | sort | uniq -c | head -14
