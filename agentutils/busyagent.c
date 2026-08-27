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
//config:	select FEATURE_EDITING
//config:	help
//config:	Run exactly one user turn of an LLM agent loop: build a chat
//config:	request, stream the reply over plain HTTP (SSE), execute any
//config:	tool calls with busybox applets, feed results back until the
//config:	model finishes, then exit. Session history is stored under
//config:	$BB_AGENT_HOME and restored automatically per working directory.
//config:
//applet:IF_BUSYAGENT(APPLET(busyagent, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_BUSYAGENT) += busyagent.o ba_impl.o

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
#include <sys/utsname.h>
#include <dirent.h>
#include "busyagent.h"

#define BA_MAX_TOKENS     16384
#define BA_DEFAULT_TURNS  8
#define BA_CTX_LIMIT_BYTES (512 * 1024)

/* ---- per-turn context: accumulator + renderer + trace ---- */

typedef struct {
	SseAccumulator accum;
	BaDisplay disp;
	const SessionPaths *paths;
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
/* display_msg_to_event：与 bash-agent 逐字一致的事件序列化 */
static char *ba_display_to_event(const BaDisplayMsg *m)
{
	StrBuf buf;
	sb_init(&buf);
	switch (m->type) {
	case BA_DM_TEXT:
		sb_append(&buf, "{\"type\":\"text\",\"content\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_THINKING:
		sb_append(&buf, "{\"type\":\"thinking\",\"content\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_TOOL_CALL:
		sb_append(&buf, "{\"type\":\"tool_call\",\"name\":");
		sb_append_json_string(&buf, m->tool_name ? m->tool_name : "");
		sb_append(&buf, ",\"id\":");
		sb_append_json_string(&buf, m->tool_id ? m->tool_id : "");
		sb_append(&buf, ",\"input\":");
		if (m->tool_input) sb_append(&buf, m->tool_input); else sb_append(&buf, "{}");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_TOOL_RESULT:
		sb_append(&buf, "{\"type\":\"tool_result\",\"tool_use_id\":");
		sb_append_json_string(&buf, m->tool_id ? m->tool_id : "");
		sb_append(&buf, ",\"name\":");
		sb_append_json_string(&buf, m->tool_name ? m->tool_name : "");
		sb_append(&buf, ",\"content\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_USAGE:
		sb_appendf(&buf,
			"{\"type\":\"usage\",\"input_tokens\":%d,\"output_tokens\":%d,"
			"\"cache_read_input_tokens\":%d,\"cache_creation_input_tokens\":%d,"
			"\"kind\":\"agent\"}",
			m->in_tokens, m->out_tokens,
			m->cache_read_tokens, m->cache_creation_tokens);
		break;
	case BA_DM_STOP:
		sb_append(&buf, "{\"type\":\"stop\",\"reason\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_ERROR:
		sb_append(&buf, "{\"type\":\"error\",\"message\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	default:
		sb_free(&buf);
		return NULL;
	}
	return buf.data;
}

/* push_display_event 同步版：写 events.jsonl + 按 format 渲染 */
static void ba_push_display(TurnCtx *t, const BaDisplayMsg *m)
{
	char *evt = ba_display_to_event(m);
	if (evt) {
		store_event_append(t->paths, evt);
		free(evt);
	}
	ba_display_push(&t->disp, m);
}

/* 工具轮结束后的聚合补推：json 模式整块 text/thinking + usage */
static void ba_flush_turn_events(TurnCtx *t)
{
	if (t->disp.format == BA_FMT_STREAM_JSON) {
		BaDisplayMsg dm;
		memset(&dm, 0, sizeof(dm));
		if (t->accum.thinking.len > 0) {
			dm.type = BA_DM_THINKING;
			dm.content = t->accum.thinking.data;
			ba_push_display(t, &dm);
		}
		memset(&dm, 0, sizeof(dm));
		if (t->accum.text.len > 0) {
			dm.type = BA_DM_TEXT;
			dm.content = t->accum.text.data;
			ba_push_display(t, &dm);
		}
	}
	if (t->accum.in_tokens || t->accum.out_tokens)
		ba_push_display(t, &(BaDisplayMsg){ .type = BA_DM_USAGE,
			.in_tokens = t->accum.in_tokens,
			.out_tokens = t->accum.out_tokens,
			.cache_read_tokens = t->accum.cache_read_tokens,
			.cache_creation_tokens = t->accum.cache_creation_tokens });
}

/* SSE 回调：对齐 bash-agent stream_display_callback —— 事件即时推送 */
static void ba_sse_callback(void *ctx, const SseEvent *evt)
{
	TurnCtx *t = (TurnCtx *)ctx;
	BaDisplayMsg dm;

	memset(&dm, 0, sizeof(dm));
	if (evt->type == SSE_TEXT) {
		dm.type = BA_DM_TEXT;
		dm.content = (char *)evt->content;
		ba_push_display(t, &dm);
	} else if (evt->type == SSE_ERROR) {
		dm.type = BA_DM_ERROR;
		dm.content = (char *)(evt->content ? evt->content : "unknown error");
		ba_push_display(t, &dm);
	}
	sse_accum_callback(&t->accum, evt);
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
/* ---- 会话运行上下文（等价 bash-agent 的 Agent 结构体传参方式；
 * 进程内 SubAgent 与 main 共用同一入口，无全局态、无私有 env 协议） ---- */
/* ---- background bash：派生/收割注册表
 * 语义对齐 async_bash_thread_fn：mkstemp 接输出，完成后
 * 主动注入 exit_code + output（不需要模型回读） ---- */
typedef struct {
	char task_id[80];
	pid_t pid;
	char tmpfile[40];
	int active;
} BaBgTask;

#define BA_BG_MAX 16
static BaBgTask g_bg[BA_BG_MAX];

static char *ba_spawn_background(const char *cmd)
{
	int i;
	for (i = 0; i < BA_BG_MAX; i++)
		if (!g_bg[i].active)
			break;
	if (i == BA_BG_MAX)
		return xstrdup("Error: too many active background tasks");

	{
		BaBgTask *t = &g_bg[i];
		int tmpfd;
		pid_t pid;

		snprintf(t->tmpfile, sizeof(t->tmpfile), "/tmp/ba_async_bash_XXXXXX");
		tmpfd = mkstemp(t->tmpfile);
		if (tmpfd < 0)
			return xstrdup("Error: failed to create temp file for background command");
		{
			char *nid = session_new_id();
			snprintf(t->task_id, sizeof(t->task_id), "task_%s", nid);
			free(nid);
		}
		pid = fork();
		if (pid < 0) {
			close(tmpfd);
			unlink(t->tmpfile);
			t->active = 0;
			return xstrdup("Error: failed to start background task");
		}
		if (pid == 0) {
			setpgid(0, 0);
			close(STDIN_FILENO);
			xopen("/dev/null", O_RDONLY);
			xmove_fd(tmpfd, STDOUT_FILENO);
			xmove_fd(dup(STDOUT_FILENO), STDERR_FILENO);
			execl(bb_busybox_exec_path, "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		setpgid(pid, pid);
		close(tmpfd);           /* 父侧不 waitpid：完成由收割器拉取 */
		pid = t->pid = pid;
		t->active = 1;
	}

	{
		StrBuf r;
		sb_init(&r);
		sb_appendf(&r, "Async task started: task_id=%s", g_bg[i].task_id);
		return r.data;
	}
}

typedef struct {
	const char *home, *cwd;
	char *prompt;
	char *session_id;
	int depth;              /* bash-agent: sub_agent_depth */
	int verbose;
	int max_turns;
	const char *model, *provider, *api_url, *api_key;
	BaDisplayFormat fmt;
	SessionPaths paths;
} BaRunCtx;

static int ba_run_session(BaRunCtx *ctx);

/* ---- resume 回放（bash-agent agent.c:508-660 对齐）----
 * 从 events.jsonl 最近 max_turns 个 user_input 起逐事件重放到 display */
static long ba_find_recent_turn_start(const char *events_path, int max_turns)
{
	FILE *f = fopen(events_path, "r");
	long *offsets;
	char *line = NULL;
	size_t cap = 0;
	int seen = 0;
	long start = -1;

	if (!f)
		return -1;
	if (max_turns <= 0)
		max_turns = 10;
	offsets = xzalloc((size_t)max_turns * sizeof(long));
	for (;;) {
		long pos = ftell(f);
		ssize_t n = getline(&line, &cap, f);

		if (n < 0)
			break;
		if (strstr(line, "\"type\":\"user_input\"")) {
			offsets[seen % max_turns] = pos;
			seen++;
		}
	}
	fclose(f);
	free(line);
	if (seen > 0)
		start = (seen >= max_turns) ? offsets[seen % max_turns]
		                            : offsets[0];
	free(offsets);
	return start;
}

static int ba_replay_events(const SessionPaths *paths, const BaDisplay *disp,
			            int max_turns)
{
	long start;
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	int replayed = 0;

	start = ba_find_recent_turn_start(paths->events, max_turns);
	if (start < 0 || disp->format != BA_FMT_HUMAN)
		return 0;
	f = fopen(paths->events, "r");
	if (!f)
		return 0;
	if (fseek(f, start, SEEK_SET) != 0) {
		fclose(f);
		return 0;
	}

	while (getline(&line, &cap, f) >= 0) {
		JsonParse jp;
		char *type;

		jp = json_parse_root(line);
		if (jp.error)
			continue;
		type = json_get_string(jp.val, "type");
		if (!type)
			continue;

		if (strcmp(type, "user_input") == 0) {
			char *content = json_get_string(jp.val, "content");
			if (content && content[0]) {
				StrBuf u;
				sb_init(&u);
				util_truncate_str(content, 80);
				{ char *nl = strchr(content, '\n');
				  if (nl) *nl = '\0'; }
				sb_appendf(&u, "\n\x1b[32m> %s\x1b[0m\n", content);
				ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
					.type = BA_DM_TEXT, .content = u.data});
				sb_free(&u);
				replayed = 1;
			}
			free(content);
		} else if (strcmp(type, "text") == 0) {
			char *content = json_get_string(jp.val, "content");
			if (content && content[0])
				replayed |= ba_display_push((BaDisplay *)disp,
					&(BaDisplayMsg){.type = BA_DM_TEXT,
						.content = content}) != NULL;
			free(content);
		} else if (strcmp(type, "thinking") == 0) {
			char *content = json_get_string(jp.val, "content");
			if (content && content[0])
				replayed |= ba_display_push((BaDisplay *)disp,
					&(BaDisplayMsg){.type = BA_DM_THINKING,
						.content = content}) != NULL;
			free(content);
		} else if (strcmp(type, "tool_call") == 0) {
			char *name = json_get_string(jp.val, "name");
			char *id = json_get_string(jp.val, "id");
			char *input_str = json_get_string(jp.val, "input_json");

			if (!input_str)
				input_str = json_as_string(json_get(jp.val, "input"));
			{
				char *sum = input_str
					? ba_tool_call_summary(name ? name : "", input_str)
					: xstrdup("");
				ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
					.type = BA_DM_TOOL_CALL,
					.tool_name = name, .tool_id = id,
					.tool_input = input_str, .content = sum});
				free(sum);
			}
			replayed = 1;
			free(name); free(id); free(input_str);
		} else if (strcmp(type, "tool_result") == 0) {
			char *content = json_get_string(jp.val, "content");
			if (content)
				util_truncate_str(content, 200);
			ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
				.type = BA_DM_TOOL_RESULT, .content = content,
				.tool_id = json_get_string(jp.val, "tool_use_id"),
				.tool_name = json_get_string(jp.val, "name")});
			free(content);
			replayed = 1;
		} else if (strcmp(type, "error") == 0) {
			char *message = json_get_string(jp.val, "message");
			ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
				.type = BA_DM_ERROR, .content = message});
			free(message);
			replayed = 1;
		} else if (strcmp(type, "sub_agent_result") == 0) {
			/* agent.c:638-658：thinking 截 120、text 截 200 */
			char *sid2 = json_get_string(jp.val, "session_id");
			char *status = json_get_string(jp.val, "status");
			char *thinking = json_get_string(jp.val, "thinking");
			char *text = json_get_string(jp.val, "text");
			int in_tok = json_get_int(jp.val, "input_tokens");
			int out_tok = json_get_int(jp.val, "output_tokens");

			if (thinking)
				thinking[util_utf8_truncate_len(thinking, 120)] = '\0';
			if (text)
				util_truncate_str(text, 200);
			ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
				.type = BA_DM_SUB_AGENT_RESULT,
				.session_id = sid2,
				.tool_exit_code = (status && strcmp(status, "ok") == 0) ? 0 : 1,
				.tool_name = thinking,   /* bash-agent 复用该字段装 thinking */
				.content = text,
				.in_tokens = in_tok, .out_tokens = out_tok});
			free(sid2); free(status); free(thinking); free(text);
			replayed = 1;
		} else if (strcmp(type, "async_task_result") == 0) {
			char *tid = json_get_string(jp.val, "task_id");
			int exit_code = json_get_int(jp.val, "exit_code");
			char *output = json_get_string(jp.val, "output");

			ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
				.type = BA_DM_ASYNC_TASK_RESULT,
				.session_id = tid,
				.tool_exit_code = exit_code,
				.content = output});
			free(tid); free(output);
			replayed = 1;
		}
		free(type);
	}
	fclose(f);
	free(line);
	return replayed;
}


