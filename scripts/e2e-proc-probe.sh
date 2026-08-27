#!/bin/sh
# 分辨 'process dead' vs 'process hung': run REPL slow, inspect /proc mid-flight
export BB_AGENT_HOME=/tmp/bbptys
rm -rf /tmp/bbptys
( printf 'remember the token pty-mango-77. just acknowledge briefly.\n'
  sleep 45
  printf 'quit\n' ) | script -q -c "/src/busybox busyagent -u http://host.docker.internal:8317/v1 -k sk-lloyd-1 -m gpt-5.6-luna" /tmp/typescript.txt &
SCRIPT_PID=$!
sleep 12   # 让第一回合跑起来
BPID=$(pgrep -f "busyagent -u" | head -1)
echo "--- mid-flight probe ---"
if [ -n "$BPID" ]; then
  echo "pid=$BPID state=$(awk '/^State/{print $2,$3}' /proc/$BPID/status)"
  echo "threads=$(ls /proc/$BPID/task | wc -l)"
  for t in /proc/$BPID/task/*; do
    echo "tid=$(basename $t) wchan=$(cat $t/wchan 2>/dev/null) stat=$(awk '/^State/{print $2}' $t/status)"
    head -3 "$t/stack" 2>/dev/null
  done
  ls -la /proc/$BPID/fd 2>/dev/null | tail -5
else
  echo "NO busyagent process!"
fi
wait $SCRIPT_PID
echo "--- typescript tail ---"
tail -12 /tmp/typescript.txt | cut -c1-110
