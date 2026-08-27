#!/bin/sh
# composition analysis: how much of the extra bytes is ours?
cd /src
echo "=== static final binary ==="
ls -la busybox | awk '{print $5, "bytes"}'
echo ""
echo "=== per-object sizes (top 15 of agentutils) ==="
for o in agentutils/*.o; do
    printf "%8d  %s\n" "$(stat -c %s "$o")" "$o"
done | sort -rn | head -15
echo ""
echo "=== total agentutils contribution ==="
total=0
for o in agentutils/*.o; do
    s=$(stat -c %s "$o"); total=$((total + s))
done
echo "sum(object files) = $total bytes (before section GC)"
echo ""
echo "=== big busybox subsystem objects present (sample) ==="
for f in libbb/lineedit.o libbb/unicode.o networking/libiproute/lib.a coreutils/lib.a; do
    [ -e "$f" ] && stat -c "%s %n" "$f"
done
