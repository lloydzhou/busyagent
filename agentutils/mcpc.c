/* mcpc - Model Context Protocol client: connect to MCP servers and
 * drive their tools from the shell.
 *
 * Sessions are @name addressed (upstream mcpc muscle memory) and live
 * inside a small daemon: the CLI is a thin client that talks to
 * $BA_HOME/mcpc/daemon.sock (line-delimited JSON) and the daemon holds
 * the Streamable-HTTP connections (Mcp-Session-Id) and the stdio child
 * processes across invocations. The daemon is the only execution path;
 * when the socket is dead the CLI forks it, and a failure to start it
 * is a hard error.
 *
 * Session metadata (url, headers, serverInfo, tools) is mirrored to
 * $BA_HOME/mcpc/servers/<name>.json so tools-get/grep work offline.
 */
//config:config MCPC
//config:	bool "mcpc (28 kb)"
//config:	default y
//config:	select AGENTUTILS_COMMON
//config:	help
//config:	  MCP client with daemon-held sessions: 'mcpc connect URL [@name]'
//config:	  establishes a session, then 'mcpc @name tools-list' and
//config:	  'mcpc @name tools-call TOOL k:=v' drive the server. stdio
//config:	  servers: 'mcpc connect cmd:COMMAND [@name]'.

//applet:IF_MCPC(APPLET(mcpc, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_MCPC) += mcpc.o

//usage:#define mcpc_trivial_usage
//usage:       "connect URL|cmd:CMD [@SESSION] [-H HDR] [--protocol-version V]\n"
//usage:       "	| ls | close @SESSION | daemon stop\n"
//usage:       "	| grep PATTERN | @SESSION COMMAND [ARGS]"
//usage:#define mcpc_full_usage "\n\n"
//usage:       "MCP client; sessions live in a background daemon\n"
//usage:       "\n"
//usage:       "connect URL [@NAME]	Connect (http(s) Streamable HTTP)\n"
//usage:       "connect cmd:CMD [@NAME]	Spawn a stdio JSON-RPC server\n"
//usage:       "ls			List sessions (name, server, status)\n"
//usage:       "close @NAME		Close a session (keeps the cache)\n"
//usage:       "daemon stop		Stop the daemon\n"
//usage:       "grep PATTERN		Search cached tools locally\n"
//usage:       "\n"
//usage:       "@NAME tools-list [--full]	List the server's tools\n"
//usage:       "@NAME tools-get TOOL		Show one tool's input schema\n"
//usage:       "@NAME tools-call TOOL [K=V|K:=JSON ...]	Call a tool\n"
//usage:       "@NAME ping			Protocol ping\n"
//usage:       "\n"
//usage:       "	--json		Raw JSON output for scripting\n"
//usage:       "	--insecure	Accepted for compatibility (TLS is never verified)"

#include "busyagent.h"
#include "agent_common.h"
#include "libbb.h"
#include "busybox.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/file.h>

#define MCPC_MAX_HEADERS 8
#define MCPC_MAX_SESSIONS 16
#define MCPC_STDIO_TIMEOUT_MS 30000
#define MCPC_CLIENT_TIMEOUT_MS 30000
#define MCPC_MAX_LINE (16 * 1024)
#define MCPC_DAEMON_START_MS 5000
#define MCPC_LOCK_MODE 0600

/* ============================================================
 * paths
 * ============================================================ */

static char *mcpc_dir(void)
{
	const char *h = getenv("BA_HOME");
	if (h && h[0])
		return xasprintf("%s/mcpc", h);
	h = getenv("HOME");
	if (h && h[0])
		return xasprintf("%s/.busyagent/mcpc", h);
	return xstrdup("/tmp/busyagent/mcpc");
}

static char *mcpc_sock_path(void)
{
	char *d = mcpc_dir();
	char *p = xasprintf("%s/daemon.sock", d);
	free(d);
	return p;
}

static char *mcpc_pid_path(void)
{
	char *d = mcpc_dir();
	char *p = xasprintf("%s/daemon.pid", d);
	free(d);
	return p;
}

static char *mcpc_lock_path(void)
{
	char *d = mcpc_dir();
	char *p = xasprintf("%s/daemon.lock", d);
	free(d);
	return p;
}

static char *mcpc_server_path(const char *name)
{
	char *d = mcpc_dir();
	char *p;

	if (!name[0] || strchr(name, '/') || strcmp(name, ".") == 0
	 || strcmp(name, "..") == 0)
		return NULL;
	p = xasprintf("%s/servers/%s.json", d, name);
	free(d);
	return p;
}

/* strip a leading '@' */
static const char *mcpc_bare(const char *name)
{
	return (name && name[0] == '@') ? name + 1 : name;
}

/* ============================================================
 * session cache (servers/<name>.json) - CLI side
 *
 * {"transport":"http|stdio","source":"url-or-cmd","server_info":{},
 *  "tools":[...], "session_held":true}
 * ============================================================ */

static void mcpc_cache_save(const char *name, const char *json)
{
	char *p = mcpc_server_path(name);
	char *dir = mcpc_dir();
	char *sd;
	FILE *f;

	if (!p) {
		free(dir);
		return;
	}
	sd = xasprintf("%s/servers", dir);
	bb_make_directory(sd, 0755, FILEUTILS_RECUR);
	free(sd);
	f = xfopen(p, "w");
	fputs(json, f);
	fputc('\n', f);
	fclose(f);
	free(p);
	free(dir);
}

static char *mcpc_cache_load(const char *name)
{
	char *p = mcpc_server_path(name);
	char *data;
	FILE *f;
	long sz;

	if (!p)
		return NULL;
	f = fopen(p, "r");
	free(p);
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	data = xmalloc(sz + 1);
	{
		size_t got = fread(data, 1, sz, f);
		fclose(f);
		data[got] = '\0';
	}
	return data;
}

/* ============================================================
 * daemon side
 * ============================================================ */

typedef struct {
	char *name;
	int transport;         /* 0=http, 1=stdio */
	char url[1024];
	char *cmd;             /* stdio command line */
	char *headers[MCPC_MAX_HEADERS];
	int n_headers;
	char *session_id;      /* Mcp-Session-Id (http) */
	char *protocol_version; /* initialize handshake version */
	int next_id;
	pid_t pid;             /* stdio child */
	int in_fd;             /* write end (child stdin) / http fd unused */
	int out_fd;            /* read end (child stdout) */
	char *server_info_json;
	char *tools_json;
	int dead;
} McpSession;

static McpSession *g_sessions[MCPC_MAX_SESSIONS];
static int g_n_sessions;
static volatile smallint g_stop;
static void (*g_old_sigpipe)(int);
static int g_sigpipe_saved;

static void mcpc_close_fds_from(int first);

