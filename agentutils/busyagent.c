/*
 * busyagent - single user-turn LLM agent applet for busybox
 *
 * One invocation = one user turn: resolve session (cwd-bound by default,
 * --new/--session to override), rebuild history into the request prefix,
 * stream the reply over plain HTTP (SSE), execute tool calls with busybox
 * applets, feed results back until the model stops, append the turn's
 * events to the session trace, exit.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
//config:config BUSYAGENT
//config:	bool "busyagent"
//config:	default y
//config:	select FEATURE_PREFER_APPLETS
//config:	help
//config:	Run exactly one user turn of an LLM agent loop: build a chat
//config:	request, stream the reply over plain HTTP (SSE), execute any
//config:	tool calls with busybox applets, feed results back until the
//config:	model finishes, then exit. Session history is stored under
//config:	$BB_AGENT_HOME and restored automatically per working directory.
//config:
//applet:IF_BUSYAGENT(APPLET(busyagent, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_BUSYAGENT) += busyagent.o ba_json.o ba_util.o
//kbuild:lib-$(CONFIG_BUSYAGENT) += ba_store.o ba_transport.o bb_http.o ba_tools.o

//usage:#define busyagent_trivial_usage
//usage:       "[OPTIONS] [PROMPT]"
//usage:#define busyagent_full_usage "\n\n"
//usage:       "Run one user turn of an LLM agent loop\n"
//usage:     "\n	-u URL	Base URL (default $BB_AGENT_BASE_URL)"
//usage:     "\n	-k KEY	API key (default $BB_AGENT_API_KEY)"
//usage:     "\n	-m MODEL	Model name"
//usage:     "\n	-p PROVIDER	claude | openai | responses (default openai)"
//usage:     "\n	-t N	Max model turns (default 8)"
//usage:     "\n	-s ID	Resume exactly this session"
//usage:     "\n	-n	Force a new session"
//usage:     "\n	-c	Resume latest session for this cwd (the default)"
//usage:     "\n	-o FMT	Output: text | json (default text)"
//usage:     "\n	-v	Verbose request info on stderr"
//usage:     "\n	-i [PATH]	Write a starter tools.json (default $BB_AGENT_HOME/tools.json) and exit"
//usage:     "\n	PROMPT	Reads stdin when no PROMPT is given"
//usage:

#include "libbb.h"
#include <unistd.h>
#include "ba_util.h"
#include "ba_json.h"
#include "ba_transport.h"
#include "ba_store.h"
#include "ba_tools.h"

#define BA_MAX_TOKENS     16384
#define BA_DEFAULT_TURNS  8
#define BA_CTX_LIMIT_BYTES (512 * 1024)

/* ---- per-turn context: accumulator + renderer + trace ---- */

typedef struct {
	SseAccumulator accum;
	const SessionPaths *paths;
	int output_json;
	int verbose;
} TurnCtx;

static void ba_emit_json_str(StrBuf *sb, const char *key, const char *val)
{
	sb_append(sb, ",\"");
	sb_append(sb, key);
	sb_append(sb, "\":");
	sb_append_json_string(sb, val ? val : "");
}

/* Render one logical event to stdout (text or stream-json) and to the
 * session trace (events.jsonl). Shapes follow bash-agent display.c. */
static void ba_render(TurnCtx *t, const char *json_frag)
{
	StrBuf line;
	const char *type = NULL;

	JsonParse jp = json_parse_root(json_frag);
	if (!jp.error)
		type = json_get_string(jp.val, "type");

	sb_init(&line);
	sb_append(&line, json_frag);
	sb_append_char(&line, '\n');

	if (t->output_json) {
		fwrite(line.data, 1, line.len, stdout);
		fflush(stdout);
	} else {
		/* human rendering（复用上面的 jp） */
		if (!jp.error && type) {
			if (strcmp(type, "text") == 0) {
				char *c = json_get_string(jp.val, "content");
				if (c) { fwrite(c, 1, strlen(c), stdout); fflush(stdout); free(c); }
			} else if (strcmp(type, "thinking") == 0 && t->verbose) {
				char *c = json_get_string(jp.val, "content");
				if (c) { fprintf(stderr, "[think] %s\n", c); free(c); }
			} else if (strcmp(type, "tool_call") == 0) {
				char *n = json_get_string(jp.val, "name");
				fprintf(stderr, "[tool] %s\n", n ? n : "?");
				free(n);
			} else if (strcmp(type, "tool_result") == 0 && t->verbose) {
				char *c = json_get_string(jp.val, "content");
				if (c) { fprintf(stderr, "[tool out] %.400s%s\n", c, strlen(c) > 400 ? "..." : ""); free(c); }
			} else if (strcmp(type, "usage") == 0 && t->verbose) {
				fprintf(stderr, "[usage] in=%d out=%d\n",
					json_get_int(jp.val, "input_tokens"),
					json_get_int(jp.val, "output_tokens"));
			} else if (strcmp(type, "error") == 0) {
				char *c = json_get_string(jp.val, "message");
				fprintf(stderr, "error: %s\n", c ? c : "?");
				free(c);
			}
		}
	}
	store_event_append(t->paths, line.data);
	sb_free(&line);
}

