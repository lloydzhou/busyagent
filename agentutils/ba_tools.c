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
#include "ba_store.h"
#include "ba_tools.h"
#include "ba_builtin_schemas.h"

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
static const SessionPaths *g_paths;   /* 内置状态工具（Plan*）的会话落点 */

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
int ba_tools_init(const char *home, const SessionPaths *paths)
{
	char *path = NULL;
	char *data = NULL;

	if (g_tools)
		return g_tool_count;
	g_paths = paths;

	if (home && home[0])
		path = xasprintf("%s/tools.json", home);
	if (path)
		data = read_file_all(path);
	if (data && ba_tools_parse(data) == 0) {
		g_tools_json_text = data;
	} else {
		if (data)
			bb_error_msg("tools.json parse failed, dynamic zone ignored (builtins remain)");
		else
			bb_error_msg("no %s — builtin 11 tools active, dynamic zone empty "
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
/* LLM 看到的 tools 数组 = 内置 11 项 + 动态区（剥离 exec）。
 * 无动态区时仅内置（sh 锚点保证能力完备）。 */
char *ba_tools_json(void)
{
    const char *src = g_tools_json_text;
    StrBuf sb;

    if (!src)
        return util_strdup(ba_builtin_schemas);

    {
        JsonParse jp = json_parse_root(src);
        int n, i;
        if (jp.error)
            return util_strdup(ba_builtin_schemas);
        n = json_array_len(jp.val);
        sb_init(&sb);
        sb_append(&sb, ba_builtin_schemas);
        sb_truncate(&sb, sb.len - 2);   /* 去掉 "]\n" 结尾，追加动态区 */
        for (i = 0; i < n; i++) {
            JsonVal item = json_array_get(jp.val, i);
            char *nm = json_get_string(item, "name");
            /* 内置名不允许动态区覆盖 */
            if (nm && (!strcmp(nm, "Read") || !strcmp(nm, "Write") || !strcmp(nm, "Edit")
                    || !strcmp(nm, "Bash") || !strcmp(nm, "Glob") || !strcmp(nm, "Grep")
                    || !strcmp(nm, "TodoWrite") || !strcmp(nm, "PlanConfirm")
                    || !strcmp(nm, "PlanClear") || !strcmp(nm, "Skill")
                    || !strcmp(nm, "SubAgent"))) {
                bb_error_msg("tools.json: '%s' shadows a builtin, skipped", nm);
                free(nm);
                continue;
            }
            {
                char *desc = json_get_string(item, "description");
                JsonVal sch = json_get(item, "input_schema");
                char *sch_txt = (sch.type != JSON_NULL) ? val_span_dup(src, sch) : NULL;
                sb_append(&sb, ",\n  {\n    \"name\":");
                sb_append_json_string(&sb, nm ? nm : "");
                sb_append(&sb, ",\"description\":");
                sb_append_json_string(&sb, desc ? desc : "");
                sb_append(&sb, ",\"input_schema\":");
                sb_append(&sb, sch_txt && sch_txt[0] ? sch_txt : "{}");
                sb_append(&sb, "}\n");
                free(nm); free(desc); free(sch_txt);
            }
        }
        sb_append(&sb, "]\n");
    }
    return sb.data;
}

/* 动态区示例模板（-i 导出）：与内置 11 项不重叠的 POSIX 直给原语 */
static const char ba_tools_template[] =
"[\n"
"  {\n"
"    \"name\": \"ls\",\n"
"    \"description\": \"List directory contents (busybox ls).\",\n"
"    \"exec\": { \"applet\": \"ls\", \"argv\": [\"-la\", \"$path\"] },\n"
"    \"input_schema\": { \"type\": \"object\",\n"
"      \"properties\": { \"path\": { \"type\": \"string\", \"description\": \"Directory or file (default cwd)\" } } }\n"
"  },\n"
"  {\n"
"    \"name\": \"head\",\n"
"    \"description\": \"Print the first lines of a file (busybox head).\",\n"
"    \"exec\": { \"applet\": \"head\", \"argv\": [\"-n\", \"${lines}\", \"$path\"] },\n"
"    \"input_schema\": { \"type\": \"object\",\n"
"      \"properties\": { \"path\": { \"type\": \"string\", \"description\": \"File to read\" },\n"
"                      \"lines\": { \"type\": \"integer\", \"description\": \"Number of lines (default 10)\" } },\n"
"      \"required\": [\"path\"] }\n"
"  },\n"
"  {\n"
"    \"name\": \"tail\",\n"
"    \"description\": \"Print the last lines of a file (busybox tail).\",\n"
"    \"exec\": { \"applet\": \"tail\", \"argv\": [\"-n\", \"${lines}\", \"$path\"] },\n"
"    \"input_schema\": { \"type\": \"object\",\n"
"      \"properties\": { \"path\": { \"type\": \"string\", \"description\": \"File to read\" },\n"
"                      \"lines\": { \"type\": \"integer\", \"description\": \"Number of lines (default 10)\" } },\n"
"      \"required\": [\"path\"] }\n"
"  },\n"
"  {\n"
"    \"name\": \"wc\",\n"
"    \"description\": \"Count lines, words and bytes of a file (busybox wc).\",\n"
"    \"exec\": { \"applet\": \"wc\", \"argv\": [\"$path\"] },\n"
"    \"input_schema\": { \"type\": \"object\",\n"
"      \"properties\": { \"path\": { \"type\": \"string\", \"description\": \"File to count\" } },\n"
"      \"required\": [\"path\"] }\n"
"  },\n"
"  {\n"
"    \"name\": \"stat\",\n"
"    \"description\": \"Display file status (size, mode, timestamps; busybox stat).\",\n"
"    \"exec\": { \"applet\": \"stat\", \"argv\": [\"$path\"] },\n"
"    \"input_schema\": { \"type\": \"object\",\n"
"      \"properties\": { \"path\": { \"type\": \"string\", \"description\": \"File to inspect\" } },\n"
"      \"required\": [\"path\"] }\n"
"  }\n"
"]\n";

/* 导出动态区示例模板到 path。条目名不得与内置 11 项重叠
 * （运行期同样强制过滤）。返回 0 成功；-1 已存在；-2 写失败。 */
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

	jp = json_parse_root(input_json && input_json[0] ? input_json : "{}");
	if (jp.error) {
		sb_append(&sb, "Error: invalid tool input JSON");
		return sb.data;
	}

	/* ---- 内置保留名（L2/L3）：主循环语义，不走 exec 映射 ---- */

	if (strcmp(name, "Bash") == 0) {
		char *cmd = json_get_string(jp.val, "command");
		char *sh_argv[4];
		int tmo = json_get_int(jp.val, "timeout_ms");
		if (!cmd || !cmd[0]) {
			sb_append(&sb, "Error: Bash requires 'command'");
			free(cmd);
			return sb.data;
		}
		sh_argv[0] = (char *)"sh";
		sh_argv[1] = (char *)"-c";
		sh_argv[2] = cmd;
		sh_argv[3] = NULL;
		status = run_captured("sh", sh_argv, &out, &err, tmo > 0 ? tmo : timeout_ms);
		result_wrap(&sb, status, &out, &err);
		free(cmd);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "Read") == 0) {
		/* cat -n 语义：整文件或 offset/limit 分页（纯 C，无 fork） */
		char *path = json_get_string(jp.val, "path");
		int offset = json_get_int(jp.val, "offset");
		int limit = json_get_int(jp.val, "limit");
		char *data, *p;
		size_t lineno = 0, count = 0;

		if (!path || !path[0]) {
			sb_append(&sb, "Error: Read requires 'path'");
			free(path);
			return sb.data;
		}
		data = read_file_all(path);
		if (!data) {
			sb_appendf(&sb, "Error: cannot read %s", path);
			free(path);
			return sb.data;
		}
		p = data;
		while (*p) {
			char *next = strchr(p, '\n');
			int ll = next ? (int)(next - p) : (int)strlen(p);
			lineno++;
			if ((!offset || lineno >= (size_t)offset)
			 && (!limit || count < (size_t)limit)) {
				sb_appendf(&sb, "%6zu\t%.*s\n", lineno, ll, p);
				count++;
			}
			if (!next)
				break;
			p = next + 1;
		}
		if (offset && lineno < (size_t)offset)
			sb_appendf(&sb, "(file has only %zu lines)", lineno);
		free(data);
		free(path);
		return sb.data;
	}

	if (strcmp(name, "Write") == 0) {
		char *path = json_get_string(jp.val, "path");
		char *content = json_get_string(jp.val, "content");
		int fd;

		if (!path || !path[0] || !content) {
			sb_append(&sb, "Error: Write requires 'path' and 'content'");
			free(path); free(content);
			return sb.data;
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			sb_appendf(&sb, "Error: cannot write %s", path);
		} else if (full_write(fd, content, strlen(content)) < 0) {
			close(fd);
			sb_appendf(&sb, "Error: short write to %s", path);
		} else {
			close(fd);
			sb_append(&sb, "wrote ");
			sb_append(&sb, path);
		}
		free(path); free(content);
		return sb.data;
	}

	if (strcmp(name, "Edit") == 0) {
		char *path = json_get_string(jp.val, "path");
		char *old_s = json_get_string(jp.val, "old_string");
		char *new_s = json_get_string(jp.val, "new_string");
		char *data, *hit, *second;

		if (!path || !old_s || !old_s[0] || !new_s) {
			sb_append(&sb, "Error: Edit requires 'path', 'old_string', 'new_string'");
			free(path); free(old_s); free(new_s);
			return sb.data;
		}
		data = read_file_all(path);
		if (!data) {
			sb_appendf(&sb, "Error: cannot read %s", path);
			free(path); free(old_s); free(new_s);
			return sb.data;
		}
		hit = strstr(data, old_s);
		if (!hit) {
			sb_append(&sb, "Error: old_string not found in ");
			sb_append(&sb, path);
		} else {
			second = strstr(hit + 1, old_s);
			if (second) {
				sb_append(&sb, "Error: old_string occurs more than once in ");
				sb_append(&sb, path);
			} else {
				StrBuf nb;
				sb_init(&nb);
				sb_append(&nb, data);
				sb_truncate(&nb, hit - data);   /* 前段 */
				sb_append(&nb, new_s);          /* 替换文本 */
				sb_append(&nb, hit + strlen(old_s)); /* 后段 */
				{
					int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if (fd < 0) {
						sb_appendf(&sb, "Error: cannot write %s", path);
					} else if (full_write(fd, nb.data, nb.len) < 0) {
						close(fd);
						sb_appendf(&sb, "Error: short write to %s", path);
					} else {
						close(fd);
						sb_append(&sb, "edited ");
						sb_append(&sb, path);
					}
				}
				sb_free(&nb);
			}
		}
		free(data);
		free(path); free(old_s); free(new_s);
		return sb.data;
	}

	if (strcmp(name, "Glob") == 0) {
		char *pattern = json_get_string(jp.val, "pattern");
		char *gpath = json_get_string(jp.val, "path");
		char *gv[6];
		int gi = 0;

		if (!pattern || !pattern[0]) {
			sb_append(&sb, "Error: Glob requires 'pattern'");
			free(pattern); free(gpath);
			return sb.data;
		}
		gv[gi++] = (char *)"find";
		gv[gi++] = (gpath && gpath[0]) ? gpath : (char *)".";
		gv[gi++] = (char *)"-name";
		gv[gi++] = pattern;
		gv[gi] = NULL;
		status = run_captured("find", gv, &out, &err, timeout_ms);
		result_wrap(&sb, status, &out, &err);
		free(pattern); free(gpath);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "Grep") == 0) {
		char *pattern = json_get_string(jp.val, "pattern");
		char *gpath = json_get_string(jp.val, "path");
		int ctx = json_get_int(jp.val, "context");
		char ctxs[16];
		char *ctx_dup = NULL;
		char *gv[9];
		int gi = 0;

		if (!pattern || !pattern[0]) {
			sb_append(&sb, "Error: Grep requires 'pattern'");
			free(pattern); free(gpath);
			return sb.data;
		}
		gv[gi++] = (char *)"grep";
		if (ctx > 0) {
			snprintf(ctxs, sizeof(ctxs), "-%d", ctx);
			ctx_dup = xstrdup(ctxs);
			gv[gi++] = ctx_dup;
		}
		gv[gi++] = (char *)"-nH";
		gv[gi++] = (char *)"-r";
		gv[gi++] = (char *)"-e";
		gv[gi++] = pattern;
		if (gpath && gpath[0])
			gv[gi++] = gpath;
		gv[gi] = NULL;
		status = run_captured("grep", gv, &out, &err, timeout_ms);
		result_wrap(&sb, status, &out, &err);
		free(ctx_dup);
		free(pattern); free(gpath);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "TodoWrite") == 0) {
		/* todos 数组 → markdown checklist，仅作为 tool_result 回喂。
		 * 持久化由 conversation history 承担（本工具的完整调用记录
		 * 就是状态本身），不落盘、不注入 system prompt。 */
		JsonVal todos = json_get(jp.val, "todos");
		int total, i;
		if (todos.type != JSON_ARRAY) {
			sb_append(&sb, "Error: TodoWrite requires 'todos' array");
			return sb.data;
		}
		total = json_array_len(todos);
		for (i = 0; i < total; i++) {
			JsonVal item = json_array_get(todos, i);
			char *content = json_get_string(item, "content");
			char *st = json_get_string(item, "status");
			sb_append(&sb, "- [");
			sb_append(&sb, (st && strcmp(st, "completed") == 0) ? "x" : " ");
			sb_append(&sb, "] ");
			sb_append(&sb, content ? content : "");
			sb_append(&sb, "\n");
			free(content); free(st);
		}
		return sb.data;
	}

	if (strcmp(name, "PlanConfirm") == 0) {
		char *draft = g_paths ? store_plan_draft_read(g_paths) : NULL;
		if (!draft || !draft[0]) {
			sb_append(&sb, "Error: no plan draft to confirm (write it first)");
			free(draft);
			return sb.data;
		}
		store_plan_set(g_paths, draft);
		store_plan_draft_clear(g_paths);
		sb_append(&sb, "Plan confirmed and locked. It is now included in every request.");
		free(draft);
		return sb.data;
	}

	if (strcmp(name, "PlanClear") == 0) {
		if (g_paths)
			store_plan_clear(g_paths);
		sb_append(&sb, "Plan cleared.");
		return sb.data;
	}

	if (strcmp(name, "Skill") == 0) {
		/* L2 知识级：按路径优先级搜索技能包，全文作为 tool_result 回喂。
		 * 搜索序（先命中先用，通用 agent 目录，不绑定特定工具）：
		 *   1. $CWD/skills/<name>/SKILL.md           项目级
		 *   2. $HOME/.agents/skills/<name>/SKILL.md  用户级
		 *   3. $BB_AGENT_HOME/skills/<name>/SKILL.md busyagent 自有 */
		char *skill = json_get_string(jp.val, "name");
		char *cwd = xrealloc_getcwd_or_warn(NULL);
		const char *home = getenv("HOME");
		const char *bag_home = getenv("BB_AGENT_HOME");
		char *data = NULL, *found = NULL;
		char *cands[3];
		int i, n = 0;

		if (!skill || !skill[0]) {
			sb_append(&sb, "Error: Skill requires 'name'");
			free(skill); free(cwd);
			return sb.data;
		}
		if (cwd) cands[n++] = xasprintf("%s/skills/%s/SKILL.md", cwd, skill);
		if (home && home[0]) cands[n++] = xasprintf("%s/.agents/skills/%s/SKILL.md", home, skill);
		cands[n++] = xasprintf("%s/skills/%s/SKILL.md",
				       (bag_home && bag_home[0]) ? bag_home : "/tmp/busyagent", skill);

		for (i = 0; i < n && !data; i++) {
			data = read_file_all(cands[i]);
			if (data) found = cands[i];
		}
		if (data && data[0]) {
			sb_appendf(&sb, "Skill: %s (from %s)\n\n", skill, found);
			sb_append(&sb, data);
		} else {
			sb_appendf(&sb, "Error: skill '%s' not found "
				     "(searched cwd/skills, ~/.agents/skills, $BB_AGENT_HOME/skills)", skill);
		}
		for (i = 0; i < n; i++) free(cands[i]);
		free(data);
		free(skill);
		free(cwd);
		return sb.data;
	}

	if (strcmp(name, "SubAgent") == 0) {
		/* L3 委托级：自举 —— fork+exec busyagent 自己，配置经环境变量下传 */
		char *prompt = json_get_string(jp.val, "prompt");
		char *sub_argv[5];
		int sub_argc = 0;
		int st;
		if (!prompt || !prompt[0]) {
			sb_append(&sb, "Error: SubAgent requires 'prompt'");
			free(prompt);
			return sb.data;
		}
		setenv("BB_AGENT_OUTPUT", "text", 1);   /* 子进程输出回喂须为纯文本 */
		sub_argv[sub_argc++] = (char *)"busyagent";
		sub_argv[sub_argc++] = (char *)"-n";    /* 独立新会话，不共享本会话 history */
		sub_argv[sub_argc++] = prompt;
		sub_argv[sub_argc] = NULL;
		st = run_captured("busyagent", sub_argv, &out, &err,
				  timeout_ms ? timeout_ms : 600000);
		result_wrap(&sb, st, &out, &err);
		free(prompt);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	/* ---- L0/L1：exec 映射 ---- */

	tool = find_tool(name);
	if (!tool || !tool->applet) {
		sb_appendf(&sb, "Error: tool '%s' is not in the tool table", name);
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