static McpSession *mcpc_sess_find(const char *name)
{
	int i;

	for (i = 0; i < g_n_sessions; i++)
		if (g_sessions[i] && strcmp(g_sessions[i]->name, name) == 0
		 && !g_sessions[i]->dead)
			return g_sessions[i];
	return NULL;
}

static void mcpc_sess_free(McpSession *s)
{
	int i;

	/* Close the pipes first so a child blocked on stdio can exit; only then
	 * signal its process group.  The pid is still owned by this daemon. */
	if (s->in_fd >= 0) {
		close(s->in_fd);
		s->in_fd = -1;
	}
	if (s->out_fd >= 0) {
		close(s->out_fd);
		s->out_fd = -1;
	}
	if (s->transport == 1 && s->pid > 0) {
		if (kill(-s->pid, SIGTERM) != 0 && errno != ESRCH)
			kill(s->pid, SIGTERM);
		for (i = 0; i < 20; i++) {
			if (waitpid(s->pid, NULL, WNOHANG) == s->pid)
				break;
			usleep(50 * 1000);
		}
		if (waitpid(s->pid, NULL, WNOHANG) != s->pid) {
			if (kill(-s->pid, SIGKILL) != 0 && errno != ESRCH)
				kill(s->pid, SIGKILL);
		}
		waitpid(s->pid, NULL, 0);
	}
	free(s->name);
	free(s->cmd);
	free(s->session_id);
	free(s->protocol_version);
	free(s->server_info_json);
	free(s->tools_json);
	free(s);
}

/* ---- stdio transport ---- */

