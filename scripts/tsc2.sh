#!/bin/sh
cd /src/testsuite
rm -rf /tmp/nh
echo "== -n first =="
BB_AGENT_HOME=/tmp/nh /src/busybox busyagent -n aa -u ftp://127.0.0.1:9/x -k k -m m -v 2>&1 | grep -o "session=[^ ]* (.*)"
echo "== -n second =="
BB_AGENT_HOME=/tmp/nh /src/busybox busyagent -n bb -u ftp://127.0.0.1:9/x -k k -m m -v 2>&1 | grep -o "session=[^ ]* (.*)"
ls -d /tmp/nh/projects/*/
echo "== self-contained -i double =="
rm -rf /tmp/ih9
BB_AGENT_HOME=/tmp/ih9 /src/busybox busyagent -i >/dev/null 2>&1 && echo first-ok
BB_AGENT_HOME=/tmp/ih9 /src/busybox busyagent -i 2>&1; echo rc=$?