static void ba_render_text(TurnCtx *t, const char *content)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"type\":\"text\",\"content\":");
	sb_append_json_string(&sb, content);
	sb_append_char(&sb, '}');
	ba_render(t, sb.data);
	sb_free(&sb);
}

static void ba_render_thinking(TurnCtx *t, const char *content)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"type\":\"thinking\",\"content\":");
	sb_append_json_string(&sb, content);
	sb_append_char(&sb, '}');
	ba_render(t, sb.data);
	sb_free(&sb);
}

static void ba_render_tool_call(TurnCtx *t, const char *id, const char *name,
				const char *input_json)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"type\":\"tool_call\",\"name\":");
	sb_append_json_string(&sb, name ? name : "");
	ba_emit_json_str(&sb, "id", id);
	sb_append(&sb, ",\"input\":");
	sb_append(&sb, (input_json && input_json[0]) ? input_json : "{}");
	sb_append_char(&sb, '}');
	ba_render(t, sb.data);
	sb_free(&sb);
}

static void ba_render_tool_result(TurnCtx *t, const char *id, const char *name,
				  const char *content)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"type\":\"tool_result\",\"tool_use_id\":");
	sb_append_json_string(&sb, id ? id : "");
	ba_emit_json_str(&sb, "name", name);
	sb_append(&sb, ",\"content\":");
	sb_append_json_string(&sb, content ? content : "");
	sb_append_char(&sb, '}');
	ba_render(t, sb.data);
	sb_free(&sb);
}

static void ba_render_usage(TurnCtx *t, int in, int out, int cr, int cc)
{
	StrBuf sb;
	sb_init(&sb);
	sb_appendf(&sb, "{\"type\":\"usage\",\"input_tokens\":%d,\"output_tokens\":%d,"
		   "\"cache_read_input_tokens\":%d,\"cache_creation_input_tokens\":%d,"
		   "\"kind\":\"agent\"}", in, out, cr, cc);
	ba_render(t, sb.data);
	sb_free(&sb);
}

static void ba_render_stop(TurnCtx *t, const char *reason)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"type\":\"stop\",\"reason\":");
	sb_append_json_string(&sb, reason ? reason : "");
	sb_append_char(&sb, '}');
	ba_render(t, sb.data);
	sb_free(&sb);
}

static void ba_render_error(TurnCtx *t, const char *msg)
{
	StrBuf sb;
	sb_init(&sb);
	sb_append(&sb, "{\"type\":\"error\",\"message\":");
	sb_append_json_string(&sb, msg ? msg : "");
	sb_append_char(&sb, '}');
	ba_render(t, sb.data);
	sb_free(&sb);
}

/* SSE callback: render + accumulate. */
static void ba_sse_callback(void *ctx, const SseEvent *evt)
{
	TurnCtx *t = (TurnCtx *)ctx;

	switch (evt->type) {
	case SSE_TEXT:
		if (!t->output_json) {
			fwrite(evt->content, 1, strlen(evt->content), stdout);
			fflush(stdout);
		}
		break;
	case SSE_ERROR:
		ba_render_error(t, evt->content);
		break;
	default:
		break;
	}
	sse_accum_callback(&t->accum, evt);
}

/* Flush accumulated turn output as trace events (text arrives streaming in
 * human mode; in json mode we emit one text event per accumulated reply to
 * keep the event stream small). */
static void ba_flush_turn_events(TurnCtx *t)
{
	if (t->accum.thinking.len > 0)
		ba_render_thinking(t, t->accum.thinking.data);
	if (t->accum.text.len > 0 && t->output_json)
		ba_render_text(t, t->accum.text.data);
	if (t->accum.in_tokens || t->accum.out_tokens)
		ba_render_usage(t, t->accum.in_tokens, t->accum.out_tokens,
				t->accum.cache_read_tokens, t->accum.cache_creation_tokens);
}

