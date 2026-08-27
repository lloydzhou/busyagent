# busyagent — an LLM agent applet for busybox

`busyagent` runs an LLM agent loop against the surrounding busybox applets:
it streams the reply over plain HTTP (SSE), executes the tool calls the model
asks for, feeds the results back, and repeats until the model finishes. Each
invocation resumes the session bound to the current working directory, so
conversations continue across runs; without a PROMPT argument (or with a tty
on stdin) it offers an interactive REPL instead.

The whole thing is a normal busybox applet: enable `CONFIG_BUSYAGENT` and the
same busybox binary that provides your shell and coreutils also provides the
agent. No runtime files beyond the session store.

## Usage

```
busyagent [-vnu] [-k KEY] [-m MODEL] [-p PROVIDER] [-t TURNS] [-s ID] [-o FMT] [PROMPT]

    -u URL      LLM endpoint, e.g. http://gw.lan:8317/v1   ($BB_AGENT_BASE_URL)
    -k KEY      API key                                   ($BB_AGENT_API_KEY)
    -m MODEL    model name                                ($BB_AGENT_MODEL)
    -p NAME     openai | claude | responses   (default openai, $BB_AGENT_PROVIDER)
    -t N        max tool-use turns             (default 8)
    -s ID       resume a specific session id
    -n          force a new session (CI / batch isolation)
    -c          explicit alias of the default continue-by-cwd behaviour
    -o FMT      text (default) | json (stream-json events on stdout)
    -v          verbose: request/response diagnostics on stderr
    -i          print the resolved system prompt and exit

    With no PROMPT argument the prompt is read from stdin;
    if stdin is a tty an interactive REPL is started instead
    (busybox lineedit: history, UTF-8 input, sessions auto-resume).
```

Sessions are stored under `$BB_AGENT_HOME` (default `$HOME/.busyagent`) and
bound to the working directory: re-running in the same cwd resumes the latest
conversation. Each session directory holds `conversation.jsonl` (model
context), `events.jsonl` (full trace — every turn, tool call and result,
inspectable with `jq`) and `stats.json`.

## Tools

The model may call any of the applets exposed in the bundled tool set
(`sh`, `grep`, `sed`, `awk`, `find`, `cat`, `ls`, `od`, `ps`, ...). Features:

- multi-turn loop capped by `-t` (default 8) with per-turn history trimming
- `Bash` with `background: true` — the tool returns `task_id` immediately;
  when the job exits the agent pushes `[bg-bash <id>] exit_code=N` plus the
  output back into the conversation and drives another model turn
- `SubAgent` — a child session (its own history, `sub_` prefix); `fork: true`
  copies the parent conversation first; nested SubAgent calls are rejected
- read-only-root friendly: the session store is append-only jsonl

## Known limitations

- HTTP only for now; TLS via busybox's built-in `tls.c` is planned. Point
  `-u` at an internal gateway that terminates TLS for public LLM APIs.
- One-shot by design: no cross-process daemon, no interactive readline
  thread — the REPL is a simple lineedit loop around the same session store.
