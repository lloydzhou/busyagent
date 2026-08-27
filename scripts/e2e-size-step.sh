#!/bin/sh
# manual single-step: remove agent, verify, force relink, measure
cd /src
sed -i '/^CONFIG_BUSYAGENT=y/d' .config
yes "" | make oldconfig >/dev/null
echo "BUSYAGENT lines left: $(grep -c CONFIG_BUSYAGENT .config)"
grep -E "ENABLE_FEATURE_EDITING |ENABLE_FEATURE_EDITING$" include/autoconf.h | head -1
grep -c "CONFIG_BUSYAGENT" include/autoconf.h || echo "autoconf.h clean"
rm -f busybox busybox_unstripped
make -j8 >/tmp/mk.log 2>&1 || { tail -5 /tmp/mk.log; exit 1; }
ls -la busybox | awk '{print "A-relink:", $5}'
./busybox busyagent "x" 2>&1 | head -1
