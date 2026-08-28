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

    -u URL      LLM endpoint, e.g. http://gw.lan:8317/v1   ($BA_BASE_URL)
    -k KEY      API key                                   ($BA_API_KEY)
    -m MODEL    model name                                ($BA_MODEL)
    -p NAME     openai | claude | responses   (default openai, $BA_PROVIDER)
    -t N        max tool-use turns             (default 8)
    -e LVL      thinking effort: minimal|low|medium|high|xhigh|max (default off, $BA_EFFORT)
                (the level is passed through verbatim to the provider; each
                backend only supports a subset - e.g. OpenAI takes
                minimal..high, newer Anthropic/Gemini models accept more)
    -s ID       resume a specific session id
    -n          force a new session (CI / batch isolation)
    -c          explicit alias of the default continue-by-cwd behaviour
    -o FMT      text (default) | json (stream-json events on stdout)
    -v          verbose: request/response diagnostics on stderr
    -i [PATH]   write a starter tools.json (default $BA_HOME/tools.json) and exit

    With no PROMPT argument the prompt is read from stdin;
    if stdin is a tty an interactive REPL is started instead
    (busybox lineedit: history, UTF-8 input, sessions auto-resume).
```

Sessions are stored under `$BA_HOME` (default `$HOME/.busyagent`) and
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
  output back into the conversation and drives another model turn. Each
  task has a hard 1-hour timeout (killed by process group, reported as
  exit 124), its log file is capped at 512 KiB (`RLIMIT_FSIZE`), and no
  task outlives the applet process
- `SubAgent` — synchronous child session (its own history, `sub_` prefix):
  the call blocks until the child finishes and its final text becomes the
  tool result; there is no background/parallel execution. `fork: true`
  copies the parent conversation first; nested SubAgent calls are rejected
- read-only-root friendly: the session store is append-only jsonl

## Dynamic tool table

An optional `$BA_HOME/tools.json` extends the built-in tool set at
runtime:

```
busyagent -i            # export the starter table (builtins + one example)
$EDITOR $BA_HOME/tools.json
busyagent ...           # entries join the request, exec mappings stripped
```

Each entry carries the public schema (`name`, `description`, `input_schema`)
plus an internal `exec` mapping that tells the executor how to dispatch:

```json
{
  "name": "MyApplet",
  "description": "...",
  "input_schema": { "type": "object", "properties": { "pattern": { "type": "string" } } },
  "exec": { "applet": "grep", "argv": ["-rl", "-e", "$pattern", "."] }
}
```

`$name` placeholders in `argv` expand from the model's input at call time.
The exported starter file includes this mapping; malformed entries (missing
`exec.applet`, non-string `argv` items) are skipped with a note on stderr.
Only the public fields are sent to the model — `exec` never leaves the
process. Names shadowing a builtin (Bash/Read/.../SubAgent) are skipped —
builtins always win. A broken file falls back to builtins with a note on
stderr. Unknown tool names reach the executor and are reported back to the
model as errors.

## Transport security

- `https://` works out of the box via the in-tree TLS client
  (`networking/tls.c`). One limitation is inherited from that code base: the
  client does not verify certificate chains, hostnames, handshake signatures
  or record MACs, so a machine in the middle controlling the network can
  impersonate the endpoint. busyagent prints a one-time warning on stderr
  for every https session so the trade-off stays visible.
- Prefer a TLS-terminating gateway (e.g. `socat`/nginx in front of the
  public LLM API) on a trusted LAN, or accept the risk on networks you
  control.

## Known limitations

- The built-in TLS path does not authenticate the server (see above); it
  encrypts the transport but cannot confirm who is on the other end.
- One-shot by design: no cross-process daemon, no interactive readline
  thread — the REPL is a simple lineedit loop around the same session store.
