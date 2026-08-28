#!/bin/sh
rm -rf /tmp/nh2
echo "== -n first =="
BB_AGENT_HOME=/tmp/nh2 /src/busybox busyagent -n aa -u ftp://127.0.0.1:9/x -k k -m m -v 2>&1 | grep -o "session=[^ ]* (.*)"
echo "== -n second =="
BB_AGENT_HOME=/tmp/nh2 /src/busybox busyagent -n bb -u ftp://127.0.0.1:9/x -k k -m m -v 2>&1 | grep -o "session=[^ ]* (.*)"
echo "== dirs =="
ls -d /tmp/nh2/projects/*/
echo "== empty prompt actual =="
/src/busybox busyagent -u ftp://127.0.0.1:9/x -k k -m m < /dev/null > /tmp/ep.out 2>/tmp/ep.err; echo "rc=$?"; echo "stdout:"; cat /tmp/ep.out; echo "stderr:"; cat /tmp/ep.err