/* ---- context trimming: drop oldest complete exchanges from request
 * prefix only (trace stays intact) ---- */

static int ba_trim_history(const char *conv_path)
{
	char **lines = NULL;
	int n = 0, i, start = 0;
	size_t total = 0;

	if (store_conv_line_count(conv_path, &lines, &n) != 0)
		return 0;
	for (i = 0; i < n; i++)
		total += strlen(lines[i]) + 1;
	if (total <= BA_CTX_LIMIT_BYTES) {
		for (i = 0; i < n; i++) free(lines[i]);
		free(lines);
		return 0;
	}
	/* find how many leading lines to drop: to a user-line boundary,
	 * keeping at least the most recent exchange */
	for (i = 0; i < n - 2; i++) {
		JsonParse jp = json_parse_root(lines[i]);
		int is_user_input = 0;
		if (!jp.error) {
			char *r = json_get_string(jp.val, "role");
			JsonVal content = json_get(jp.val, "content");
			/* 真 user 输入行 content 是字符串；tool_result 行是数组 */
			is_user_input = (r && strcmp(r, "user") == 0
					 && content.type == JSON_STRING);
			free(r);
		}
		total -= strlen(lines[i]) + 1;
		start = i + 1;
		if (is_user_input && total <= BA_CTX_LIMIT_BYTES)
			break;
	}
	for (i = 0; i < n; i++) free(lines[i]);
	free(lines);
	if (start > 0)
		store_conv_trim_tail(conv_path, n - start) /* keep last n-start */;
	return start;
}

