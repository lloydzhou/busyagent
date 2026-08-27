# two-stage: builder compiles static busybox(+busyagent); the final image
# is FROM scratch with only the rootfs contents.
#
#   docker build -t lloydzhou/busyagent .
#
# Config policy (single source of truth):
#   busybox.config  <- the ONLY configuration input, committed to the repo
#                      and read via KCONFIG_CONFIG. It already pins:
#                        CONFIG_STATIC=y                   (scratch-ready)
#                        LAST_SUPPORTED_WCHAR=1114111      (CJK input echo)
#                        UNICODE_WIDE_WCHARS=y             (CJK 2-column width)
#                        # CONFIG_TC is not set            (alpine headers)
#   The RUN below only *asserts* these invariants so a config regression
#   fails the build instead of silently producing a degraded binary.
#
FROM alpine:3.23 AS build
RUN apk add --no-cache gcc make musl-dev linux-headers findutils
WORKDIR /src
COPY . .

ENV KCONFIG_CONFIG=busybox.config

RUN yes "" | make oldconfig >/dev/null && \
    grep -q '^CONFIG_STATIC=y' busybox.config && \
    grep -q '^CONFIG_LAST_SUPPORTED_WCHAR=1114111$' busybox.config && \
    grep -q '^CONFIG_UNICODE_WIDE_WCHARS=y' busybox.config && \
    grep -q '^# CONFIG_TC is not set$' busybox.config && \
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