static int mcpc_stdio_spawn(McpSession *s, const char *cmd)
{
	int in_pipe[2];
	int out_pipe[2];
	pid_t pid;

	if (pipe(in_pipe) != 0)
		return -1;
	if (pipe(out_pipe) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(in_pipe[0]); close(in_pipe[1]);
		close(out_pipe[0]); close(out_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		signal(SIGPIPE, SIG_DFL);
		close(in_pipe[1]);
		close(out_pipe[0]);
		if (setsid() < 0)
			_exit(127);
		xmove_fd(in_pipe[0], STDIN_FILENO);
		xmove_fd(out_pipe[1], STDOUT_FILENO);
		mcpc_close_fds_from(3);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	s->pid = pid;
	s->in_fd = in_pipe[1];
	s->out_fd = out_pipe[0];
	return 0;
}

/* read one \n-terminated line from fd with a timeout; malloc'd or NULL */
static char *mcpc_read_line_to(int fd, int timeout_ms)
{
	StrBuf sb;
	int64_t deadline = (int64_t)monotonic_ms() + timeout_ms;

	sb_init(&sb);
	for (;;) {
		char ch;
		struct pollfd pfd;
		int pr;
		ssize_t n;

		pfd.fd = fd;
		pfd.events = POLLIN;
		pr = safe_poll(&pfd, 1, 250);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		if (pr == 0) {
			if ((int64_t)monotonic_ms() >= deadline)
				goto fail;
			if (g_stop)
				goto fail;
			continue;
		}
		if (!(pfd.revents & (POLLIN | POLLHUP)))
			goto fail;
		n = safe_read(fd, &ch, 1);
		if (n <= 0)
			goto fail;
		if (ch == '\n')
			break;
		if (ch != '\r') {
			if (sb.len >= MCPC_MAX_LINE)
				goto fail;
			sb_append_char(&sb, ch);
		}
	}
	if (!sb.data)
		sb_append(&sb, "");
	return sb.data;
 fail:
	sb_free(&sb);
	return NULL;
}

/* write the request, then read lines until the one carrying our id */
static char *mcpc_stdio_roundtrip(McpSession *s, const char *req, int want_id)
{
	char *resp = NULL;

	if (full_write(s->in_fd, req, strlen(req)) != (ssize_t)strlen(req)
	 || full_write(s->in_fd, "\n", 1) != 1) {
		s->dead = 1;
		return NULL;
	}
	for (;;) {
		JsonParse jp;
		char *line = mcpc_read_line_to(s->out_fd, MCPC_STDIO_TIMEOUT_MS);
		if (!line) {
			s->dead = 1;
			return NULL;
		}
		jp = json_parse_root(line);
		if (jp.error) {
			free(line);
			continue;   /* garbage line: skip */
		}
		{
			JsonVal idv = json_get(jp.val, "id");
			char *raw;
			int match = 0;
			char *version = json_get_string(jp.val, "jsonrpc");

			if (!version || strcmp(version, "2.0") != 0) {
				free(version);
				free(line);
				continue;
			}
			free(version);
			if (idv.type == JSON_NUMBER && (int)json_number_val(idv) == want_id)
				match = 1;
			else {
				raw = json_as_string(idv);
				if (raw) {
					char idstr[32];
					snprintf(idstr, sizeof(idstr), "%d", want_id);
					match = (strcmp(raw, idstr) == 0);
					free(raw);
				}
			}
			if (match) {
				resp = line;
				return resp;
			}
		}
		free(line);   /* notification (progress, logging...): drop */
	}
}

/* ---- HTTP (Streamable HTTP) transport ---- */

/* SSE-wrapped JSON-RPC: retain only the first matching message event. */
typedef struct {
	AgcSse sse;
	char *json;
	StrBuf body;
	AgcHttpResp *resp;
	int want_id;
} McpHttpSseCtx;

static int mcpc_json_id_matches(JsonVal root, int want_id)
{
	JsonVal idv;
	char *raw;
	char idstr[32];

	if (root.type != JSON_OBJECT)
		return 0;
	{
		char *version = json_get_string(root, "jsonrpc");
		int ok = version && strcmp(version, "2.0") == 0;
		free(version);
		if (!ok)
			return 0;
	}
	idv = json_get(root, "id");
	snprintf(idstr, sizeof(idstr), "%d", want_id);
	if (idv.type == JSON_NUMBER) {
		char *endp;
		raw = json_as_string(idv);
		if (!raw)
			return 0;
		endp = raw;
		if (*endp == '-')
			endp++;
		if (!*endp)
			return 0;
		while (*endp >= '0' && *endp <= '9')
			endp++;
		if (*endp) {
			free(raw);
			return 0;
		}
		if (strcmp(raw, idstr) != 0) {
			free(raw);
			return 0;
		}
		free(raw);
		return 1;
	}
	if (idv.type == JSON_STRING) {
		raw = json_string_val(idv);
		if (!raw)
			return 0;
		if (strcmp(raw, idstr) != 0) {
			free(raw);
			return 0;
		}
		free(raw);
		return 1;
	}
	return 0;
}

static void mcpc_sse_event_match(void *ctx, const char *event,
				 const char *data, size_t data_len)
{
	McpHttpSseCtx *c = ctx;
	JsonParse jp;

	if (c->json || strcmp(event, "message") != 0)
		return;
	c->json = xstrndup(data, data_len);
	jp = json_parse_root(c->json);
	if (jp.error || !mcpc_json_id_matches(jp.val, c->want_id)) {
		free(c->json);
		c->json = NULL;
		return;
	}
}

static int mcpc_http_chunk(void *ctx, const char *data, size_t len)
{
	McpHttpSseCtx *c = ctx;

	if (c->json)
		return 1;
	if (c->want_id < 0 || !c->resp->content_type
		|| strncasecmp(c->resp->content_type, "text/event-stream", 17) != 0) {
		if (c->body.len > BA_MAX_BODY || len > BA_MAX_BODY - c->body.len)
			return -1;
		sb_appendn(&c->body, data, len);
		return 0;
	}
	agc_sse_feed(&c->sse, data, len, mcpc_sse_event_match, c);
	if (c->sse.error)
		return -1;
	return c->json ? 1 : 0;
}

static char *mcpc_http_roundtrip(McpSession *s, const char *req, int want_id)
{
	AgcHttpResp resp;
	const char *hdrs[MCPC_MAX_HEADERS + 3];
	McpHttpSseCtx ctx;
	int nh = 0;
	int i;
	int sid_idx = -1;
	AgcHttpReq hreq;
	int rc;

	for (i = 0; i < s->n_headers; i++)
		hdrs[nh++] = s->headers[i];
	hdrs[nh++] = "Content-Type: application/json";
	hdrs[nh++] = "Accept: application/json, text/event-stream";
	if (s->session_id) {
		hdrs[nh++] = xasprintf("Mcp-Session-Id: %s", s->session_id);
		sid_idx = nh - 1;
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.want_id = want_id;
	ctx.resp = &resp;
	sb_init(&ctx.body);
	memset(&hreq, 0, sizeof(hreq));
	hreq.method = "POST";
	hreq.url = s->url;
	hreq.headers = hdrs;
	hreq.header_count = nh;
	hreq.body = req ? req : "";
	hreq.body_len = strlen(req ? req : "");
	hreq.timeout_ms = MCPC_STDIO_TIMEOUT_MS;
	rc = agc_http_request_stream(&hreq, mcpc_http_chunk, &ctx, &resp);
	if (sid_idx >= 0)
		free((void *)hdrs[sid_idx]);
	if (rc < 0) {
		s->dead = 1;
		agc_sse_free(&ctx.sse);
		free(ctx.json);
		sb_free(&ctx.body);
		return NULL;
	}
	if (resp.status < 200 || resp.status >= 300) {
		bb_error_msg("HTTP %d", resp.status);
		agc_http_resp_free(&resp);
		agc_sse_free(&ctx.sse);
		free(ctx.json);
		sb_free(&ctx.body);
		return NULL;
	}
	if (resp.session_id && !s->session_id)
		s->session_id = xstrdup(resp.session_id);
	if (resp.content_type
		&& strncasecmp(resp.content_type, "text/event-stream", 17) == 0) {
		char *out;
		/* The stream may end immediately after the final event delimiter. */
		agc_sse_finish(&ctx.sse, mcpc_sse_event_match, &ctx);
		if (ctx.sse.error)
			ctx.json = NULL;
		if (!ctx.json) {
			agc_sse_free(&ctx.sse);
			agc_http_resp_free(&resp);
			bb_error_msg("SSE stream carried no matching message event");
			return NULL;
		}
		out = ctx.json;
		ctx.json = NULL;
		agc_sse_free(&ctx.sse);
		agc_http_resp_free(&resp);
		sb_free(&ctx.body);
		return out;
	}
	/* The stream callback owns body collection for ordinary HTTP replies. */
	if (ctx.body.data) {
		resp.body = ctx.body.data;
		resp.body_len = ctx.body.len;
		ctx.body.data = NULL;
		ctx.body.len = 0;
		ctx.body.cap = 0;
	}
	/* A notification may legally have no body. */
	if (want_id < 0 && (resp.status == 202 || !resp.body || !resp.body[0])) {
		agc_http_resp_free(&resp);
		sb_free(&ctx.body);
		return xstrdup("");
	}
	/* An RPC request must not manufacture a successful id=0 response from
	 * HTTP 202/empty output. */
	if (resp.status == 202 || !resp.body || !resp.body[0]) {
		agc_http_resp_free(&resp);
		sb_free(&ctx.body);
		bb_error_msg("HTTP response has no JSON-RPC result");
		return NULL;
	}
	{
		char *out = xstrdup(resp.body);
		JsonParse jp = json_parse_root(out);
		if (jp.error || !mcpc_json_id_matches(jp.val, want_id)) {
			free(out);
			out = NULL;
			bb_error_msg("HTTP response has wrong JSON-RPC id");
		}
		agc_http_resp_free(&resp);
		sb_free(&ctx.body);
		return out;
	}
}

/* ---- JSON-RPC over either transport ---- */

/* send method(params_json) and return the raw response object text */
static char *mcpc_rpc_call(McpSession *s, const char *method,
			   const char *params_json)
{
	char *req;
	char *resp;

	if (s->dead)
		return NULL;
	req = xasprintf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
			s->next_id, method,
			(params_json && params_json[0]) ? params_json : "{}");
	if (s->transport == 1)
		resp = mcpc_stdio_roundtrip(s, req, s->next_id);
	else
		resp = mcpc_http_roundtrip(s, req, s->next_id);
	s->next_id++;
	free(req);
	return resp;
}

static int mcpc_notify(McpSession *s, const char *method)
{
	char *req = xasprintf("{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}", method);
	int rc = 0;

	if (s->transport == 1) {
		if (full_write(s->in_fd, req, strlen(req)) != (ssize_t)strlen(req)
			|| full_write(s->in_fd, "\n", 1) != 1) {
			s->dead = 1;
			rc = -1;
		}
	} else {
		/* Notifications do not carry an id; a 202/empty response is valid. */
		char *r = mcpc_http_roundtrip(s, req, -1);
		if (!r && s->dead)
			rc = -1;
		free(r);
	}
	free(req);
	return rc;
}

/* initialize + initialized; 0 on success (fills server_info/tools) */
static int mcpc_handshake(McpSession *s)
{
	/* protocolVersion is server-dependent: strict endpoints (e.g.
	 * api.z.ai MCP) reject anything but the version they implement,
	 * so connect can pin it with --protocol-version */
	const char *ver = (s->protocol_version && s->protocol_version[0])
			  ? s->protocol_version : "2025-03-26";
	char *init_params = xasprintf(
		"{\"protocolVersion\":\"%s\",\"capabilities\":{},"
		"\"clientInfo\":{\"name\":\"busybox-mcpc\",\"version\":\"1.0\"}}", ver);
	char *resp;
	JsonParse jp;
	JsonVal result;

	resp = mcpc_rpc_call(s, "initialize", init_params);
	free(init_params);
	if (!resp)
		return -1;
	jp = json_parse_root(resp);
	if (jp.error) {
		bb_error_msg("bad initialize response");
		free(resp);
		return -1;
	}
	result = json_get(jp.val, "result");
	if (result.type != JSON_OBJECT) {
		char *err = json_get_string(jp.val, "error");
		bb_error_msg("initialize failed: %s", err ? err : "no result");
		free(err);
		free(resp);
		return -1;
	}
	{
		JsonVal si = json_get(result, "serverInfo");
		free(s->server_info_json);
		if (si.type == JSON_OBJECT)
			s->server_info_json = xstrndup(si.src + si.start,
						       si.end - si.start);
		else
			s->server_info_json = xstrdup("{}");
	}
	free(resp);
	if (mcpc_notify(s, "notifications/initialized") != 0)
		return -1;

	/* cache the tool list up front: tools-list/grep stay offline */
	resp = mcpc_rpc_call(s, "tools/list", "");
	if (resp) {
		jp = json_parse_root(resp);
		if (!jp.error) {
			JsonVal r2 = json_get(jp.val, "result");
			JsonVal tools = json_get(r2, "tools");
			free(s->tools_json);
			if (tools.type == JSON_ARRAY)
				s->tools_json = xstrndup(tools.src + tools.start,
							  tools.end - tools.start);
			else
				s->tools_json = xstrdup("[]");
		}
		free(resp);
	}
	return 0;
}

/* open a session: connect/spawn + handshake. 0 on success. */
static McpSession *mcpc_sess_open(const char *name, const char *target,
				  const char *extra_header,
				  const char *protocol_version)
{
	McpSession *s;
	McpSession *old = mcpc_sess_find(name);

	if (old) {
		bb_error_msg("session '%s' already live (mcpc close @%s first)",
			     name, name);
		return NULL;
	}
	if (g_n_sessions >= MCPC_MAX_SESSIONS) {
		bb_error_msg("too many sessions");
		return NULL;
	}
	s = xzalloc(sizeof(*s));
	s->name = xstrdup(name);
	s->in_fd = s->out_fd = -1;
	s->next_id = 1;

	if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\')
		|| strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		bb_error_msg("invalid session name '%s'", name ? name : "?");
		goto fail;
	}
	if (strncmp(target, "cmd:", 4) == 0) {
		s->transport = 1;
		s->cmd = xstrdup(target + 4);
		if (mcpc_stdio_spawn(s, s->cmd) != 0) {
			bb_error_msg("spawn failed: %s", s->cmd);
			goto fail;
		}
	} else {
		s->transport = 0;
		if (strncmp(target, "http://", 7) != 0
		 && strncmp(target, "https://", 8) != 0) {
			/* upstream convenience: bare host -> https:// */
			snprintf(s->url, sizeof(s->url), "https://%s", target);
		} else {
			snprintf(s->url, sizeof(s->url), "%s", target);
		}
		if (extra_header && extra_header[0])
			s->headers[s->n_headers++] = xstrdup(extra_header);
	}
	if (protocol_version && protocol_version[0])
		s->protocol_version = xstrdup(protocol_version);

	if (mcpc_handshake(s) != 0) {
		bb_error_msg("handshake failed for @%s", name);
		goto fail;
	}
	g_sessions[g_n_sessions++] = s;
	return s;
 fail:
	mcpc_sess_free(s);
	return NULL;
}

/* ============================================================
 * daemon: request handling (line JSON in, line JSON out)
 * ============================================================ */

/* build {"ok":true,"result":<result_json>} or {"ok":false,"error":".."} */
static char *mcpc_ok(const char *result_json)
{
	return xasprintf("{\"ok\":true,\"result\":%s}",
			 (result_json && result_json[0]) ? result_json : "{}");
}

static char *mcpc_err(const char *fmt, const char *arg)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"ok\":false,\"error\":\"");
	{
		const char *p = fmt;
		for (; *p; p++) {
			if (p[0] == '%' && p[1] == 's') {
				const char *a = arg ? arg : "?";
				for (; *a; a++) {
					if (*a == '"' || *a == '\\')
						sb_append_char(&sb, '\\');
					sb_append_char(&sb, *a);
				}
				p++;
			} else {
				if (*p == '"' || *p == '\\')
					sb_append_char(&sb, '\\');
				sb_append_char(&sb, *p);
			}
		}
	}
	sb_append(&sb, "\"}");
	return sb.data;
}

