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

## tools.json lifecycle (dynamic zone only — nothing embedded)

`agentutils/tools.json` no longer exists in this repo and nothing is
embedded in the binary. The **only** runtime source is
`$BA_HOME/tools.json`:

- `busyagent -i [PATH]` exports a starter table (ls/head/tail/wc/stat),
  creating parent dirs, refusing to overwrite;
- dynamic entries are appended to the 11 builtins (builtin names may
  not be shadowed; skipped with a warning);
- a missing or broken file just means "dynamic zone empty" — the 11
  builtins keep the agent fully capable.

## Tools: 11 builtins + a dynamic sugar zone

Builtin tools are compiled in and cannot be shadowed by tools.json.
The boundary is deliberate: a builtin is either a meta-operation on the
agent's own state space, the execution anchor, or a content transfer
that needs C logic. Names deliberately match mainstream-agent training
vocabulary (Read/Write/Edit/Bash/Glob/Grep/TodoWrite/PlanConfirm/
PlanClear/Skill/SubAgent); the Bash description names the busybox sh
base honestly.

| Group | Builtins | Mechanism |
|---|---|---|
| content | Read (offset/limit, cat -n), Write, Edit (exact-once) | pure C |
| execution | Bash | sh -c on busybox sh; background=true detaches to a task log |
| index/delegate | Glob (find -name), Grep (grep -nHr --include) | busybox applets |
| state | TodoWrite | checklist as tool_result; conversation history IS the state |
| state | PlanConfirm / PlanClear | plan.draft -> plan.md; current plan injected into system prompt |
| knowledge | Skill | SKILL.md loaded from cwd/skills > ~/.agents/skills > $BA_HOME/skills |
| delegate | SubAgent | self-bootstrapping fork+exec `busyagent --new`, env-passed config |

Dynamic zone ($BA_HOME/tools.json via `busyagent -i`) carries
world-operation primitives only (default sample: ls/head/tail/wc/stat).
Names may not shadow builtins (skipped with warning). Deleting the file
removes nothing essential: every dynamic entry is expressible through
Bash.

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
