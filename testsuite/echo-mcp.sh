#!/bin/sh
# Minimal stdio MCP (JSON-RPC 2.0) server for the mcpc testsuite.
# Kept as a standalone file for manual testing; the .tests file embeds
# its own copy under a .tmp name.
#
# line JSON parsing is deliberately grep-based: this only needs to
# survive well-formed requests from mcpc itself.
while IFS= read -r line; do
	method=$(printf '%s' "$line" | grep -o '"method":"[^"]*"' | head -1 | cut -d'"' -f4)
	id=$(printf '%s' "$line" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
	case "$method" in
	initialize)
		printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-03-26","capabilities":{"tools":{}},"serverInfo":{"name":"echo-mcp","version":"1.0"}}}\n' "$id"
		;;
	notifications/*)
		:   # notifications get no response
		;;
	tools/list)
		printf '{"jsonrpc":"2.0","id":%s,"result":{"tools":[{"name":"echo","description":"echo back its arguments","inputSchema":{"type":"object","properties":{"msg":{"type":"string"}}}},{"name":"add","description":"add two numbers","inputSchema":{"type":"object"}}]}}\n' "$id"
		;;
	tools/call)
		printf '{"jsonrpc":"2.0","id":%s,"result":{"content":[{"type":"text","text":"echo-called"}],"isError":false}}\n' "$id"
		;;
	ping)
		printf '{"jsonrpc":"2.0","id":%s,"result":{}}\n' "$id"
		;;
	*)
		printf '{"jsonrpc":"2.0","id":%s,"error":{"code":-32601,"message":"unknown method %s"}}\n' "$id" "$method"
		;;
	esac
done
