#!/bin/sh
cd /src/testsuite
echo "== tools list =="
for t in Bash Read Grep SubAgent; do
  echo -n "$t: "
  grep -m1 -o "\"$t\"" ba.testhome/tools.json 2>&1 | head -1
done
echo "== overwrite =="
BB_AGENT_HOME=$PWD/ba.testhome /src/busybox busyagent -i 2>&1
echo rc=$?
echo "== empty prompt =="
/src/busybox busyagent -u ftp://127.0.0.1:9/x -k k -m m < /dev/null 2>&1
echo rc=$?
echo "== -n twice =="
rm -rf /tmp/nh
BB_AGENT_HOME=/tmp/nh /src/busybox busyagent -n a -u ftp://127.0.0.1:9/x -k k -m m >/dev/null 2>&1
echo r1=$?
BB_AGENT_HOME=/tmp/nh /src/busybox busyagent -n b -u ftp://127.0.0.1:9/x -k k -m m >/dev/null 2>&1
echo r2=$?
ls -d /tmp/nh/projects/*/ | wc -l
