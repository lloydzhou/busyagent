# oapi — an OpenAPI CLI applet for busybox

`oapi` turns an OpenAPI (swagger) JSON document into a set of shell
commands: `oapi connect NAME SPEC` caches the document under
`$BA_HOME/oapi/apis/NAME.json`, and afterwards every `operationId` is
callable as `oapi NAME OPERATION` with its path parameters positional
and its query/header parameters as `--kebab-case` flags. stdout carries
data only, diagnostics go to stderr, and a non-2xx response exits 1 —
the same contract as the python `oapi` CLI this applet mirrors.

Enable `CONFIG_OAPI` (selects the shared `AGENTUTILS_COMMON`
infrastructure). JSON specs only in this MVP (YAML is a follow-up);
`--header` covers manual auth until securitySchemes land.

## Usage

```
oapi connect NAME SPEC-URL-OR-FILE   cache an API document as NAME
oapi sync NAME                        re-fetch from the stored source
oapi ls                               list registered APIs
oapi rm NAME                          remove NAME from the registry
oapi schema NAME [OPERATION]          list operations / show parameters
oapi api NAME METHOD PATH             raw request escape hatch

oapi NAME OPERATION [PARAM]... [--FLAG V]...
```

Example (petstore):

```
$ oapi connect petstore http://127.0.0.1:8080/petstore.json
connected petstore: 3 operations (source http://127.0.0.1:8080/petstore.json)

$ oapi petstore get-pet-by-id 7
{"id":7,"name":"lassie"}

$ oapi petstore find-pets --status available --jq '[].name'
fido
rex

$ oapi petstore create-pet -F name=milou -F vaccinated=true
{"name":"milou","vaccinated":true}
```

## Operation addressing

- command name = `operationId` in kebab-case (`getPetById` →
  `get-pet-by-id`); the raw `operationId` also matches.
- required path parameters are positional, filled in template order
  (`/pets/{id}` → `oapi petstore get-pet-by-id 7`).
- query and header parameters become `--kebab-case` flags
  (`--status available`); header-typed flags are sent as request
  headers, everything else goes to the query string.
- `$ref` parameters (`#/components/...`) are folded in via a JSON
  Pointer walk; array-typed query parameters are comma-joined.

## Request body

- `--body X` sends X verbatim: inline JSON, `@file` or `-` (stdin).
- `-F key=value` (repeatable) builds a JSON object; values are parsed
  as JSON when they look like it (`true`, `42`, `[1,2]`), quoted as
  strings otherwise. `Content-Type: application/json` is set
  automatically.

## Output

- 2xx: the raw response body on stdout (plus a newline when missing).
- `--jq PATH` filters JSON responses: `a.b[0].c`, `items[].id`.
- `-o text` prints scalars as-is and array elements one per line.
- non-2xx: `oapi: HTTP <code>: <body>` on stderr, exit 1.
- `--dry-run` prints `METHOD url`, headers and body instead of sending.
- `--header 'Name: value'` (repeatable) adds custom headers — the hook
  for API keys until securityScheme support lands.

## Server URL resolution

`servers[0].url` wins when absolute. A relative server url (`/api`) is
resolved against the fetch source recorded by `connect`, so a spec
served from `http://gw:8080/spec.json` with `"url": "/v2"` produces
`http://gw:8080/v2/...`.

## Layout

```
$BA_HOME/oapi/apis/<name>.json     {"source":..., "fetched_at":..., "spec":{...}}
```

The registry is plain stateless JSON: copy it between machines, edit
it by hand, delete it to forget an API.

## See also

- `docs/mcpc.md` — the MCP client applet (daemon-held @sessions)
- `agentutils/agent_common.h` — the shared HTTP/SSE infrastructure
