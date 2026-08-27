# two-stage: builder compiles static busybox(+busyagent); the final image
# is FROM scratch with only the rootfs contents. Multi-platform:
#
#   docker buildx build --platform linux/amd64,linux/arm64 \
#     -t lloydzhou/busyagent:latest -t lloydzhou/busybox:latest --push .
#
# Config policy: self-contained in this file. No committed .config.
#   make defconfig (kconfig defaults) then pin exactly four things:
#     CONFIG_STATIC=y                 zero-dependency binary for scratch
#     LAST_SUPPORTED_WCHAR=1114111    full CJK codepoint tables (input echo)
#     UNICODE_WIDE_WCHARS=y           CJK measured at 2 columns
#     CONFIG_BUSYAGENT=y              pulls our applet + its selects
#
FROM alpine:3.23 AS build
# TARGETARCH is injected by buildx. busybox's Makefile derives the CPU
# subdir from uname, which under qemu emulation reports the host arch -
# pin ARCH explicitly (same reason docker-library/busybox does).
ARG TARGETARCH

RUN apk add --no-cache gcc make musl-dev linux-headers findutils
WORKDIR /src
COPY . .

RUN case "$TARGETARCH" in \
        amd64)          export ARCH=x86_64  ;; \
        arm64)          export ARCH=aarch64 ;; \
        arm|arm/v7)     export ARCH=arm     ;; \
        386)            export ARCH=i386    ;; \
        ppc64le)        export ARCH=powerpc ;; \
        s390x)          export ARCH=s390    ;; \
        *)              export ARCH="$(uname -m)" ;; \
    esac; \
    echo "building for ARCH=$ARCH (TARGETARCH=$TARGETARCH)" && \
    make defconfig >/dev/null && \
    sed -i -e '/^CONFIG_STATIC=/d' -e '/^# CONFIG_STATIC is not set/d' .config && \
    echo 'CONFIG_STATIC=y' >> .config && \
    sed -i -e '/^CONFIG_LAST_SUPPORTED_WCHAR=/d' -e '/^# CONFIG_LAST_SUPPORTED_WCHAR is not set/d' .config && \
    echo 'CONFIG_LAST_SUPPORTED_WCHAR=1114111' >> .config && \
    sed -i -e '/^CONFIG_UNICODE_WIDE_WCHARS=/d' -e '/^# CONFIG_UNICODE_WIDE_WCHARS is not set/d' .config && \
    echo 'CONFIG_UNICODE_WIDE_WCHARS=y' >> .config && \
    sed -i -e '/^CONFIG_TC=/d' -e '/^# CONFIG_TC is not set/d' .config && \
    echo '# CONFIG_TC is not set' >> .config && \
    grep -v '^CONFIG_BUSYAGENT=' .config > .t && mv .t .config && \
    echo 'CONFIG_BUSYAGENT=y' >> .config && \
    yes "" | make oldconfig >/dev/null && \
    grep -q '^CONFIG_STATIC=y' .config && \
    grep -q '^CONFIG_LAST_SUPPORTED_WCHAR=1114111$' .config && \
    grep -q '^CONFIG_UNICODE_WIDE_WCHARS=y' .config && \
    grep -q '^CONFIG_BUSYAGENT=y' .config && \
    make ARCH="$ARCH" -j"$(nproc)"

# minimal rootfs: binary + every applet link + /etc basics, normalized mtimes.
# /root/.busyagent is the sessions/history mount point (declared VOLUME below).
RUN mkdir -p out-rootfs/bin out-rootfs/etc out-rootfs/root/.busyagent; \
    cp busybox out-rootfs/bin/; \
    printf 'root:x:0:0:root:/root:/bin/sh\n' > out-rootfs/etc/passwd; \
    printf 'root:x:0:\n' > out-rootfs/etc/group; \
    chroot out-rootfs /bin/busybox --install /bin

FROM scratch
COPY --from=build /src/out-rootfs/ /

ENV BB_AGENT_HOME=/root/.busyagent
VOLUME ["/root/.busyagent"]
WORKDIR /root

CMD ["sh"]
