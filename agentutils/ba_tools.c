/*
 * ba_tools - table-driven tool execution for busyagent
 *
 * Tool definitions live in $BB_AGENT_HOME/tools.json — the only source,
 * never embedded in the binary. A sample lives at scripts/tools.example.json.
 * Each
 * tool carries an "exec" mapping: {"applet": "grep", "argv": ["-nH", "-e",
 * "$pattern", "$path"]}. At execution time argv templates are expanded with
 * values from the model's input JSON (missing optional values drop their
 * argument), then dispatched:
 *
 *   - applet is NOFORK  -> run_nofork_applet() in a forked child (direct
 *     in-process call, no exec: the busybox advantage)
 *   - anything else     -> fork + execl(bb_busybox_exec_path, applet, ...)
 *
 * Children run with piped stdout/stderr and are SIGKILLed on timeout.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include "libbb.h"
#include "busybox.h"   /* for APPLET_IS_NOFORK */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ba_util.h"
#include "ba_json.h"
#include "ba_transport.h"
#include "ba_tools.h"

#define BA_TOOL_TIMEOUT_MS   120000
#define BA_OUTPUT_MAX        (128 * 1024)

/* ---- tool table ---- */

typedef struct {
	char *name;            /* LLM-visible tool name */
	char *applet;          /* busybox applet to dispatch */
	char **argv_tpl;       /* argv template strings, $var placeholders */
	int argv_count;
} BaTool;

static BaTool *g_tools;
static int g_tool_count;
static char *g_tools_json_text;   /* 当前生效的 tools 数组原文 */

static char *read_file_all(const char *path)
{
	long sz;
	char *buf;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	sz = xlseek(fd, 0, SEEK_END);
	xlseek(fd, 0, SEEK_SET);
	buf = xzalloc(sz + 1);
	sz = full_read(fd, buf, sz);
	if (sz < 0)
		sz = 0;
	buf[sz] = '\0';
	close(fd);
	return buf;
}

/* Parse one tools JSON text into the global table. Returns 0 on success. */
static int ba_tools_parse(const char *json)
{
	JsonParse jp = json_parse_root(json);
	JsonVal arr, item;
	int n, i;

	if (jp.error)
		return -1;
	arr = jp.val;
	n = json_array_len(arr);
	if (n <= 0)
		return -1;

	g_tools = xzalloc(n * sizeof(BaTool));
	g_tool_count = n;

	for (i = 0; i < n; i++) {
		JsonVal exec, argv_val;
		int an, j;

		item = json_array_get(arr, i);
		g_tools[i].name = json_get_string(item, "name");
		exec = json_get(item, "exec");
		g_tools[i].applet = json_get_string(exec, "applet");
		argv_val = json_get(exec, "argv");
		an = json_array_len(argv_val);
		if (an > 0) {
			g_tools[i].argv_tpl = xzalloc(an * sizeof(char *));
			g_tools[i].argv_count = an;
			for (j = 0; j < an; j++)
				g_tools[i].argv_tpl[j] = json_string_val(json_array_get(argv_val, j));
		}
	}
	return 0;
}

/* Load tools from $BB_AGENT_HOME/tools.json. The file is the only
 * source: nothing is embedded in the binary. Missing or broken file
 * means "no tools" — plain Q&A still works, requests just omit tools. */
int ba_tools_init(const char *home)
{
	char *path = NULL;
	char *data = NULL;

	if (g_tools)
		return g_tool_count;

	if (home && home[0])
		path = xasprintf("%s/tools.json", home);
	if (path)
		data = read_file_all(path);
	if (data && ba_tools_parse(data) == 0) {
		g_tools_json_text = data;
	} else {
		if (data)
			bb_error_msg("tools.json parse failed, continuing without tools");
		else
			bb_error_msg("no %s — continuing as plain chat agent "
				     "(busyagent -i exports a starter table)", path);
		free(data);
		g_tools_json_text = NULL;
	}
	free(path);
	return g_tool_count;
}

