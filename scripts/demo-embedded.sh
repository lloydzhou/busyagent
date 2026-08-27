#!/bin/sh
# Embedded-scenario simulation:
#   a resource-starved, read-only-root busybox device where an init
#   script fails. busyagent must find the root cause using only
#   busybox applets present on the "device".
#
# constraints applied to the container:
#   --memory 64m        small RAM device
#   --cpus 0.5          weak core
#   --read-only         squashfs-style read-only rootfs
#   --tmpfs /tmp:8m     the only writable area (jffs2 stand-in)
#   no HOME             device has no HOME env; BB_AGENT_HOME -> /tmp/agent
set -e
IMG=lloydzhou/busyagent:latest
URL=http://host.docker.internal:8317/v1

# the "faulty init script" - two real problems for the agent to find:
#   1. calls /usr/bin/demo-daemon which does not exist on the device
#   2. a stray ^M (CRLF) at the end of the shebang line
PAYLOAD='#!/bin/sh\r
\r
start() {\r
    /usr/bin/demo-daemon --port 8080 &\r
}\r
\r
case "$1" in\r
    start) start ;;\r
esac\r
'

docker run --rm --network host \
    --memory 64m --cpus 0.5 --read-only \
    --tmpfs /tmp:rw,size=8m,noexec,nodev,nosuid \
    -e BB_AGENT_BASE_URL="$URL" -e BB_AGENT_API_KEY=sk-lloyd-1 \
    -e BB_AGENT_MODEL=gpt-5.6-luna \
    "$IMG" sh -c "
set -e
printf '$PAYLOAD' > /tmp/S99demo        # device drops the file in /tmp for triage
export BB_AGENT_HOME=/tmp/agent         # writable area = sessions + history

busyagent '这台设备上 /tmp/S99demo 是一个开机启动脚本但服务一直没起来。请只使用本机 busybox 工具（cat grep sh ls wc od head 等）排查出全部根因，并按重要性列出。不要尝试修改文件。'
"
