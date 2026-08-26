#!/bin/sh
# binary size report: with vs without CONFIG_BUSYAGENT
set -e
cd /src
measure() {
	make -j8 >/dev/null 2>&1
	ls -la busybox | awk '{print $5, $9}'
}
make allnoconfig >/dev/null 2>&1
sed -i '/CONFIG_BUSYAGENT/d' .config
echo "CONFIG_BUSYAGENT=n" >> .config
make oldconfig >/dev/null 2>&1
echo -n "allnoconfig WITHOUT busyagent: "; measure
sed -i '/CONFIG_BUSYAGENT/d' .config
echo "CONFIG_BUSYAGENT=y" >> .config
make oldconfig >/dev/null 2>&1
echo -n "allnoconfig WITH busyagent:    "; measure
make defconfig >/dev/null 2>&1
sed -i '/^CONFIG_TC=/d' .config; echo "CONFIG_TC=n" >> .config
sed -i '/CONFIG_FEATURE_SHOW_USAGE/d' .config
make oldconfig >/dev/null 2>&1
sed -i '/CONFIG_BUSYAGENT/d' .config
echo "CONFIG_BUSYAGENT=n" >> .config
make oldconfig >/dev/null 2>&1
echo -n "defconfig WITHOUT busyagent:   "; measure
sed -i '/CONFIG_BUSYAGENT/d' .config
echo "CONFIG_BUSYAGENT=y" >> .config
make oldconfig >/dev/null 2>&1
echo -n "defconfig WITH busyagent:      "; measure