void ba_tools_free(void)
{
	int i, j;
	for (i = 0; i < g_tool_count; i++) {
		free(g_tools[i].name);
		free(g_tools[i].applet);
		for (j = 0; j < g_tools[i].argv_count; j++)
			free(g_tools[i].argv_tpl[j]);
		free(g_tools[i].argv_tpl);
	}
	free(g_tools);
	g_tools = NULL;
	g_tool_count = 0;
	free(g_tools_json_text);
	g_tools_json_text = NULL;
}

/* span 复制：JsonVal 是 (src,start,end) 视图 */
static char *val_span_dup(const char *src, JsonVal v)
{
    size_t n = v.end - v.start;
    char *s = xmalloc(n + 1);
    memcpy(s, src + v.start, n);
    s[n] = '\0';
    return s;
}

/* 当前生效的 tools 数组（LLM 视角：剥离 busyagent 专有的 exec 字段）。
 * 无工具表时返回 NULL —— 调用方据此在请求中省略 tools 字段。 */
char *ba_tools_json(void)
{
    const char *src = g_tools_json_text;
    JsonParse jp;
    StrBuf sb;
    int n, i;

    if (!src)
        return NULL;
    jp = json_parse_root(src);
    if (jp.error)
        return util_strdup(src);
    n = json_array_len(jp.val);
    sb_init(&sb);
    sb_append_char(&sb, '[');
    for (i = 0; i < n; i++) {
        JsonVal item = json_array_get(jp.val, i);
        char *name = json_get_string(item, "name");
        char *desc = json_get_string(item, "description");
        JsonVal sch = json_get(item, "input_schema");
        char *sch_txt = val_span_dup(src, sch);
        if (i > 0) sb_append_char(&sb, ',');
        sb_append(&sb, "{\"name\":");
        sb_append_json_string(&sb, name ? name : "");
        sb_append(&sb, ",\"description\":");
        sb_append_json_string(&sb, desc ? desc : "");
        sb_append(&sb, ",\"input_schema\":");
        sb_append(&sb, sch_txt && sch_txt[0] ? sch_txt : "{}");
        sb_append_char(&sb, '}');
        free(name); free(desc); free(sch_txt);
    }
    sb_append_char(&sb, ']');
    return sb.data;
}

/* 工具表模板 — `busyagent -i [PATH]` 导出为可编辑的 tools.json。
 * 这是代码常量而非运行时数据源：运行时唯一来源是导出后的文件。 */
static const char ba_tools_template[] =
"[\n\
  {\n\
    \"name\": \"Bash\",\n\
    \"description\": \"Executes a given shell command via busybox sh and returns stdout/stderr. Use for running programs, pipelines, and anything not covered by the other tools. Output is truncated at 128KB.\",\n\
    \"exec\": {\n\
      \"applet\": \"sh\",\n\
      \"argv\": [\"-c\", \"$command\"]\n\
    },\n\
    \"input_schema\": {\n\
      \"type\": \"object\",\n\
      \"properties\": {\n\
        \"command\": {\n\
          \"type\": \"string\",\n\
          \"description\": \"The shell command to execute\"\n\
        }\n\
      },\n\
      \"required\": [\"command\"]\n\
    }\n\
  },\n\
  {\n\
    \"name\": \"Read\",\n\
    \"description\": \"Read a file from the local filesystem with the busybox cat applet.\",\n\
    \"exec\": {\n\
      \"applet\": \"cat\",\n\
      \"argv\": [\"$path\"]\n\
    },\n\
    \"input_schema\": {\n\
      \"type\": \"object\",\n\
      \"properties\": {\n\
        \"path\": {\n\
          \"type\": \"string\",\n\
          \"description\": \"Path to the file to read (absolute or relative to cwd)\"\n\
        }\n\
      },\n\
      \"required\": [\"path\"]\n\
    }\n\
  },\n\
  {\n\
    \"name\": \"Grep\",\n\
    \"description\": \"Search file contents with the busybox grep applet. Returns matching lines with line numbers. If path is omitted, searches the current directory recursively.\",\n\
    \"exec\": {\n\
      \"applet\": \"grep\",\n\
      \"argv\": [\"-nH\", \"-r\", \"-e\", \"$pattern\", \"$path\"]\n\
    },\n\
    \"input_schema\": {\n\
      \"type\": \"object\",\n\
      \"properties\": {\n\
        \"pattern\": {\n\
          \"type\": \"string\",\n\
          \"description\": \"Regular expression to search for\"\n\
        },\n\
        \"path\": {\n\
          \"type\": \"string\",\n\
          \"description\": \"File or directory to search (default: cwd, recursive)\"\n\
        }\n\
      },\n\
      \"required\": [\"pattern\"]\n\
    }\n\
  }\n\
]";

