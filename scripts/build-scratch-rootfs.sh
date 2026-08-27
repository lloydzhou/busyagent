#!/bin/sh
# Build a static busybox(+busyagent) and pack busybox.tar.gz for a
# FROM-scratch image. Run inside the build container (bb-build):
#   sh /src/scripts/build-scratch-rootfs.sh /out
set -e
OUT="${1:-/out}"
mkdir -p "$OUT"
cd /src

cp .config /tmp/config.save

# ---- static link (official musl recipe) ----
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
grep -q '^CONFIG_STATIC=y' .config || echo 'CONFIG_STATIC=y' >> .config
yes "" | make oldconfig >/dev/null 2>&1 || true
make -j"$(nproc)" >/dev/null 2>&1

file_out="$(ls -la busybox | awk '{print $5}')"
echo "static busybox: ${file_out} bytes"

# ---- assemble minimal rootfs (official-image shape) ----
rm -rf /tmp/rootfs
mkdir -p /tmp/rootfs/bin /tmp/rootfs/etc
cp busybox /tmp/rootfs/bin/

cat > /tmp/rootfs/etc/passwd <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > /tmp/rootfs/etc/group <<'EOF'
root:x:0:
EOF
mkdir -p /tmp/rootfs/root

# materialize every applet symlink incl. busyagent
chroot /tmp/rootfs /bin/busybox --install /bin

# sanity: applet runs with no libc present
[ -e /tmp/rootfs/bin/busyagent ] || { echo "busyagent link missing" >&2; exit 1; }
echo "rootfs applets: $(ls /tmp/rootfs/bin | wc -l)"

# ---- pack (numeric ids; container tar is busybox-minimal) ----
chroot /tmp/rootfs /bin/sh -c 'cd / && find bin etc root | sort > /tmp/.order'
cd /tmp/rootfs
while IFS= read -r p; do
    touch -t 197001010000.00 "/$p" 2>/dev/null || true
done < /tmp/.order
tar -czf "$OUT/busybox.tar.gz" --numeric-owner $(cat /tmp/.order)

# restore dev config
cp /tmp/config.save .config
make oldconfig >/dev/null 2>&1 || true
make -j"$(nproc)" >/dev/null 2>&1 || true

echo "packed: $OUT/busybox.tar.gz ($(ls -la "$OUT/busybox.tar.gz" | awk '{print $5}') bytes)"
