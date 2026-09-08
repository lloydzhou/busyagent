# jq — a tiny jq applet for busybox

`jq` extracts values from JSON with a jq-style path expression. It is
deliberately the path subset of jq — the pieces a shell pipeline
actually needs — sharing one evaluator (`agc_json_path` in
`agent_common.c`) with `oapi --jq`.

```
$ echo '{"items":[{"id":1},{"id":2}]}' | jq '.items[].id'
1
2

$ curl -s ... | jq -r '.name'
fido
```

## Usage

```
jq [-re] PATH [FILE|-]

    -r    print string values raw (no quotes)
    -e    exit 1 when nothing was printed
```

Reads stdin when FILE is absent or `-`.

## Path language

| expression        | meaning                                          |
|-------------------|--------------------------------------------------|
| `.a.b` / `a.b`    | object fields (leading dot optional)             |
| `.[0]` / `.a[2]`  | array index                                      |
| `.[]` / `.a[]`    | array spread: one match per element               |
| `.a[].b`          | spread then field                                 |
| `.`               | the root itself                                   |

Semantics match jq for the subset: a missing key or an out-of-range
index yields `null`; `[]` over a non-array (including null) yields
nothing; every match prints on its own line, strings quoted unless
`-r`. At most 64 matches and 16 segments per path (enough for CLI
use; errors say so otherwise).

Not implemented (by design): pipes, `select()`, construction,
arithmetic, regex, multi-document streams.

## See also

- `docs/oapi.md` — `--jq` uses the same evaluator
- `agentutils/agent_common.h` — `agc_json_path()` API
