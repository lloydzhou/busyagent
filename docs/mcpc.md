# mcpc — an MCP client applet for busybox

`mcpc` connects to Model Context Protocol servers and drives their
tools from the shell. Sessions are `@name` addressed (the upstream mcpc
muscle memory) and live inside a small background daemon that holds the
Streamable-HTTP connections (with their `Mcp-Session-Id`) and the stdio
child processes across invocations — MCP is a stateful protocol, so the
daemon is the only execution path: when the socket is dead the CLI
forks it (detached, stdio to /dev/null, pid file under
`$BA_HOME/mcpc/`), and a failure to start it is a hard error.

Session metadata (url, headers, serverInfo, tools) is mirrored to
`$BA_HOME/mcpc/servers/<name>.json`, so `tools-get` and `grep` work
offline and survive daemon restarts.

Enable `CONFIG_MCPC` (selects the shared `AGENTUTILS_COMMON`
infrastructure). The in-tree TLS client does not verify certificates —
same trade-off and warning as `busyagent`.

## Usage

```
mcpc connect URL [@NAME]              http(s) Streamable HTTP server
mcpc connect cmd:COMMAND [@NAME]      spawn a stdio JSON-RPC server
mcpc ls                               list sessions (name, transport, status)
mcpc close @NAME                      close a session (cache survives)
mcpc daemon stop                      stop the daemon
mcpc grep PATTERN                     search cached tools locally

mcpc @NAME tools-list [--full]        list tools (from the local cache)
mcpc @NAME tools-get TOOL             show one tool's input schema
mcpc @NAME tools-call TOOL [K=V|K:=JSON ...]   call a tool
mcpc @NAME ping                       protocol ping

global: --json (raw JSON output)  --insecure (accepted, TLS is never verified)
```

Example:

```
$ mcpc connect https://api.example.com/mcp @zs
connected @zs

$ mcpc @zs tools-list
web_search_prime        search the web
web_reader              read a url as markdown

$ mcpc @zs tools-call web_search_prime query=busybox count:=5
{"content":[{"type":"text","text":"..."}]}

$ mcpc grep reader
zs         web_reader
```

## Argument typing on tools-call

- `key=value` passes the value as a JSON string.
- `key:=value` passes it as a raw JSON value (`n:=5` → `5`,
  `flag:=true` → `true`, `s:='{"a":1}'` → an object); a value that does
  not parse falls back to a string.

## The daemon

- socket `$BA_HOME/mcpc/daemon.sock` (line-delimited JSON protocol),
  pid file `daemon.pid`; a stale pid file is detected and replaced.
- the daemon is spawned implicitly by any session command, runs
  detached (`setsid`, stdio on /dev/null so it never holds a caller's
  pipeline open), and dies cleanly on `mcpc daemon stop` (or SIGTERM),
  killing stdio children and removing its socket.
- on a dropped connection mcpc reports an error and exits — it never
  replays a request, so a `create_file`-style tool cannot fire twice.
- stdio servers are one `sh -c` child per session with line-framed
  JSON-RPC; responses are matched by `id`, notifications are dropped.

## Layout

```
$BA_HOME/mcpc/daemon.sock     daemon RPC socket
$BA_HOME/mcpc/daemon.pid      daemon pid (stale-checked)
$BA_HOME/mcpc/servers/*.json  {"transport":..., "source":..., "server_info":{}, "tools":[...]}
```

## See also

- `docs/oapi.md` — the OpenAPI CLI applet (stateless registry)
- `agentutils/agent_common.h` — the shared HTTP/SSE infrastructure
