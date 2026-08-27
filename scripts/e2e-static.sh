#!/bin/sh
# static-build experiment: does the official-image recipe (CONFIG_STATIC=y)
# work with our agentutils tree, and what does it cost in bytes?
set -e
cd /src
cp .config /tmp/config.bak
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
yes "" | make oldconfig >/dev/null 2>&1 || true
grep -q "^CONFIG_STATIC=y" .config || echo "CONFIG_STATIC=y" >> .config
make oldconfig >/dev/null 2>&1
make -j8 >/dev/null 2>&1
echo "rc=$?"
ls -la busybox | awk '{print "static busybox:", $5, "bytes"}'
./busybox busyagent 2>&1 | head -2 || true
cp .config /tmp/config.static
cp /tmp/config.bak .config
