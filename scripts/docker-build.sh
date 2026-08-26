#!/bin/sh
# Build busybox + busyagent inside the busyagent-builder container.
# Usage: docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" \
#            busyagent-builder /src/scripts/docker-build.sh [target]
set -e
cd /src
target="${1:-allnoconfig}"
if [ "$target" = "defconfig" ]; then
	make defconfig >/dev/null
else
	make allnoconfig >/dev/null
	sed -i '/CONFIG_BUSYAGENT/d' .config
	echo "CONFIG_BUSYAGENT=y" >> .config
	make oldconfig >/dev/null
fi
make -j"$(nproc)"
echo "=== built ==="
ls -la busybox
./busybox busyagent --help 2>&1 | head -4
