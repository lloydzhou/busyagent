# two-stage: builder compiles static busybox(+busyagent); the final image
# is FROM scratch with only the rootfs contents.
#
#   docker build -t busyagent-scratch .
#   docker run --rm -it busyagent-scratch sh
#   docker run --rm --network host -e BB_AGENT_HOME=/tmp \
#       busyagent-scratch busyagent -u http://127.0.0.1:8317/v1 "hi"
#
FROM alpine:3.23 AS build
RUN apk add --no-cache gcc make musl-dev linux-headers findutils
WORKDIR /src
COPY . .

# static link + full UTF-8 tables (interactive CJK input) + busyagent applet.
# .config from the repo is allnoconfig-based; pin what we need here.
RUN sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config || true; \
    grep -q '^CONFIG_STATIC=y' .config || echo 'CONFIG_STATIC=y' >> .config; \
    if grep -q '^CONFIG_LAST_SUPPORTED_WCHAR' .config; then \
        sed -i 's/^CONFIG_LAST_SUPPORTED_WCHAR=.*/CONFIG_LAST_SUPPORTED_WCHAR=1114111/' .config; \
    else echo 'CONFIG_LAST_SUPPORTED_WCHAR=1114111' >> .config; fi; \
    grep -q '^CONFIG_UNICODE_WIDE_WCHARS=y' .config || echo 'CONFIG_UNICODE_WIDE_WCHARS=y' >> .config; \
    grep -q '^CONFIG_BUSYAGENT=y' .config || echo 'CONFIG_BUSYAGENT=y' >> .config; \
    # alpine's current linux-headers dropped the CBQ constants busybox tc.c uses
    sed -i 's/^CONFIG_TC=.*/# CONFIG_TC is not set/' .config; \
    grep -q '^# CONFIG_TC is not set' .config || echo '# CONFIG_TC is not set' >> .config; \
    yes "" | make oldconfig >/dev/null; \
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
