#!/usr/bin/env python3
"""Stub LLM server (SSE, openai chat/completions wire) for deterministic
e2e tests. Scriptable via /tmp/stub_script.json: list of steps; each step
{"when_substrate": "<regex on last user message>", "tool": {...}|None,
 "text": "..."}. Simplest mode: positional replies.

Here we hardcode the async-bash scenario:
  turn1 -> tool_call Bash{background:true, command:'sleep 1; echo async-marker-31337'}
  any later turn whose last user msg contains '[bg-bash' + marker
        -> text 'RESULT-CAPTURED: <marker present?> exit_code=<n>'
"""
import http.server, json, re, sys

def sse_respond(handler, payload):
    body_chunks = [payload]
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Content-Length", str(sum(len(c) for c in body_chunks)))
    handler.end_headers()
    for c in body_chunks:
        handler.wfile.write(c.encode())

def chunk(delta=None, finish=None, tool_calls=None, usage=None):
    d = {}
    if delta is not None: d["content"] = delta
    if tool_calls is not None:
        d["tool_calls"] = tool_calls
        d.pop("content", None)
    msg = {"role": "assistant", **({"delta": d})}
    obj = {"id":"stub","object":"chat.completion.chunk","model":"stub",
           "choices":[{"index":0,"delta":d,"finish_reason":finish}]}
    out = f"data: {json.dumps(obj)}\n\n"
    if usage is not None:
        uobj = {"id":"stub","object":"chat.completion.chunk","model":"stub",
                "choices":[],"usage":{"prompt_tokens":10,"completion_tokens":5}}
        out += f"data: {json.dumps(uobj)}\n\n"
    out += "data: [DONE]\n\n"
    return out

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n))
        msgs = req["messages"]
        # find last user text and whether bg result arrived
        last_user = ""
        saw_bg_result = False
        for m in msgs:
            if m["role"] == "user":
                if isinstance(m["content"], str): last_user = m["content"]
                elif isinstance(m["content"], list):
                    for b in m["content"]:
                        if b.get("type") == "text": last_user = b.get("text","")
            if m["role"] == "user" and isinstance(m["content"], str) \
               and "[bg-bash" in m["content"]:
                saw_bg_result = True

        if not any(m["role"]=="assistant" for m in msgs):
            # first turn: spawn the background task
            tc = [{"index":0,"id":"call_stub_bg_1","type":"function",
                   "function":{"name":"Bash",
                       "arguments":json.dumps({
                           "command":"sleep 1; echo async-marker-31337",
                           "background": True})}}]
            payload = chunk(tool_calls=tc, finish="tool_calls")
        elif not saw_bg_result:
            payload = chunk(delta="STARTED", finish="stop")
        else:
            got = ("async-marker-31337" in json.dumps(msgs))
            payload = chunk(delta=f"CHILD-REPORT present={got}",
                            finish="stop")
        sse_respond(self, payload)

    def log_message(self, *a):
        pass

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18997
    http.server.HTTPServer(("0.0.0.0", port), H).serve_forever()
