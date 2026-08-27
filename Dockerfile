# two-stage: builder compiles static busybox(+busyagent); the final image
# is FROM scratch with only the rootfs contents.
#
#   docker build -t lloydzhou/busyagent .
#
# Config policy: self-contained in this file. No committed .config.
#   make defconfig (kconfig defaults) then pin exactly four things:
#     CONFIG_STATIC=y                 zero-dependency binary for scratch
#     LAST_SUPPORTED_WCHAR=1114111    full CJK codepoint tables (input echo)
#     UNICODE_WIDE_WCHARS=y           CJK measured at 2 columns
#     CONFIG_BUSYAGENT=y              pulls our applet + its selects
#
FROM alpine:3.23 AS build
RUN apk add --no-cache gcc make musl-dev linux-headers findutils
WORKDIR /src
COPY . .

RUN make defconfig >/dev/null && \
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
    make -j"$(nproc)"

# minimal rootfs: binary + every applet link + /etc basics, normalized mtimes
RUN mkdir -p out-rootfs/bin out-rootfs/etc out-rootfs/root; \
    cp busybox out-rootfs/bin/; \
    printf 'root:x:0:0:root:/root:/bin/sh\n' > out-rootfs/etc/passwd; \
    printf 'root:x:0:\n' > out-rootfs/etc/group; \
    chroot out-rootfs /bin/busybox --install /bin

FROM scratch
COPY --from=build /src/out-rootfs/ /
CMD ["sh"]