static void ba_handle_async_result(const BaDisplay *disp,
                                   SessionPaths *paths, const char *tid,
                                   int exit_code, const char *output)
{
	{   /* trace 事件（字段同 bash-agent agent.c:716-721） */
		StrBuf evt;
		sb_init(&evt);
		sb_appendf(&evt, "{\"type\":\"async_task_result\",\"task_id\":\"%s\",\"exit_code\":%d,\"output\":",
				   tid, exit_code);
		sb_append_json_string(&evt, output ? output : "");
		sb_append_char(&evt, '}');
		store_event_append(paths, evt.data);
		sb_free(&evt);
	}
	if (disp->format != BA_FMT_NONE) {   /* display: 统一 push */
			ba_display_push((BaDisplay *)disp, &(BaDisplayMsg){
				.type = BA_DM_ASYNC_TASK_RESULT,
				.session_id = (char *)tid,
				.tool_exit_code = exit_code,
				.content = (char *)(output ? output : "")});
	}
	{   /* 会话注入 user 角色（agent.c:730-735）：驱动下一轮 model turn */
		StrBuf u;
		sb_init(&u);
		sb_appendf(&u, "[bg-bash %s] exit_code=%d\nOutput: %s",
			   tid, exit_code, output ? output : "");
		store_conv_add_user(paths->conversation, u.data);
		sb_free(&u);
	}
}