static char *mcpc_daemon_handle(const char *line)
{
	JsonParse jp = json_parse_root(line);
	char *op;
	char *name;
	char *str;

	if (jp.error)
		return mcpc_err("bad request: %s", jp.error);
	op = json_get_string(jp.val, "op");
	if (!op)
		return mcpc_err("missing op", NULL);
	name = json_get_string(jp.val, "name");

	if (strcmp(op, "ping") == 0) {
		if (!name)
			return mcpc_err("missing name", NULL);
		if (!mcpc_sess_find(name))
			return mcpc_err("no live session: %s", name);
		return mcpc_ok("{\"alive\":true}");
	}
	if (strcmp(op, "list") == 0) {
		StrBuf sb;
		int i;
		sb_init(&sb);
		sb_append(&sb, "[");
		for (i = 0; i < g_n_sessions; i++) {
			McpSession *s = g_sessions[i];
			if (!s)
				continue;
			if (sb.len > 1)
				sb_append_char(&sb, ',');
			sb_appendf(&sb, "{\"name\":\"%s\",\"transport\":\"%s\","
				   "\"pid\":%d,\"server_info\":%s}",
				   s->name, s->transport ? "stdio" : "http",
				   (int)getpid(),
				   s->server_info_json ? s->server_info_json : "{}");
		}
		sb_append(&sb, "]");
		str = mcpc_ok(sb.data);
		sb_free(&sb);
		return str;
	}
	if (strcmp(op, "connect") == 0) {
		McpSession *s;
		char *target = json_get_string(jp.val, "target");
		char *hdr = json_get_string(jp.val, "header");
		char *ver = json_get_string(jp.val, "protocol_version");
		char *reply;

		if (!name || !target) {
			free(ver);
			return mcpc_err("connect needs name+target", NULL);
		}
		s = mcpc_sess_open(name, target, hdr, ver);
		free(target);
		free(hdr);
		free(ver);
		if (!s)
			return mcpc_err("cannot connect %s", name);
		{
			char *r = xasprintf("{\"server_info\":%s,\"tools\":%s}",
					    s->server_info_json ? s->server_info_json : "{}",
					    s->tools_json ? s->tools_json : "[]");
			reply = mcpc_ok(r);
			free(r);
		}
		return reply;
	}
	if (strcmp(op, "close") == 0) {
		int i;
		if (!name)
			return mcpc_err("missing name", NULL);
		for (i = 0; i < g_n_sessions; i++) {
			if (g_sessions[i] && strcmp(g_sessions[i]->name, name) == 0) {
				mcpc_sess_free(g_sessions[i]);
				g_sessions[i] = g_sessions[g_n_sessions - 1];
				g_n_sessions--;
				return mcpc_ok(NULL);
			}
		}
		return mcpc_err("no such session: %s", name);
	}
	if (strcmp(op, "call") == 0) {
		McpSession *s;
		char *method;
		char *params;
		char *resp;
		char *reply = NULL;

		if (!name)
			return mcpc_err("missing name", NULL);
		s = mcpc_sess_find(name);
		if (!s)
			return mcpc_err("no live session: %s", name);
		method = json_get_string(jp.val, "method");
		/* params may be any JSON type (object for tools/call arguments):
		 * take the raw source slice, not just string values */
		params = json_as_string(json_get(jp.val, "params"));
		if (!method) {
			free(params);
			return mcpc_err("missing method", NULL);
		}
		resp = mcpc_rpc_call(s, method, params);
		free(method);
		free(params);
		if (!resp)
			reply = mcpc_err("transport failed on @%s", name);
		else
			reply = mcpc_ok(resp);
		free(resp);
		return reply;
	}
	if (strcmp(op, "tools") == 0) {
		McpSession *s;
		if (!name)
			return mcpc_err("missing name", NULL);
		s = mcpc_sess_find(name);
		if (!s)
			return mcpc_err("no live session: %s", name);
		{
			char *r = xasprintf("{\"server_info\":%s,\"tools\":%s}",
					    s->server_info_json ? s->server_info_json : "{}",
					    s->tools_json ? s->tools_json : "[]");
			char *reply = mcpc_ok(r);
			free(r);
			return reply;
		}
	}
	if (strcmp(op, "stop") == 0) {
		g_stop = 1;
		return mcpc_ok("{\"stopping\":true}");
	}
	return mcpc_err("unknown op: %s", op);
}

