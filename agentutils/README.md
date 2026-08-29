# agentutils — busyagent applet

Resolve the cwd-bound session (default resumes the latest), rebuild its
history into the request prefix, stream the reply over HTTP(S) (SSE),
execute tool calls with busybox applets, feed results back until the
model stops, and append the turn's events to the session trace. With a
PROMPT argument that loop runs once and exits; a tty on stdin starts an
interactive REPL instead (sessions auto-resume per cwd).

## Files

| File | Role |
|---|---|
| `busyagent.c` | CLI (getopt32), session resolution, turn loop, background-bash registry, renderer/trace |
| `ba_impl.c` | everything else in one translation unit: JSON, utils, protocol, session store, HTTP(S) transport, SSE parsing, display, tools |
| `busyagent.h` | shared declarations (events, accumulator, store, transport API) |
| `ba_builtin_schemas.h` | the compiled-in builtin tool schemas |

Nothing is embedded from a `tools.json` at build time: the **only**
runtime source of dynamic tools is `$BA_HOME/tools.json`.

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
| execution | Bash | sh -c on busybox sh; background=true registers a capped background task (see below) |
| index/delegate | Glob (find -name), Grep (grep -nHr --include) | busybox applets |
| state | TodoWrite | checklist as tool_result; conversation history IS the state |
| state | PlanConfirm / PlanClear | plan.draft -> plan.md; current plan injected into system prompt |
| knowledge | Skill | SKILL.md loaded from cwd/skills > ~/.agents/skills > $BA_HOME/skills |
| delegate | SubAgent | in-process synchronous child session (`sub_` prefix, depth 1); `fork: true` copies the parent conversation first; nested SubAgent calls are rejected |

Dynamic zone ($BA_HOME/tools.json via `busyagent -i`) carries
world-operation primitives only (default sample: ls/head/tail/wc/stat).
Names may not shadow builtins (skipped with warning). Deleting the file
removes nothing essential: every dynamic entry is expressible through
Bash.

Each tool entry carries an `exec` mapping that never reaches the LLM
(stripped from the schemas sent to the model):

```json
"exec": { "applet": "grep", "argv": ["-nH", "-r", "-e", "$pattern", "$path"] }
```

At execution time `$var` placeholders expand from the model's input
JSON (missing optional keys drop their argument). NOFORK applets are
called in-process via `run_nofork_applet()` inside a forked child;
anything else execs busybox itself. Children are SIGKILLed on timeout.

Background bash (`background: true` in Bash input) does not just
detach: every task is registered (max 16), carries a hard 1-hour
deadline (killed by process group, reported as exit code 124) and its
log file is capped at 512 KiB via `RLIMIT_FSIZE`; finished tasks are
reaped each turn and their exit code plus output pushed back into the
conversation, and nothing outlives the applet process.

## Terminology (kept aligned with issue #2)

- **history** — the session's accumulated turns; rebuilt into each
  request's prefix on resume
- **trace** — the on-disk `events.jsonl` evidence of every turn
  (`cat`/`jq` to inspect); there is deliberately no UI-level replay