/* drain 已完成任务：block=1 时阻塞等待（bash-agent
 * 非交互 drain 循环 agent.c:1513 同款语义） */
static void ba_drain_background(BaRunCtx *ctx, SessionPaths *paths,
                                BaDisplayFormat fmt, int block)
{
	(void)ctx;
	for (;;) {
		int reaped = 0;
		int i;
		for (i = 0; i < 16; i++) {
			if (!g_bg[i].active)
				continue;
			{
				int st = 0;
				pid_t r = waitpid(g_bg[i].pid, &st, block ? 0 : WNOHANG);
				if (r == g_bg[i].pid) {
					int code = WIFEXITED(st) ? WEXITSTATUS(st)
					         : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
					char *out = NULL;
					FILE *f2 = fopen(g_bg[i].tmpfile, "r");
					long fsz;
					BaDisplay dtmp;
					if (f2) {
						fseek(f2, 0, SEEK_END);
						fsz = ftell(f2);
						fseek(f2, 0, SEEK_SET);
						out = xmalloc(fsz + 1);
						out[fread(out, 1, fsz, f2)] = '\0';
						fclose(f2);
					}
					remove(g_bg[i].tmpfile);
					memset(&dtmp, 0, sizeof(dtmp));
					dtmp.format = fmt;
					dtmp.out = stdout;
					ba_handle_async_result(&dtmp, paths,
								       g_bg[i].task_id, code, out);
					free(out);
					g_bg[i].active = 0;
					reaped = 1;
				}
			}
		}
		if (!reaped)
			break;
		block = 0;   /* 剩下的只收已就绪的 */
	}
}