static void mcpc_sighandler(int sig UNUSED_PARAM)
{
	g_stop = 1;
}

static void mcpc_ignore_sigpipe(void)
{
	if (!g_sigpipe_saved) {
		g_old_sigpipe = signal(SIGPIPE, SIG_IGN);
		g_sigpipe_saved = 1;
	}
}

static void mcpc_close_fds_from(int first)
{
	struct rlimit lim;
	int fd;

	if (getrlimit(RLIMIT_NOFILE, &lim) != 0 || lim.rlim_cur > 65536)
		lim.rlim_cur = 65536;
	for (fd = first; fd < (int)lim.rlim_cur; fd++)
		close(fd);
}

static void mcpc_restore_sigpipe(void)
{
	if (g_sigpipe_saved) {
		signal(SIGPIPE, g_old_sigpipe);
		g_sigpipe_saved = 0;
	}
}

static int mcpc_daemon_alive(void);

static void mcpc_unlink_runtime(const char *sp, const char *pp)
{
	if (sp)
		unlink(sp);
	if (pp)
		unlink(pp);
}

/* the daemon process: listen on the unix socket, serve line requests */
static int mcpc_daemon_run(void)
{
	char *sp = mcpc_sock_path();
	char *pp = mcpc_pid_path();
	char *lp = mcpc_lock_path();
	struct sockaddr_un addr;
	int lfd;
	int lockfd;
	int i;

	mcpc_ignore_sigpipe();
	{
		char *d = mcpc_dir();
		bb_make_directory(d, 0755, FILEUTILS_RECUR);
		free(d);
	}
	lockfd = open(lp, O_RDWR | O_CREAT, MCPC_LOCK_MODE);
	if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
		if (lockfd >= 0)
			close(lockfd);
		free(lp);
		free(sp);
		free(pp);
		return 1;
	}
	free(lp);
	signal(SIGINT, mcpc_sighandler);
	signal(SIGTERM, mcpc_sighandler);

	if (mcpc_daemon_alive()) {
		close(lockfd);
		free(sp);
		free(pp);
		return 1;
	}
	if (unlink(sp) != 0 && errno != ENOENT) {
		bb_perror_msg("unlink %s", sp);
		close(lockfd);
		free(sp);
		free(pp);
		return 1;
	}
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		bb_perror_msg("socket");
		close(lockfd);
		free(sp);
		free(pp);
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sp, sizeof(addr.sun_path) - 1);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		bb_perror_msg("bind %s", sp);
		close(lfd);
		close(lockfd);
		free(sp);
		free(pp);
		return 1;
	}
	if (listen(lfd, 4) != 0) {
		bb_perror_msg("listen");
		close(lfd);
		mcpc_unlink_runtime(sp, pp);
		close(lockfd);
		free(sp);
		free(pp);
		return 1;
	}
	{
		FILE *f = fopen(pp, "w");
		if (!f) {
			bb_perror_msg("open %s", pp);
			close(lfd);
			mcpc_unlink_runtime(sp, pp);
			close(lockfd);
			free(sp);
			free(pp);
			return 1;
		}
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}

	while (!g_stop) {
		int cfd;
		struct pollfd pfd;
		int pr;

		pfd.fd = lfd;
		pfd.events = POLLIN;
		pr = safe_poll(&pfd, 1, 500);
		if (pr <= 0)
			continue;
		cfd = accept(lfd, NULL, NULL);
		if (cfd < 0)
			continue;
		/* one request per connection keeps the protocol trivial */
		{
			StrBuf sb;
			char ch;
			char *resp;
			int64_t deadline = (int64_t)monotonic_ms() + MCPC_CLIENT_TIMEOUT_MS;

			sb_init(&sb);
			for (;;) {
				struct pollfd cpfd;
				int pr;

				cpfd.fd = cfd;
				cpfd.events = POLLIN;
				pr = safe_poll(&cpfd, 1, 250);
				if (pr < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				if (pr == 0) {
					if ((int64_t)monotonic_ms() >= deadline)
						break;
					continue;
				}
				if (safe_read(cfd, &ch, 1) <= 0)
					break;
				if (ch == '\n')
					break;
				if (ch != '\r') {
					if (sb.len >= MCPC_MAX_LINE)
						break;
					sb_append_char(&sb, ch);
				}
			}
			if (sb.data) {
				resp = mcpc_daemon_handle(sb.data);
				if (resp) {
					full_write(cfd, resp, strlen(resp));
					full_write(cfd, "\n", 1);
					free(resp);
				}
			}
			sb_free(&sb);
		}
		close(cfd);
	}

	for (i = 0; i < g_n_sessions; i++)
		if (g_sessions[i])
			mcpc_sess_free(g_sessions[i]);
	close(lfd);
	mcpc_unlink_runtime(sp, pp);
	close(lockfd);
	free(sp);
	free(pp);
	return 0;
}

/* ============================================================
 * CLI side
 * ============================================================ */

static int mcpc_dial(void)
{
	char *sp = mcpc_sock_path();
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		free(sp);
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sp, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		free(sp);
		return -1;
	}
	free(sp);
	return fd;
}