int busyagent_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int busyagent_main(int argc UNUSED_PARAM, char **argv)
{
	const char *base_url = getenv("BB_AGENT_BASE_URL");
	const char *api_key  = getenv("BB_AGENT_API_KEY");
	const char *home_env = getenv("BB_AGENT_HOME");
	const char *model = NULL, *provider = "openai", *session_arg = NULL;
	const char *output_fmt = "text";
	int max_turns = BA_DEFAULT_TURNS, verbose = 0;
	int opt_new = 0;
	char *prompt = NULL, *home, *cwd, *session_id = NULL;
	char *tools_json = NULL;
	SessionPaths paths;
	char *api_url;
	int rc = 0;
	unsigned opts;
	const char *o_u = NULL, *o_k = NULL, *o_m = NULL, *o_p = NULL;
	const char *o_t = NULL, *o_s = NULL, *o_o = NULL;
	/* bit positions follow the option string "vu:k:m:p:t:s:o:nc" */
	enum {
		OPT_verbose = 1 << 0,
		OPT_new     = 1 << 8,
		OPT_continue= 1 << 9,
		OPT_init    = 1 << 10,
	};

	opts = getopt32(argv, "^"
		"vu:k:m:p:t:s:o:nci" "\0",
		&o_u, &o_k, &o_m, &o_p, &o_t, &o_s, &o_o);
	if (opts & OPT_verbose) verbose = 1;
	if (opts & OPT_new) opt_new = 1;
	/* OPT_continue is the default behavior; the flag is a no-op kept
	 * for muscle-memory compatibility with cagent */

	if (opts & OPT_init) {
		/* -i [PATH]: export starter tools.json and exit */
		const char *dst = argv[optind];
		char *def = NULL;
		int trc;
		if (!dst || !dst[0]) {
			const char *h = getenv("BB_AGENT_HOME");
			def = xasprintf("%s/tools.json", (h && h[0]) ? h : "/tmp/busyagent");
			dst = def;
		}
		trc = ba_tools_write_template(dst);
		if (trc == 0)
			printf("wrote %s\n", dst);
		else if (trc == -1)
			bb_error_msg_and_die("%s already exists (delete it first to re-export)", dst);
		else
			bb_error_msg_and_die("cannot write %s", dst);
		free(def);
		return 0;
	}
	if (o_u) base_url = o_u;
	if (o_k) api_key = o_k;
	if (o_m) model = o_m;
	if (o_p) provider = o_p;
	if (o_t) max_turns = atoi(o_t);
	if (o_s) session_arg = o_s;
	if (o_o) output_fmt = o_o;
	if (max_turns <= 0) max_turns = BA_DEFAULT_TURNS;

	argv += optind;
	if (!argv[0]) {
		/* read prompt from stdin */
		size_t cap = 4096, len = 0;
		prompt = xmalloc(cap);
		while (1) {
			ssize_t n = read(0, prompt + len, cap - len - 1);
			if (n < 0) bb_perror_msg_and_die("read stdin");
			if (n == 0) break;
			len += n;
			if (cap - len < 2) {
				cap *= 2;
				prompt = xrealloc(prompt, cap);
			}
		}
		prompt[len] = '\0';
		trim(prompt);   /* libbb: strip surrounding whitespace */
		if (!prompt[0])
			bb_error_msg_and_die("empty prompt (no PROMPT arg and empty stdin)");
	} else {
		prompt = xstrdup(argv[0]);
	}

	if (!base_url || !base_url[0])
		bb_error_msg_and_die("no base URL: use -u URL or $BB_AGENT_BASE_URL");
	if (!api_key || !api_key[0])
		bb_error_msg_and_die("no API key: use -k KEY or $BB_AGENT_API_KEY");
	if (!model || !model[0])
		bb_error_msg_and_die("no model: use -m MODEL");
	if (strcmp(provider, "claude") != 0 && strcmp(provider, "openai") != 0
	 && strcmp(provider, "responses") != 0)
		bb_error_msg_and_die("bad provider '%s' (claude|openai|responses)", provider);

	/* api url */
	{
		size_t bl = strlen(base_url);
		char *b_stripped = NULL;
		const char *b = base_url;
		if (bl && base_url[bl-1] == '/') {
			b_stripped = xstrndup(base_url, bl - 1);
			b = b_stripped;
		}
		if (strcmp(provider, "claude") == 0)
			api_url = xasprintf("%s/messages", b);
		else if (strcmp(provider, "responses") == 0)
			api_url = xasprintf("%s/responses", b);
		else
			api_url = xasprintf("%s/chat/completions", b);
		free(b_stripped);
	}

	/* session resolution: --session > --new > default(cwd latest) */
	home = xstrdup(home_env && home_env[0] ? home_env : "/tmp/busyagent");
	cwd = xrealloc_getcwd_or_warn(NULL);
	if (session_arg && session_arg[0]) {
		session_id = xstrdup(session_arg);
	} else if (opt_new) {
		session_id = session_new_id();
	} else {
		session_id = store_session_resolve_continue(home, cwd);
		if (!session_id)
			session_id = session_new_id();
	}
	{
		int is_new;
		paths = store_session_paths_for(home, cwd, session_id);
		is_new = (access(paths.session_dir, F_OK) != 0);
		if (store_session_init(&paths, is_new) != 0) {
			bb_error_msg_and_die("session init failed: %s", paths.session_dir);
		}
		if (verbose)
			fprintf(stderr, "[verbose] session=%s (%s) home=%s\n",
				session_id, is_new ? "new" : "resumed", home);
	}

	/* 提示（如缺失/损坏）由 ba_tools_init 内部给出：缺表即纯 chat 降级 */
	ba_tools_init(home);

	/* record user turn */
	{
		StrBuf evt;
		sb_init(&evt);
		sb_append(&evt, "{\"type\":\"user_input\",\"content\":");
		sb_append_json_string(&evt, prompt);
		sb_append_char(&evt, '}');
		store_event_append(&paths, evt.data);
		sb_free(&evt);
	}
	if (store_conv_add_user(paths.conversation, prompt) != 0)
		bb_error_msg_and_die("conversation append failed");

	/* minimal system prompt (phase 1) */
	{
		const char *sys_min =
			"You are busyagent, a minimal agent running as a busybox applet.\n"
			"Answer concisely. Use tools when needed to inspect files or run commands.";

		int turn;
		tools_json = ba_tools_json();   /* constant across turns */
		for (turn = 0; turn < max_turns; turn++) {
			TurnCtx tctx;
			char **lines = NULL;
			int line_count = 0;
			char *claude_body, *body;
			const char *headers[8];
			char auth_header[512];
			int hdr_count = 0, sse_rc;
			SseAccumulator *accum;

			ba_trim_history(paths.conversation);

			if (store_conv_line_count(paths.conversation, &lines, &line_count) != 0)
				bb_error_msg_and_die("conversation read failed");

			claude_body = build_claude_request(model, sys_min, tools_json,
							   lines, line_count,
							   BA_MAX_TOKENS, NULL, NULL);
			body = claude_body;
			if (strcmp(provider, "openai") == 0) {
				body = convert_to_openai(claude_body);
				free(claude_body);
			} else if (strcmp(provider, "responses") == 0) {
				body = convert_to_responses(claude_body);
				free(claude_body);
			}

			if (verbose && body)
				fprintf(stderr, "[verbose] request body (%zu bytes): %.200s%s\n",
					strlen(body), body, strlen(body) > 200 ? "..." : "");

			headers[hdr_count++] = "Content-Type: application/json";
			headers[hdr_count++] = "User-Agent: busyagent/0.1";
			if (strcmp(provider, "claude") == 0) {
				headers[hdr_count++] = "x-app: cli";
				snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key);
				headers[hdr_count++] = auth_header;
				headers[hdr_count++] = "anthropic-version: 2023-06-01";
			} else {
				snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
				headers[hdr_count++] = auth_header;
			}

			memset(&tctx, 0, sizeof(tctx));
			tctx.paths = &paths;
			tctx.output_json = (strcmp(output_fmt, "json") == 0);
			tctx.verbose = verbose;
			sse_accum_init(&tctx.accum);
			accum = &tctx.accum;

			sse_rc = http_post_sse(api_url, headers, hdr_count,
					       body, strlen(body), provider,
					       ba_sse_callback, &tctx, NULL);
			free(body);
			{
				int i;
				for (i = 0; i < line_count; i++) free(lines[i]);
				free(lines);
			}

			if (sse_rc != 0 || accum->error) {
				/* SSE_ERROR 回调已渲染过时不重复（json 流/trace 双份） */
				if (!accum->error)
					ba_render_error(&tctx, "HTTP request failed");
				ba_render_stop(&tctx, "error");
				sse_accum_free(accum);
				rc = 1;
				goto out;
			}

			ba_flush_turn_events(&tctx);

			if (accum->tool_count > 0 && accum->stop_reason
			 && (strcmp(accum->stop_reason, "tool_use") == 0
			  || strcmp(accum->stop_reason, "tool_calls") == 0)) {
				const char **ids, **contents;
				int i;
				/* json 模式下中间轮的 text/thinking/usage 也要进输出流和 trace */
				ba_flush_turn_events(&tctx);
				/* record assistant message with tool calls */
				{
					const char **tids = xzalloc(accum->tool_count * sizeof(char *));
					const char **tnames = xzalloc(accum->tool_count * sizeof(char *));
					const char **tinputs = xzalloc(accum->tool_count * sizeof(char *));
					for (i = 0; i < accum->tool_count; i++) {
						tids[i] = accum->tools[i].id;
						tnames[i] = accum->tools[i].name;
						tinputs[i] = accum->tools[i].input_json.data;
						ba_render_tool_call(&tctx, accum->tools[i].id,
								    accum->tools[i].name,
								    accum->tools[i].input_json.data);
					}
					store_conv_add_assistant(paths.conversation,
								 accum->thinking.len ? accum->thinking.data : NULL,
								 accum->text.len ? accum->text.data : NULL,
								 accum->tool_count, tids, tnames, tinputs);
					free(tids); free(tnames); free(tinputs);
				}
				/* execute tools */
				ids = xzalloc(accum->tool_count * sizeof(char *));
				contents = xzalloc(accum->tool_count * sizeof(char *));
				for (i = 0; i < accum->tool_count; i++) {
					char *out = ba_tool_execute(accum->tools[i].name,
								    accum->tools[i].input_json.data,
								    120000);
					ba_render_tool_result(&tctx, accum->tools[i].id,
							      accum->tools[i].name, out);
					ids[i] = accum->tools[i].id;
					contents[i] = out;
				}
				store_conv_add_tool_results(paths.conversation,
							    accum->tool_count, ids, contents);
				for (i = 0; i < accum->tool_count; i++)
					free((void *)contents[i]);
				free(ids); free(contents);
				sse_accum_free(accum);
				continue;   /* next model turn */
			}

			/* final assistant message */
			store_conv_add_assistant(paths.conversation,
						 accum->thinking.len ? accum->thinking.data : NULL,
						 accum->text.len ? accum->text.data : NULL,
						 0, NULL, NULL, NULL);
			ba_render_stop(&tctx, accum->stop_reason ? accum->stop_reason : "end_turn");
			if (!tctx.output_json)
				fwrite("\n", 1, 1, stdout);
			sse_accum_free(accum);
			break;
		}
	}

out:
	store_session_paths_free(&paths);
	free(tools_json);
	free(session_id);
	free(cwd);
	free(home);
	free(api_url);
	free(prompt);
	return rc;
}