static int ba_bg_active_count(void)
{
	int i, n = 0;
	for (i = 0; i < 16; i++)
		if (g_bg[i].active)
			n++;
	return n;
}

/* 裁掉 conversation 尾部未应答的 tool_use assistant
 * （fork 复制发生在 tool_result 落库前；openai 对悬空 tool_calls 400） */
static void ba_trim_trailing_tool_use(const char *conv_path)
{
	char **lines = NULL;
	int n = 0, i, cut = 0;

	if (store_conv_line_count(conv_path, &lines, &n) != 0)
		return;
	for (i = n - 1; i >= 0; i--) {
		JsonParse jp = json_parse_root(lines[i]);
		char *role = jp.error ? NULL : json_get_string(jp.val, "role");
		int is_assistant = (role && strcmp(role, "assistant") == 0);
		free(role);
		if (!is_assistant)
			break;
		{
			JsonVal content = json_get(jp.val, "content");
			int has_tu = 0;
			if (content.type == JSON_ARRAY) {
				int j, jl = json_array_len(content);
				for (j = 0; j < jl && !has_tu; j++) {
					JsonVal blk = json_array_get(content, j);
					char *t = json_get_string(blk, "type");
					has_tu = (t && strcmp(t, "tool_use") == 0);
					free(t);
				}
			}
			if (!has_tu)
				break;          /* 尾部 assistant 无 tool_use：干净 */
			cut = 1;            /* 悬空 tool_use：删掉这一行 */
			break;
		}
	}
	if (cut > 0 && n >= cut) {
		/* 重写文件去掉最后 cut 行（trim_tail 语义是保留尾 N 行，方向相反） */
		FILE *f = fopen(conv_path, "w");
		if (f) {
			for (i = 0; i < n - cut; i++)
				fprintf(f, "%s\n", lines[i]);
			fclose(f);
		}
	}
	for (i = 0; i < n; i++)
		free(lines[i]);
	free(lines);
}

