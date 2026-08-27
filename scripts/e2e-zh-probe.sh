#!/bin/sh
# A) control experiment: does busybox ash show the same ?? in this container?
export BB_AGENT_HOME=/tmp/bbzh2 LANG=C.UTF-8 LC_ALL=C.UTF-8
rm -rf /tmp/bbzh2
cat > /tmp/probe.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
int main(void) {
    wchar_t w;
    char *loc = setlocale(LC_CTYPE, "");
    int r = mbstowcs(&w, "\xe4\xbd\xa0", 4);   /* 你 */
    printf("locale=%s r=%d wc=%x MB_CUR_MAX=%d\n",
           loc ? loc : "(null)", (int)r,
           r >= 1 ? (unsigned)w : 0u, (int)MB_CUR_MAX);
    return 0;
}
EOF
echo "--- setlocale/mbstowcs probe (no explicit setlocale var set) ---"
gcc /tmp/probe.c -o /tmp/probe && /tmp/probe
LC_ALL=C.UTF-8 /tmp/probe
echo "--- ash chinese echo via script(1) ---"
printf '你好\nexit\n' | script -q -c "/src/busybox sh" /dev/null 2>&1 | head -3 | od -c | head -6