static int mcpc_daemon_alive(void)
{
	char *pp = mcpc_pid_path();
	FILE *f = fopen(pp, "r");
	long pid = 0;
	char buf[64];
	int alive;

	free(pp);
	if (!f)
		return 0;
	if (fgets(buf, sizeof(buf), f))
		pid = atol(buf);
	fclose(f);
	if (pid <= 0)
		return 0;
	alive = kill((pid_t)pid, 0) == 0 || errno == EPERM;
	return alive;
}

static int mcpc_spawn_daemon(void)
{
	char *sp = mcpc_sock_path();
	const char *bb = bb_busybox_exec_path;
	pid_t pid;
	int i;

	if (mcpc_daemon_alive()) {
		int fd = mcpc_dial();
		if (fd >= 0) {
			close(fd);
			free(sp);
			return 0;
		}
		free(sp);
		return -1;
	}
	if (unlink(sp) != 0 && errno != ENOENT) {
		free(sp);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		bb_perror_msg("fork");
		free(sp);
		return -1;
	}
	if (pid == 0) {
		/* detach completely: a daemon holding the caller's stdout
		 * pipe open would hang every "mcpc ... | grep" pipeline */
		{
			int nul = open("/dev/null", O_RDWR);
			if (nul >= 0) {
				dup2(nul, STDIN_FILENO);
				dup2(nul, STDOUT_FILENO);
				dup2(nul, STDERR_FILENO);
				if (nul > 2)
					close(nul);
			}
		}
		setsid();
		mcpc_close_fds_from(3);
		mcpc_restore_sigpipe();
		/* argv[0] must be the applet name: busybox dispatches on it
		 * (same convention as the busyagent tool executor) */
		execl(bb, "mcpc", "daemon", "run", (char *)NULL);
		_exit(127);
	}
	for (i = 0; i < MCPC_DAEMON_START_MS / 100; i++) {
		int fd = mcpc_dial();
		if (fd >= 0) {
			close(fd);
			free(sp);
			return 0;
		}
		usleep(100 * 1000);
	}
	bb_error_msg("daemon did not come up");
	free(sp);
	return -1;
}