static char *ba_handle_sub_agent(BaRunCtx *parent, const char *prompt,
                                 const char *description, int fork_mode)
{
	char *raw_id, *sub_session_id;
	BaRunCtx sub;
	int rc;

	if (!prompt || !prompt[0])
		return xstrdup("Error: no prompt provided");
	if (parent->depth >= 1)
		return xstrdup("Error: sub-agent recursion limit reached; child agents cannot launch SubAgent");

	raw_id = session_new_id();
	sub_session_id = xasprintf("sub_%s", raw_id);
	free(raw_id);

	/* 记录 sub_agent_start 事件（字段照抄 bash-agent） */
	{
		StrBuf evt;
		sb_init(&evt);
		sb_appendf(&evt, "{\"type\":\"sub_agent_start\",\"session_id\":");
		sb_append_json_string(&evt, sub_session_id);
		sb_appendf(&evt, ",\"prompt\":");
		sb_append_json_string(&evt, prompt);
		sb_appendf(&evt, ",\"description\":");
		sb_append_json_string(&evt, description ? description : "");
		sb_appendf(&evt, ",\"fork\":%s}", fork_mode ? "true" : "false");
		store_event_append(&parent->paths, evt.data);
		sb_free(&evt);
	}

	memset(&sub, 0, sizeof(sub));
	sub.home = parent->home;
	sub.cwd = parent->cwd;
	sub.prompt = (char *)prompt;
	sub.session_id = sub_session_id;
	sub.depth = parent->depth + 1;
	sub.verbose = parent->verbose;         /* diag: temp inherit; bash-agent forces 0 */
	sub.max_turns = parent->max_turns;
	sub.model = parent->model;
	sub.provider = parent->provider;
	sub.api_url = parent->api_url;
	sub.api_key = parent->api_key;
	sub.fmt = BA_FMT_NONE;                 /* 无队列架构下的静默等价 */

	/* fork=true：发起侧立即复制父会话（agent.c:1812 同款）。
	 * 复制发生在父的 tool_result 落库前，尾部可能挂着未应答的
	 * tool_use assistant；openai 上游对此 400，须裁到完整交换为止 */
	if (fork_mode) {
		SessionPaths sub_paths = store_session_paths_for(parent->home,
		                                                 parent->cwd,
		                                                 sub_session_id);
		store_session_init_sub(&parent->paths, &sub_paths, 1);
		ba_trim_trailing_tool_use(sub_paths.conversation);
		store_session_paths_free(&sub_paths);
	}

	rc = ba_run_session(&sub);

	/* 提取最后一条 assistant 消息的 text（agent.c:1693 同款倒序扫描） */
	{
		char **lines = NULL;
		int line_count = 0, li;
		char *result_text = NULL;

		store_conv_line_count(sub.paths.conversation, &lines, &line_count);
		for (li = line_count - 1; li >= 0 && !result_text; li--) {
			JsonParse jp = json_parse_root(lines[li]);
			if (jp.error)
				continue;
			{
				char *role = json_get_string(jp.val, "role");
				if (role && strcmp(role, "assistant") == 0) {
					JsonVal content = json_get(jp.val, "content");
					if (content.type == JSON_ARRAY) {
						int clen = json_array_len(content), j;
						for (j = clen - 1; j >= 0 && !result_text; j--) {
							JsonVal block = json_array_get(content, j);
							char *btype = json_get_string(block, "type");
							if (btype && strcmp(btype, "text") == 0)
								result_text = json_get_string(block, "text");
							free(btype);
						}
					}
				}
				free(role);
			}
		}
		for (li = 0; li < line_count; li++)
			free(lines[li]);
		free(lines);

		/* sub_agent_result 事件（供 replay/stream-json 复现） */
		{
			StrBuf evt;
			sb_init(&evt);
			sb_appendf(&evt, "{\"type\":\"sub_agent_result\",\"session_id\":");
			sb_append_json_string(&evt, sub_session_id);
			sb_appendf(&evt, ",\"status\":\"%s\"", rc == 0 ? "ok" : "failed");
			sb_append(&evt, ",\"thinking\":\"\"");
			sb_append(&evt, ",\"text\":");
			sb_append_json_string(&evt, result_text ? result_text : "");
			sb_append_char(&evt, '}');
			store_event_append(&parent->paths, evt.data);
			sb_free(&evt);
		}
		return result_text ? result_text
		                   : xstrdup(rc == 0 ? "(no text)"
		                                     : "Error: sub-agent failed");
	}
}

