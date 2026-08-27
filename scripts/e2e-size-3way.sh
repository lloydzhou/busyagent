#!/bin/sh
# three-way static size comparison, with forced relinks.
#   A) baseline busybox (no busyagent, no CJK tables)   <- official-image-ish
#   B) A + CJK input tables (LAST=1114111 + WIDE)
#   C) B + busyagent                                     <- what we ship
set -e
cd /src
cp .config /tmp/config.full

pin_agent_off() {
    sed -i '/^CONFIG_BUSYAGENT=/d; /^# CONFIG_BUSYAGENT is not set/d' .config
    echo '# CONFIG_BUSYAGENT is not set' >> .config
    yes "" | make oldconfig >/dev/null
}
rebuild() {
    rm -f busybox busybox_unstripped
    make -j8 >/tmp/mk.log 2>&1 || { tail -4 /tmp/mk.log; exit 1; }
}

# ---- A ----
pin_agent_off
sed -i 's/^CONFIG_LAST_SUPPORTED_WCHAR=.*/CONFIG_LAST_SUPPORTED_WCHAR=767/' .config
sed -i 's/^CONFIG_UNICODE_WIDE_WCHARS=y/# CONFIG_UNICODE_WIDE_WCHARS is not set/' .config
yes "" | make oldconfig >/dev/null
rebuild
A=$(stat -c %s busybox); echo "A) baseline (no agent, no CJK tables): $A"

# ---- B ----
sed -i 's/^CONFIG_LAST_SUPPORTED_WCHAR=.*/CONFIG_LAST_SUPPORTED_WCHAR=1114111/' .config
sed -i 's/^# CONFIG_UNICODE_WIDE_WCHARS is not set/CONFIG_UNICODE_WIDE_WCHARS=y/' .config
yes "" | make oldconfig >/dev/null
rebuild
B=$(stat -c %s busybox); echo "B) + CJK tables                        : $B"

# ---- C ----
cp /tmp/config.full .config
yes "" | make oldconfig >/dev/null
grep -q '^CONFIG_BUSYAGENT=y' .config || echo 'CONFIG_BUSYAGENT=y' >> .config
yes "" | make oldconfig >/dev/null
rebuild
C=$(stat -c %s busybox); echo "C) + busyagent (= what we ship)        : $C"

awk -v a=$A -v b=$B -v c=$C 'BEGIN{
  printf "\n--- deltas ---\n";
  printf "CJK input support      : %+d bytes\n", b-a;
  printf "busyagent itself       : %+d bytes\n", c-b;
  printf "total over official-ish: %+d bytes (%.0f%%)\n", c-a, (c-a)/a*100;
}'