/* line request -> malloc'd response line (or NULL + error message) */
static char *mcpc_rpc(const char *line)
{
	int fd = mcpc_dial();
	char *resp = NULL;

	if (fd < 0) {
		if (mcpc_spawn_daemon() != 0) {
			bb_error_msg("cannot start the mcpc daemon");
			return NULL;
		}
		fd = mcpc_dial();
		if (fd < 0) {
			bb_error_msg("cannot reach the mcpc daemon");
			return NULL;
		}
	}
	{
		size_t len = strlen(line);
		if (full_write(fd, line, len) != (ssize_t)len
		 || full_write(fd, "\n", 1) != 1) {
			close(fd);
			return NULL;
		}
	}
	{
		StrBuf sb;
		char ch;
		int64_t deadline = (int64_t)monotonic_ms() + MCPC_CLIENT_TIMEOUT_MS;

		sb_init(&sb);
		for (;;) {
			struct pollfd pfd;
			int pr;
			ssize_t n;

			pfd.fd = fd;
			pfd.events = POLLIN;
			pr = safe_poll(&pfd, 1, 250);
			if (pr < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (pr == 0) {
				if ((int64_t)monotonic_ms() >= deadline)
					break;
				continue;
			}
			if (!(pfd.revents & (POLLIN | POLLHUP)))
				break;
			n = safe_read(fd, &ch, 1);
			if (n <= 0)
				break;
			if (ch == '\n')
				break;
			if (ch != '\r') {
				if (sb.len >= MCPC_MAX_LINE) {
					sb_free(&sb);
					close(fd);
					return NULL;
				}
				sb_append_char(&sb, ch);
			}
		}
		close(fd);
		if (sb.data)
			resp = sb.data;
		else
			sb_free(&sb);
	}
	return resp;
}

/* json-escape a string into sb */
static void mcpc_esc(StrBuf *sb, const char *s)
{
	sb_append_json_string(sb, s ? s : "");
}

/* ---- CLI commands ---- */

static int mcpc_cmd_connect(const char *name, const char *target,
			    const char *header, const char *protocol_version)
{
	StrBuf req;
	char *resp;
	JsonParse jp;
	int rc = 1;

	sb_init(&req);
	sb_append(&req, "{\"op\":\"connect\",\"name\":");
	mcpc_esc(&req, name);
	sb_append(&req, ",\"target\":");
	mcpc_esc(&req, target);
	if (header && header[0]) {
		sb_append(&req, ",\"header\":");
		mcpc_esc(&req, header);
	}
	if (protocol_version && protocol_version[0]) {
		sb_append(&req, ",\"protocol_version\":");
		mcpc_esc(&req, protocol_version);
	}
	sb_append(&req, "}");
	resp = mcpc_rpc(req.data);
	sb_free(&req);
	if (!resp)
		return 1;
	jp = json_parse_root(resp);
	if (jp.error) {
		bb_error_msg("daemon: %s", resp);
		goto out;
	}
	if (!json_get_bool(jp.val, "ok", 0)) {
		char *err = json_get_string(jp.val, "error");
		bb_error_msg("connect @%s failed: %s", name, err ? err : "?");
		free(err);
		goto out;
	}
	{
		/* mirror the session cache for offline tools-get/grep */
		JsonVal r = json_get(jp.val, "result");
		JsonVal si = json_get(r, "server_info");
		JsonVal tools = json_get(r, "tools");
		StrBuf cache;
		sb_init(&cache);
		sb_append(&cache, "{\"transport\":");
		mcpc_esc(&cache, strncmp(target, "cmd:", 4) == 0 ? "stdio" : "http");
		sb_append(&cache, ",\"source\":");
		mcpc_esc(&cache, target);
		sb_append(&cache, ",\"server_info\":");
		sb_append(&cache, si.type == JSON_OBJECT
			  ? xstrndup(si.src + si.start, si.end - si.start)
			  : xstrdup("{}"));
		sb_append(&cache, ",\"tools\":");
		sb_append(&cache, tools.type == JSON_ARRAY
			  ? xstrndup(tools.src + tools.start, tools.end - tools.start)
			  : xstrdup("[]"));
		sb_append(&cache, "}");
		mcpc_cache_save(name, cache.data);
		sb_free(&cache);
	}
	printf("connected @%s\n", name);
	rc = 0;
 out:
	free(resp);
	return rc;
}

static void mcpc_print_tools(const char *tools_json, int full)
{
	JsonParse jp = json_parse_root(tools_json);
	JsonVal arr = jp.val;
	int i;

	if (jp.error)
		return;
	/* accept both a bare tools array and the cache wrapper object */
	if (arr.type == JSON_OBJECT)
		arr = json_get(arr, "tools");
	for (i = 0; i < json_array_len(arr); i++) {
		JsonVal t = json_array_get(arr, i);
		char *name = json_get_string(t, "name");
		char *desc = json_get_string(t, "description");
		if (full) {
			printf("%.*s\n", (int)(t.end - t.start), t.src + t.start);
		} else {
			char *d1 = NULL;
			if (desc) {
				d1 = strtok(desc, "\n");
			}
			printf("%-24s %s\n", name ? name : "?", d1 ? d1 : "");
			free(desc);
		}
		free(name);
	}
}

static int mcpc_cmd_session(const char *name, char **argv, int argc,
			    int json_out)
{
	/* @name tools-list [--full] | tools-get T | tools-call T args | ping */
	const char *cmd = argv[0];
	int rc = 1;

	if (!cmd)
		bb_error_msg_and_die("no session command (tools-list|tools-get|tools-call|ping)");

	if (strcmp(cmd, "ping") == 0) {
		char *req = xasprintf("{\"op\":\"ping\",\"name\":\"%s\"}", name);
		char *resp = mcpc_rpc(req);
		JsonParse jp;

		free(req);
		if (!resp)
			return 1;
		jp = json_parse_root(resp);
		if (jp.error || !json_get_bool(jp.val, "ok", 0)) {
			char *err = jp.error ? NULL : json_get_string(jp.val, "error");
			bb_error_msg("@%s: %s", name, err ? err : "ping failed");
			free(err);
			free(resp);
			return 1;
		}
		free(resp);
		printf("pong @%s\n", name);
		return 0;
	}

	if (strcmp(cmd, "tools-list") == 0) {
		/* local cache first, daemon refresh as fallback */
		char *cache = mcpc_cache_load(name);
		int full = (argc > 1 && strcmp(argv[1], "--full") == 0);
		if (cache) {
			JsonParse cp = json_parse_root(cache);
			if (!cp.error) {
				if (json_out)
					printf("%s\n", cache);
				else
					mcpc_print_tools(cache, full);
				free(cache);
				return 0;
			}
			free(cache);
		}
		/* fall back to the daemon copy */
		{
			StrBuf req;
			char *resp;
			JsonParse jp;

			sb_init(&req);
			sb_append(&req, "{\"op\":\"tools\",\"name\":");
			mcpc_esc(&req, name);
			sb_append(&req, "}");
			resp = mcpc_rpc(req.data);
			sb_free(&req);
			if (!resp)
				return 1;
			jp = json_parse_root(resp);
			if (!jp.error && json_get_bool(jp.val, "ok", 0)) {
				char *tools = json_get_string(json_get(jp.val, "result"), "tools");
				if (json_out)
					printf("%s\n", tools ? tools : "[]");
				else
					mcpc_print_tools(tools ? tools : "[]", full);
				free(tools);
				rc = 0;
			}
			free(resp);
			return rc;
		}
	}

	if (strcmp(cmd, "tools-get") == 0) {
		char *cache = mcpc_cache_load(name);
		const char *want = argv[1];
		JsonParse cp;
		JsonVal arr;
		int i;

		if (!cache)
			bb_error_msg_and_die("no cached tools for @%s (connect first)", name);
		if (!want)
			bb_error_msg_and_die("usage: mcpc @%s tools-get TOOL", name);
		cp = json_parse_root(cache);
		if (cp.error)
			bb_error_msg_and_die("bad cache for @%s", name);
		arr = json_get(cp.val, "tools");
		for (i = 0; i < json_array_len(arr); i++) {
			JsonVal t = json_array_get(arr, i);
			char *nm = json_get_string(t, "name");
			int hit = (nm && strcmp(nm, want) == 0);
			free(nm);
			if (hit) {
				printf("%.*s\n", (int)(t.end - t.start), t.src + t.start);
				free(cache);
				return 0;
			}
		}
		bb_error_msg_and_die("tool '%s' not found in @%s", want, name);
	}

	if (strcmp(cmd, "tools-call") == 0) {
		const char *tool = argv[1];
		StrBuf args;
		StrBuf creq;
		char *resp;
		JsonParse jp;
		int i;

		if (!tool)
			bb_error_msg_and_die("usage: mcpc @%s tools-call TOOL [K=V|K:=JSON...]",
					     name);
		sb_init(&args);
		sb_append(&args, "{\"name\":");
		mcpc_esc(&args, tool);
		sb_append(&args, ",\"arguments\":{");
		for (i = 2; i < argc; i++) {
			const char *a = argv[i];
			const char *eq = strchr(a, '=');
			char *key;
			int raw = 0;

			if (!eq || eq == a)
				bb_error_msg_and_die("bad argument '%s' (want K=V or K:=JSON)", a);
			key = xstrndup(a, eq - a);
			if (key[strlen(key)-1] == ':') {
				key[strlen(key)-1] = '\0';
				raw = 1;
			}
			if (i > 2)
				sb_append_char(&args, ',');
			mcpc_esc(&args, key);
			sb_append_char(&args, ':');
			if (raw && eq[1]) {
				JsonParse vp = json_parse_root(eq + 1);
				if (!vp.error)
					sb_append(&args, eq + 1);
				else
					mcpc_esc(&args, eq + 1);
			} else {
				mcpc_esc(&args, eq + 1);
			}
			free(key);
		}
		sb_append(&args, "}}");

		sb_init(&creq);
		sb_append(&creq, "{\"op\":\"call\",\"name\":");
		mcpc_esc(&creq, name);
		sb_append(&creq, ",\"method\":\"tools/call\",\"params\":");
		sb_append(&creq, args.data);
		sb_append(&creq, "}");
		sb_free(&args);

		resp = mcpc_rpc(creq.data);
		sb_free(&creq);
		if (!resp)
			return 1;
		jp = json_parse_root(resp);
		if (jp.error || !json_get_bool(jp.val, "ok", 0)) {
			char *err = json_get_string(jp.val, "error");
			bb_error_msg("tools-call failed: %s", err ? err : "daemon error");
			free(err);
			free(resp);
			return 1;
		}
		{
			/* result embeds the raw JSON-RPC response object */
			JsonVal r = json_get(jp.val, "result");
			JsonVal inner = json_get(r, "result");
			JsonVal errv = json_get(r, "error");

			if (errv.type == JSON_OBJECT) {
				printf("%.*s\n", (int)(errv.end - errv.start),
				       errv.src + errv.start);
				free(resp);
				return 1;
			}
			if (inner.type == JSON_NULL)
				printf("%s\n", resp);
			else
				printf("%.*s\n", (int)(inner.end - inner.start),
				       inner.src + inner.start);
		}
		free(resp);
		return 0;
	}

	bb_error_msg_and_die("unknown session command '%s'", cmd);
}

static int mcpc_cmd_grep(const char *pattern)
{
	char *dir = xasprintf("%s/servers", mcpc_dir());
	DIR *d = opendir(dir);
	struct dirent *de;
	int hits = 0;

	free(dir);
	if (!d) {
		printf("no cached servers (mcpc connect URL @name)\n");
		return 0;
	}
	while ((de = readdir(d)) != NULL) {
		char *dot = strrchr(de->d_name, '.');
		char name[128];
		char *data;
		char *line;

		if (!dot || strcmp(dot, ".json") != 0)
			continue;
		{
			size_t n = dot - de->d_name;
			if (n >= sizeof(name))
				continue;
			memcpy(name, de->d_name, n);
			name[n] = '\0';
		}
		data = mcpc_cache_load(name);
		if (!data)
			continue;
		/* search each tool's name/description */
		{
			JsonParse jp = json_parse_root(data);
			int i;
			if (!jp.error) {
				JsonVal tools = json_get(jp.val, "tools");
				for (i = 0; i < json_array_len(tools); i++) {
					JsonVal t = json_array_get(tools, i);
					char *nm = json_get_string(t, "name");
					char *ds = json_get_string(t, "description");
					if ((nm && strstr(nm, pattern))
					 || (ds && strstr(ds, pattern))) {
						printf("%-10s %s\n", name, nm ? nm : "?");
						hits++;
					}
					free(nm);
					free(ds);
				}
			}
		}
		free(data);
		(void)line;
	}
	closedir(d);
	if (!hits)
		printf("no tools match '%s'\n", pattern);
	return 0;
}

int mcpc_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;

int mcpc_main(int argc UNUSED_PARAM, char **argv)
{
	const char *a1;

	mcpc_ignore_sigpipe();
	int json_out = 0;
	int i;

	if (!argv[1])
		bb_show_usage();

	/* global flags may sit anywhere; strip --json/--insecure/--timeout N */
	{
		int out = 1;
		for (i = 1; argv[i]; i++) {
			if (strcmp(argv[i], "--json") == 0)
				json_out = 1;
			else if (strcmp(argv[i], "--insecure") == 0)
				continue;
			else if (strcmp(argv[i], "--timeout") == 0 && argv[i+1])
				i++;
			else if (strncmp(argv[i], "--timeout=", 10) == 0)
				continue;
			else
				argv[out++] = argv[i];
		}
		argv[out] = NULL;
	}

	a1 = argv[1];
	if (!a1)
		bb_show_usage();

	if (strcmp(a1, "daemon") == 0) {
		if (argv[2] && strcmp(argv[2], "run") == 0)
			return mcpc_daemon_run();
		if (argv[2] && strcmp(argv[2], "stop") == 0) {
			char *resp = mcpc_rpc("{\"op\":\"stop\"}");
			char *sp;
			int waited;

			if (!resp)
				return 1;
			free(resp);
			/* report success only after the daemon really took the
			 * socket down: closing sessions can take a moment */
			sp = mcpc_sock_path();
			for (waited = 0; waited < MCPC_DAEMON_START_MS; waited += 100) {
				struct stat st;

				if (stat(sp, &st) != 0)
					break;
				usleep(100 * 1000);
			}
			free(sp);
			printf("daemon stopped\n");
			return 0;
		}
		bb_error_msg_and_die("usage: mcpc daemon run|stop");
	}

	if (strcmp(a1, "connect") == 0) {
		const char *target = argv[2];
		const char *name = argv[3] ? mcpc_bare(argv[3]) : NULL;
		const char *def = "default";
		const char *header = NULL;
		const char *pver = NULL;
		int k;

		if (!target)
			bb_error_msg_and_die("usage: mcpc connect URL|cmd:CMD [@NAME]"
					     " [-H HEADER] [--protocol-version VER]");
		if (!name)
			name = def;
		for (k = 3; argv[k]; k++) {
			if (strncmp(argv[k], "--header=", 9) == 0)
				header = argv[k] + 9;
			else if (strcmp(argv[k], "-H") == 0 && argv[k+1])
				header = argv[++k];
			else if (strncmp(argv[k], "--protocol-version=", 19) == 0)
				pver = argv[k] + 19;
			else if (strcmp(argv[k], "--protocol-version") == 0
			 && argv[k+1])
				pver = argv[++k];
		}
		return mcpc_cmd_connect(name, target, header, pver);
	}

	if (strcmp(a1, "ls") == 0) {
		char *resp = mcpc_rpc("{\"op\":\"list\"}");
		JsonParse jp;
		int li;

		if (!resp)
			return 1;
		jp = json_parse_root(resp);
		if (jp.error || !json_get_bool(jp.val, "ok", 0)) {
			printf("no sessions (mcpc connect URL @name)\n");
			free(resp);
			return 0;
		}
		{
			JsonVal arr = json_get(jp.val, "result");
			for (li = 0; li < json_array_len(arr); li++) {
				JsonVal s = json_array_get(arr, li);
				char *nm = json_get_string(s, "name");
				char *tr = json_get_string(s, "transport");
				int pid = json_get_int(s, "pid");
				if (json_out)
					printf("%.*s\n", (int)(s.end - s.start), s.src + s.start);
				else
					printf("@%-14s %-6s live (daemon pid %d)\n",
					       nm ? nm : "?", tr ? tr : "?", pid);
				free(nm);
				free(tr);
			}
		}
		free(resp);
		return 0;
	}

	if (strcmp(a1, "close") == 0) {
		StrBuf req;
		char *resp;
		JsonParse cp;
		if (!argv[2] || argv[3])
			bb_error_msg_and_die("usage: mcpc close @NAME");
		sb_init(&req);
		sb_append(&req, "{\"op\":\"close\",\"name\":");
		mcpc_esc(&req, mcpc_bare(argv[2]));
		sb_append(&req, "}");
		resp = mcpc_rpc(req.data);
		sb_free(&req);
		if (!resp)
			return 1;
		cp = json_parse_root(resp);
		if (cp.error || !json_get_bool(cp.val, "ok", 0)) {
			char *err = json_get_string(cp.val, "error");
			bb_error_msg("close failed: %s", err ? err : "daemon error");
			free(err);
			free(resp);
			return 1;
		}
		printf("closed @%s\n", mcpc_bare(argv[2]));
		free(resp);
		return 0;
	}

	if (strcmp(a1, "grep") == 0) {
		if (!argv[2])
			bb_error_msg_and_die("usage: mcpc grep PATTERN");
		return mcpc_cmd_grep(argv[2]);
	}

	if (a1[0] == '@') {
		int rc;
		/* shift past the session commands for the arg builder */
		char **sargv = argv + 2;
		int sargc = 0;
		while (sargv[sargc])
			sargc++;
		rc = mcpc_cmd_session(mcpc_bare(a1), sargv, sargc, json_out);
		return rc;
	}

	bb_error_msg("unknown command '%s' (connect|ls|close|grep|daemon|@name ...)", a1);
	return 1;
}