/* ---- 会话运行体：等价 bash-agent 的 agent_loop(Agent*, prompt, role) ---- */
static int ba_run_session(BaRunCtx *ctx)
{
	SessionPaths paths;
	const char *model = ctx->model;
	const char *provider = ctx->provider;
	const char *api_url = ctx->api_url;
	const char *api_key = ctx->api_key;
	const char *prompt = ctx->prompt;
	int verbose = ctx->verbose;
	int max_turns = ctx->max_turns > 0 ? ctx->max_turns : BA_DEFAULT_TURNS;
	int rc = 0;
	char *tools_json = NULL;
	char *sys_full = NULL;

	{
		int is_new;
		paths = store_session_paths_for(ctx->home, ctx->cwd, ctx->session_id);
		is_new = (access(paths.session_dir, F_OK) != 0);
		if (store_session_init(&paths, is_new) != 0)
			bb_error_msg_and_die("session init failed: %s", paths.session_dir);
		ctx->paths = paths;
		if (verbose)
			fprintf(stderr, "[verbose] session=%s (%s) home=%s\n",
				ctx->session_id, is_new ? "new" : "resumed", ctx->home);
	}

	ba_tools_set_paths(&paths);

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

	/* system prompt：对齐 bash-agent 的分块结构（ba_prompt.c） */
	{
		BaPromptCtx pctx;
		int turn;
		memset(&pctx, 0, sizeof(pctx));
		pctx.cwd = ctx->cwd;
		pctx.home = ctx->home;
		pctx.plan = paths.plan;
		pctx.plan_draft = paths.plan_draft;
		sys_full = ba_build_prompt(&pctx);
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

			/* 循环顶收割：上一轮期间完成的后台任务先注入
			 * 再构造请求（对齐 bash-agent 的队列回注时点） */
			ba_drain_background(ctx, &paths, ctx->fmt, 0);

			ba_trim_history(paths.conversation);

			if (store_conv_line_count(paths.conversation, &lines, &line_count) != 0)
				bb_error_msg_and_die("conversation read failed");

			claude_body = build_claude_request(model, sys_full, tools_json,
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
			tctx.verbose = verbose;
			ba_disp_init(&tctx.disp, ctx->fmt);
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
					ba_push_display(&tctx, &(BaDisplayMsg){
						.type = BA_DM_ERROR, .content = (char *)"HTTP request failed"});
				ba_push_display(&tctx, &(BaDisplayMsg){
					.type = BA_DM_STOP, .content = (char *)"error"});
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
						{
							char *sum = ba_tool_call_summary(
								accum->tools[i].name,
								accum->tools[i].input_json.data);
							BaDisplayMsg dm;
							memset(&dm, 0, sizeof(dm));
							dm.type = BA_DM_TOOL_CALL;
							dm.tool_name = accum->tools[i].name;
							dm.tool_id = accum->tools[i].id;
							dm.tool_input = accum->tools[i].input_json.data;
							dm.content = sum;
							ba_push_display(&tctx, &dm);
							free(sum);
						}
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
					char *out;
					int handled = 0;

					if (strcmp(accum->tools[i].name, "Bash") == 0) {
						/* bash-agent agent.c:1075 同款：Bash
						 * background=true 在 loop 层截获 */
						JsonParse bjp = json_parse_root(
							accum->tools[i].input_json.data);
						char *cmd = NULL;
						int is_async = 0;
						if (!bjp.error) {
							is_async = json_get_bool(bjp.val, "background", false);
							cmd = json_get_string(bjp.val, "command");
						}
						if (is_async) {
							out = (cmd && cmd[0])
							      ? ba_spawn_background(cmd)
							      : xstrdup("Error: no command provided");
							handled = 1;
						}
						free(cmd);
					} else if (strcmp(accum->tools[i].name, "SubAgent") == 0) {
						/* bash-agent agent.c:1050 同款：loop 层拦截 SubAgent */
						JsonParse sjp = json_parse_root(
							accum->tools[i].input_json.data);
						char *sprompt = NULL, *sdesc = NULL;
						int sfork = 0;
						if (!sjp.error) {
							sprompt = json_get_string(sjp.val, "prompt");
							sdesc = json_get_string(sjp.val, "description");
							sfork = json_get_bool(sjp.val, "fork", false) ? 1 : 0;
						}
						out = ba_handle_sub_agent(ctx,
									 sprompt ? sprompt : "",
									 sdesc ? sdesc : "", sfork);
						free(sprompt); free(sdesc);
						handled = 1;
					}

					if (!handled)
						out = ba_tool_execute(accum->tools[i].name,
								      accum->tools[i].input_json.data,
								      120000);

					{
						BaDisplayMsg dm;
						memset(&dm, 0, sizeof(dm));
						dm.type = BA_DM_TOOL_RESULT;
						dm.tool_id = accum->tools[i].id;
						dm.tool_name = accum->tools[i].name;
						dm.content = out;
						ba_push_display(&tctx, &dm);
					}
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
			ba_push_display(&tctx, &(BaDisplayMsg){
				.type = BA_DM_STOP,
				.content = accum->stop_reason ? accum->stop_reason : "end_turn"});
			/* 换行由 STOP 渲染的 ensure_newline 条件补：
			 * 模型回答本身以 \n 结尾时不再多打（bash-agent
			 * display.c DISPLAY_STOP 同款） */
			sse_accum_free(accum);

			/* end_turn 但仍有后台任务：阻塞收割并以注入结果
			 * 继续新轮（bash-agent 非交互 drain → run_loop 同款） */
			if (ba_bg_active_count() > 0 && turn < max_turns - 1) {
				ba_drain_background(ctx, &paths, ctx->fmt, 1);
				sse_accum_init(&tctx.accum);
				continue;   /* 注入的 [bg-bash] user 消息驱动新回合 */
			}
			break;
		}
	}

out:
	ctx->paths = paths;
	store_session_paths_free(&paths);
	free(tools_json);
	free(sys_full);
	return rc;
}

