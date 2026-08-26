# agentutils — busyagent applet

One invocation = one user turn of an LLM agent loop: resolve the
cwd-bound session (default resumes the latest), rebuild its history
into the request prefix, stream the reply over plain HTTP (SSE),
execute tool calls with busybox applets, feed results back until the
model stops, append the turn's events to the session trace, exit.

## Files

| File | Role |
|---|---|
| `busyagent.c` | CLI (getopt32), session resolution, turn loop, renderer/trace |
| `bb_http.c` | plain-HTTP transport on libbb primitives (no curl); phase-2 TLS lands here |
| `ba_tools.c` | table-driven tool execution (see below) |
| `ba_{json,util,protocol,store,transport}.[ch]` | pure-C modules ported from [bash-agent](https://github.com/lloydzhou/bash-agent), curl paths removed |
| `tools.json` | **source of the embedded default tool table** — see below |

## tools.json lifecycle (read this before "why is there a json in the repo")

`tools.json` in this directory is *not* read at runtime. It is the
maintainer-editable source of the default table, converted at build
time into `ba_tools_embed.h` (C array) and linked into the binary:

```
agentutils/tools.json --(build-time generator)--> ba_tools_embed.h --> busybox
                                                                              |
                                                  first run / broken file     | seed + fallback
                                                                              v
                                                     $BB_AGENT_HOME/tools.json   <- runtime truth
                                                                    (user-editable; delete to re-seed)
```

Rationale: a bare busybox binary must work with zero external files
(busybox philosophy), so the default must be embedded somewhere; a JSON
source file is just the maintainable form of that embedding.

Each tool entry carries an `exec` mapping that never reaches the LLM
(stripped by `ba_tools_json()`):

```json
"exec": { "applet": "grep", "argv": ["-nH", "-r", "-e", "$pattern", "$path"] }
```

At execution time `$var` placeholders expand from the model's input
JSON (missing optional keys drop their argument). NOFORK applets are
called in-process via `run_nofork_applet()` inside a forked child;
anything else execs busybox itself. Children are SIGKILLed on timeout.

To regenerate `ba_tools_embed.h` after editing `tools.json`:

```
python3 - <<'EOF'
data = open('agentutils/tools.json','rb').read()
lines = ['/* tools.json embedded at build time - generated from agentutils/tools.json, do not edit */',
         'static const char embedded_tools_json[] = {']
for i in range(0, len(data), 20):
    lines.append('\t' + ','.join(str(b) for b in data[i:i+20]) + ',')
lines.append('\t0 };')
lines.append('#define embedded_tools_json_len %d' % len(data))
open('agentutils/ba_tools_embed.h','w').write('\n'.join(lines) + '\n')
EOF
```

## Terminology (kept aligned with issue #2)

- **history** — the session's accumulated turns; rebuilt into each
  request's prefix on resume
- **trace** — the on-disk `events.jsonl` evidence of every turn
  (`cat`/`jq` to inspect); there is deliberately no UI-level replay
