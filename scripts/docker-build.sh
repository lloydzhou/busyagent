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
	# lineedit 输入回显需要的完整码表与 CJK 宽度：
	# allnoconfig 把 LAST_SUPPORTED_WCHAR 截到 767(0x2FF)，
	# CJK 码点会被 wcwidth 判为不可显示而画成 '?'（ash 同病）
	cat >> .config <<'EOF'
CONFIG_BUSYAGENT=y
CONFIG_LAST_SUPPORTED_WCHAR=1114111
CONFIG_UNICODE_WIDE_WCHARS=y
EOF
	make oldconfig >/dev/null
fi
make -j"$(nproc)"
echo "=== built ==="
ls -la busybox
./busybox busyagent --help 2>&1 | head -4
