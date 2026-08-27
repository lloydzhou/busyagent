# busyagent — an AI agent inside busybox

> A single 1.3MB static binary: **409 busybox applets + an LLM agent that can
> actually use them to solve problems**. No libc dependency, `FROM scratch`,
> `linux/amd64` and `linux/arm64`.

## Quick start

Interactive REPL (busybox lineedit: UTF-8 input, history, sessions auto-resume
per working directory):

```bash
docker run --rm --network host \
  -v busyagent-data:/root/.busyagent \
  -e BB_AGENT_BASE_URL=http://host:8317/v1 \
  -e BB_AGENT_API_KEY=sk-xxx \
  -e BB_AGENT_MODEL=your-model \
  -it lloydzhou/busyagent
```

Or one-shot question straight to the answer:

```bash
docker run --rm --network host \
  -e BB_AGENT_BASE_URL=... -e BB_AGENT_API_KEY=... -e BB_AGENT_MODEL=... \
  lloydzhou/busyagent busyagent "why did this build fail"
```

The agent plans, calls busybox applets (`sh`, `grep`, `sed`, `awk`, `find`,
`od`, `ps`, ...), feeds results back and iterates until done. Background tasks
(`background: true`) push their exit code and output back into the session
when they finish. Sessions and the full event trace live in
`/root/.busyagent` — mount it as a volume to keep them across containers.

## Environment variables

| Variable | Description |
|---|---|
| `BB_AGENT_BASE_URL` | LLM API endpoint (OpenAI-compatible `/v1`; claude / responses protocols also supported) |
| `BB_AGENT_API_KEY`  | API key |
| `BB_AGENT_MODEL`    | Model name |
| `BB_AGENT_HOME`     | Session/history storage dir (defaults to `/root/.busyagent` in this image) |
| `BB_AGENT_OUTPUT`   | `text` (default) / `json` (stream-json for programmatic use) |

## Build

```bash
docker buildx build --platform linux/amd64,linux/arm64 \
  -t lloydzhou/busyagent:latest -t lloydzhou/busybox:latest --push .
```

## Limitations

- HTTP only for now (TLS via busybox's built-in `tls.c` is planned); put an
  internal gateway in front of public LLM APIs and point the device at it
- Requires an OpenAI-compatible endpoint