/* 导出模板到 path（默认由调用方决定，通常 $BB_AGENT_HOME/tools.json）。
 * 返回 0 成功；-1 文件已存在（不覆盖）；-2 写入失败。 */
int ba_tools_write_template(const char *path)
{
	int fd;
	char *dir;

	if (access(path, F_OK) == 0)
		return -1;
	dir = xstrdup(path);
	{
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			bb_make_directory(dir, 0755, FILEUTILS_RECUR);
		}
	}
	free(dir);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -2;
	if (full_write(fd, ba_tools_template, strlen(ba_tools_template)) < 0) {
		close(fd);
		return -2;
	}
	close(fd);
	return 0;
}

/* ---- execution ---- */

struct out_buf {
	char *data;
	size_t len;
	size_t cap;
};

static void out_append(struct out_buf *b, const char *ptr, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		while (nc < b->len + n + 1)
			nc *= 2;
		if (nc > BA_OUTPUT_MAX + 1)
			nc = BA_OUTPUT_MAX + 1;
		if (nc <= b->len)
			return;   /* full */
		b->data = xrealloc(b->data, nc);
		b->cap = nc;
	}
	if (b->len + n > BA_OUTPUT_MAX)
		n = BA_OUTPUT_MAX - b->len;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
}

/* Run one argv in a forked child with captured output.
 * NOFORK applets -> run_nofork_applet() direct call; others -> exec
 * busybox itself with the applet name. */
static int run_captured(const char *applet, char **argv,
			struct out_buf *out, struct out_buf *err,
			int timeout_ms)
{
	int pipe_out[2], pipe_err[2];
	pid_t pid;
	int applet_no = find_applet_by_name(applet);
	int nofork = (applet_no >= 0 && APPLET_IS_NOFORK(applet_no));
	int status = 0;
	int64_t deadline;
	char buf[4096];

	if (applet_no < 0 && strcmp(applet, "sh") != 0)
		return -1;   /* unknown and not sh: refuse */

	xpipe(pipe_out);
	xpipe(pipe_err);

	pid = fork();
	if (pid < 0) {
		close(pipe_out[0]); close(pipe_out[1]);
		close(pipe_err[0]); close(pipe_err[1]);
		return -1;
	}
	if (pid == 0) {
		/* child */
		signal(SIGPIPE, SIG_DFL);
		close(pipe_out[0]); close(pipe_err[0]);
		xmove_fd(xopen("/dev/null", O_RDONLY), STDIN_FILENO);
		xmove_fd(pipe_out[1], STDOUT_FILENO);
		xmove_fd(pipe_err[1], STDERR_FILENO);
		if (nofork) {
			/* NOFORK applets must never run in the parent process:
			 * they mutate global busybox state. We are forked. */
			_exit(run_nofork_applet(applet_no, argv));
		}
		BB_EXECVP(bb_busybox_exec_path, argv);
		/* argv[0] must be the applet name for busybox dispatch */
		_exit(127);
	}
	close(pipe_out[1]); close(pipe_err[1]);

	deadline = (int64_t)monotonic_ms() + timeout_ms;
	for (;;) {
		struct pollfd pfd[2];
		int nready;

		pfd[0].fd = pipe_out[0];
		pfd[0].events = POLLIN;
		pfd[1].fd = pipe_err[0];
		pfd[1].events = POLLIN;
		nready = poll(pfd, 2, 100);
		if (nready > 0) {
			if (pfd[0].revents & (POLLIN | POLLHUP)) {
				ssize_t n = read(pipe_out[0], buf, sizeof(buf));
				if (n > 0) out_append(out, buf, n);
			}
			if (pfd[1].revents & (POLLIN | POLLHUP)) {
				ssize_t n = read(pipe_err[0], buf, sizeof(buf));
				if (n > 0) out_append(err, buf, n);
			}
		}
		if (waitpid(pid, &status, WNOHANG) == pid) {
			ssize_t n;
			while ((n = read(pipe_out[0], buf, sizeof(buf))) > 0)
				out_append(out, buf, n);
			while ((n = read(pipe_err[0], buf, sizeof(buf))) > 0)
				out_append(err, buf, n);
			break;
		}
		if (monotonic_ms() > deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			status = -2;   /* timeout marker */
			break;
		}
	}
	close(pipe_out[0]); close(pipe_err[0]);
	return status;
}