int busyagent_main(int argc UNUSED_PARAM, char **argv)
{
	const char *base_url = getenv("BB_AGENT_BASE_URL");
	const char *api_key  = getenv("BB_AGENT_API_KEY");
	const char *home_env = getenv("BB_AGENT_HOME");
	const char *model_env = getenv("BB_AGENT_MODEL");
	const char *provider_env = getenv("BB_AGENT_PROVIDER");
	const char *output_env = getenv("BB_AGENT_OUTPUT");
	const char *model = (model_env && model_env[0]) ? model_env : NULL;
	const char *provider = (provider_env && provider_env[0]) ? provider_env : "openai";
	const char *session_arg = NULL;
	const char *output_fmt = (output_env && output_env[0]) ? output_env : "text";
	int max_turns = BA_DEFAULT_TURNS, verbose = 0;
	int opt_new = 0;
	char *prompt = NULL, *home, *cwd, *session_id = NULL;
	char *api_url = NULL;
	int rc = 0;
	unsigned opts;
	const char *o_u = NULL, *o_k = NULL, *o_m = NULL, *o_p = NULL;
	const char *o_t = NULL, *o_s = NULL, *o_o = NULL;
	/* bit positions follow the option string "vu:k:m:p:t:s:o:nci" */
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

	{
		if (home_env && home_env[0]) {
			home = xstrdup(home_env);
		} else {
			/* 持久会话记忆需要持久位置：$HOME/.busyagent */
			const char *h = getenv("HOME");
			home = (h && h[0]) ? xasprintf("%s/.busyagent", h) : xstrdup("/tmp/busyagent");
		}
	}
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
		signal(SIGPIPE, SIG_IGN);   /* cagent.c 同款：HTTP/pipe 写入半关闭时不致死于默认行为 */
	}
	if (!argv[0]) {
		if (isatty(STDIN_FILENO)) {
			/* ---- 交互式 REPL（bash-agent cagent 交互模式对齐）----
			 * 行编辑用 busybox lineedit（read_line_input），
			 * 能力等价 linenoise：历史/光标/常用快捷键；
			 * 多行粘贴、剪贴板图片注入延后（linenoise 特性） */
			char line[8192];
			BaRunCtx repl;
			/* 行编辑 state：运行期上下键历史；持久化自管
			 * （hist_file 置空关闭 lineedit 的内建读写，
			 * 避免与手写 append 双写交错）：history 文件
			 * 每行一条、启动回灌 —— 角色对齐 bash-agent 的
			 * $HOME/.bash-agent/history（HistoryLoad/Save） */
			line_input_t *li;
			char *hist_path;

			hist_path = xasprintf("%s/history", home);
			mkdir(home, 0755);
			li = new_line_input_t(DO_HISTORY);
			{
				FILE *hf0 = fopen(hist_path, "r");
				if (hf0) {
					char hbuf[8192];
					while (li->cnt_history < li->max_history
					    && fgets(hbuf, sizeof(hbuf), hf0)) {
						size_t hl = strlen(hbuf);
						if (hl && hbuf[hl-1] == '\n')
							hbuf[--hl] = '\0';
						if (hl)
							li->history[li->cnt_history++] =
								xstrdup(hbuf);
					}
					fclose(hf0);
					li->cur_history = li->cnt_history;
				}
			}

			fprintf(stderr, "busyagent ready (interactive). "
					"Type 'quit' or Ctrl-D to exit.\n");
			/* resumed 会话：先回放最近 10 轮事件（cagent.c:350-355 同款） */
			{
				SessionPaths rp = store_session_paths_for(home, cwd, session_id);
				if (access(rp.session_dir, F_OK) == 0) {
					BaDisplay rd;
					memset(&rd, 0, sizeof(rd));
					rd.format = repl.fmt;
					rd.out = stdout;
					if (ba_replay_events(&rp, &rd, 10))
						fwrite("\n", 1, 1, stdout);
				}
				store_session_paths_free(&rp);
			}
			memset(&repl, 0, sizeof(repl));
			repl.home = home;
			repl.cwd = cwd;
			repl.session_id = session_id;
			repl.depth = 0;
			repl.verbose = verbose;
			repl.max_turns = max_turns;
			repl.model = model;
			repl.provider = provider;
			repl.api_url = api_url;
			repl.api_key = api_key;
			repl.fmt = (strcmp(output_fmt, "json") == 0)
			           ? BA_FMT_STREAM_JSON : BA_FMT_HUMAN;

			while (1) {
				int rl = read_line_input(li, "\x1b[32m> \x1b[0m",
							 line, sizeof(line));
				if (rl <= 0)
					break;   /* EOF / Ctrl-D */
				trim(line);
				if (!line[0])
					continue;
				if (!strcmp(line, "quit") || !strcmp(line, "exit"))
					break;
				{   /* 行级持久化（bash-agent 每行 HistoryAdd+Save
				     * 同款语义）；quit/空行不入历史 */
					int hf = open(hist_path,
						O_WRONLY | O_CREAT | O_APPEND,
						0600);
					if (hf >= 0) {
						full_write(hf, line,
							   strlen(line));
						full_write(hf, "\n", 1);
						close(hf);
					}
				}
				/* 已完成的后台任务先收割展示（交互模式不阻塞：
				 * bash-agent input-loop 注释同款语义）。
				 * 首次输入前 repl.paths 未建，run_session
				 * 循环顶收割点负责首轮；此后每次进入前
				 * 先展示期间完成项 */
				if (repl.paths.session_dir)
					ba_drain_background(&repl, &repl.paths,
							    repl.fmt, 0);
				repl.prompt = xstrdup(line);
				rc = ba_run_session(&repl);
				free(repl.prompt);
				session_id = repl.session_id; /* run 后 sid 稳定 */
			}
			if (repl.paths.session_dir)
				ba_drain_background(&repl, &repl.paths,
						    repl.fmt, 1);
			/* 退出提示（cagent.c:399-400 同款：Goodbye + resume 提示） */
			fprintf(stderr, "\x1b[36mGoodbye!\x1b[0m\n");
			fprintf(stderr,
				"\x1b[90mResume with: --session %s  or  --continue\x1b[0m\n",
				session_id ? session_id : "?");
			free_line_input_t(li);
			free(hist_path);
			free(api_url);
			free(session_id);
			free(cwd);
			free(home);
			return rc;
		}
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


	BaRunCtx root;

	memset(&root, 0, sizeof(root));
	root.home = home;
	root.cwd = cwd;
	root.prompt = prompt;
	root.session_id = session_id;
	root.depth = 0;                 /* bash-agent: agent->sub_agent_depth */
	root.verbose = verbose;
	root.max_turns = max_turns;
	root.model = model;
	root.provider = provider;
	root.api_url = api_url;
	root.api_key = api_key;
	root.fmt = (strcmp(output_fmt, "json") == 0)
	           ? BA_FMT_STREAM_JSON : BA_FMT_HUMAN;

	rc = ba_run_session(&root);

	free(api_url);
	free(session_id);
	free(cwd);
	free(home);
	free(prompt);
	return rc;
}

