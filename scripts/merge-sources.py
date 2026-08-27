#!/usr/bin/env python3
"""Merge agentutils/ba_*.c into agentutils/ba_impl.c and the ba_*.h into
agentutils/busyagent.h. Removes per-file includes of merged headers."""

import re, os

os.chdir(os.path.join(os.path.dirname(__file__) or '.', '..') or '.')
os.chdir('agentutils')

impl_files = ['ba_util.c', 'ba_json.c', 'bb_http.c', 'ba_store.c',
              'ba_display.c', 'ba_transport.c', 'ba_prompt.c', 'ba_tools.c']
hdr_files = ['ba_util.h', 'ba_json.h', 'ba_store.h', 'ba_display.h',
             'ba_transport.h', 'ba_prompt.h', 'ba_tools.h']

def strip_includes(src, dropped):
    out = []
    for line in src.splitlines():
        m = re.match(r'#include\s+"(ba_[a-z_]+\.h|bb_http\.h)"', line)
        if m:
            dropped.add(m.group(1))
            continue
        out.append(line)
    return '\n'.join(out)

dropped = set()

# ---- busyagent.h ----
hdr_parts = ['#ifndef BUSYAGENT_H\n#define BUSYAGENT_H\n\n']
for h in hdr_files:
    src = open(h).read()
    hdr_parts.append(f'/* ==== {h} ==== */\n')
    hdr_parts.append(strip_includes(src, dropped).strip() + '\n\n')
hdr_parts.append('#endif /* BUSYAGENT_H */\n')
open('busyagent.h', 'w').write(''.join(hdr_parts))

# ---- ba_impl.c ----
impl_parts = ['''/*
 * ba_impl.c - support implementation for the busyagent applet.
 *
 * Everything the applet needs besides its main loop lives here, in
 * dependency order: utils, JSON parser/serializer, HTTP client (SSE),
 * session store, display rendering, protocol adaptation, system prompt
 * assembly and the busybox tool executor.
 */
#include "busyagent.h"
#include "ba_builtin_schemas.h"
''']
for f in impl_files:
    src = open(f).read()
    impl_parts.append(f'\n/* ==== {f} ==== */\n')
    impl_parts.append(strip_includes(src, dropped).strip() + '\n')
open('ba_impl.c', 'w').write(''.join(impl_parts))

print('merged:', ', '.join(impl_files), '-> ba_impl.c')
print('merged:', ', '.join(hdr_files), '-> busyagent.h')
print('dropped includes:', sorted(dropped))