static void result_wrap(StrBuf *sb, int status, struct out_buf *out,
			struct out_buf *err)
{
	if (status == -2) {
		sb_append(sb, "Error: command timed out\n");
		return;
	}
	if (status == -1) {
		sb_append(sb, "Error: failed to start command\n");
		return;
	}
	if (out->data && out->len)
		sb_append(sb, out->data);
	if (err->data && err->len) {
		sb_append(sb, "\n[stderr]\n");
		sb_append(sb, err->data);
	}
	if ((!out->data || !out->len) && (!err->data || !err->len)) {
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "(no output, exit %d)",
			 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		sb_append(sb, tmp);
	}
}

static const BaTool *find_tool(const char *name)
{
	int i;
	for (i = 0; i < g_tool_count; i++)
		if (g_tools[i].name && strcmp(g_tools[i].name, name) == 0)
			return &g_tools[i];
	return NULL;
}

/* Expand one argv template token: "$var" -> value from input JSON;
 * a token that references a missing key expands to NULL (dropped). */
static char *expand_token(const char *tpl, JsonVal input)
{
	char key[128];
	const char *p;

	if (tpl[0] != '$')
		return util_strdup(tpl);
	p = tpl + 1;
	if (p[0] == '{' && p[strlen(p) - 1] == '}') {
		/* ${name} */
		size_t n = strlen(p) - 2;
		if (n >= sizeof(key))
			return NULL;
		memcpy(key, p + 1, n);
		key[n] = '\0';
		p = key;
	}
	return json_get_string(input, p);
}

char *ba_tool_execute(const char *name, const char *input_json, int timeout_ms)
{
	StrBuf sb;
	JsonParse jp;
	const BaTool *tool;
	char **argv;
	int argc, i, ai = 0;
	struct out_buf out = { NULL, 0, 0 }, err = { NULL, 0, 0 };
	int status;

	sb_init(&sb);

	tool = find_tool(name);
	if (!tool || !tool->applet) {
		sb_appendf(&sb, "Error: tool '%s' is not in the tool table", name);
		return sb.data;
	}

	jp = json_parse_root(input_json && input_json[0] ? input_json : "{}");
	if (jp.error) {
		sb_append(&sb, "Error: invalid tool input JSON");
		return sb.data;
	}

	/* argv[0] = applet name (busybox dispatch convention) */
	argc = tool->argv_count + 1;
	argv = xzalloc(argc * sizeof(char *));
	argv[ai++] = tool->applet;
	for (i = 0; i < tool->argv_count; i++) {
		char *v = expand_token(tool->argv_tpl[i], jp.val);
		if (!v) {
			/* missing optional key: drop this argument */
			argc--;
			continue;
		}
		argv[ai++] = v;
	}
	argv[ai] = NULL;

	if (timeout_ms <= 0)
		timeout_ms = BA_TOOL_TIMEOUT_MS;
	status = run_captured(tool->applet, argv, &out, &err, timeout_ms);
	result_wrap(&sb, status, &out, &err);

	for (i = 1; i < ai; i++)
		free(argv[i]);
	free(argv);
	free(out.data);
	free(err.data);
	return sb.data;
}
