/*
 * ba_impl.c - support implementation for the busyagent applet.
 *
 * Everything the applet needs besides its main loop lives here, in
 * dependency order: utils, JSON parser/serializer, HTTP client (SSE),
 * session store, display rendering, protocol adaptation, system prompt
 * assembly and the busybox tool executor.
 */
#include "busyagent.h"
#include "agent_common.h"
#include "ba_builtin_schemas.h"
/* everything below leans on libbb (xmalloc_read, full_write,
 * bb_make_directory, lineedit, ...) - include it once up front */
#include "libbb.h"
#include "busybox.h"

/* ==== ba_util.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * bb_http - plain-HTTP transport for busyagent
 *
 * Replaces the libcurl backend of bash-agent's transport.c using only
 * busybox/libbb primitives and the shared httpx-style client in
 * agent_common.c (agc_http_request_stream) feeding the provider-agnostic
 * SSE pump in ba_transport.c.
 *
 * TLS is provided by the in-tree client (networking/tls.c); the
 * no-verification trade-off is announced once per process.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#define BA_MAX_RETRIES        2
#define BA_RETRY_MAX_TIME_MS  20000

/* forward declarations - the provider parsers live below (ba_transport) */
static void parse_openai_sse_event(StreamCtx *sctx, const char *data, size_t data_len);
static void parse_responses_sse_event(StreamCtx *sctx, const char *event,
				      const char *data, size_t data_len);
static void process_residual_json(const char *residual, const char *provider,
				  sse_callback_fn callback, void *ctx);
static void emit_simple_event(sse_callback_fn callback, void *ctx,
			      SseEventType type, const char *content);

/* pump context: provider dispatch rides on the shared SSE splitter
 * (agent_common.c) while a bounded raw copy catches non-SSE JSON bodies */
typedef struct {
	StreamCtx *sctx;
	AgcSse sse;
	StrBuf raw;         /* whole body while no SSE field was recognized */
	AgcHttpResp *resp;  /* response headers, filled before body chunks */
	int mode_known;     /* content-type evaluated for this body */
	int is_sse;         /* body declared itself text/event-stream */
} BaSsePump;

/* one complete SSE event: dispatch each data line separately, matching
 * the historical per-"data:"-line parse of the providers (each line is
 * an independent JSON document for claude/openai) */
static void ba_sse_dispatch_event(void *vctx, const char *event,
				  const char *data, size_t data_len)
{
	BaSsePump *p = vctx;
	StreamCtx *sctx = p->sctx;
	const char *line = data;
	size_t remain = data_len;

	while (remain > 0) {
		const char *nl = memchr(line, '\n', remain);
		size_t llen = nl ? (size_t)(nl - line) : remain;

		if (llen > 0) {
			/* the provider parsers need NUL-terminated input */
			char *copy = xstrndup(line, llen);

			if (strcmp(sctx->provider, "openai") == 0)
				parse_openai_sse_event(sctx, copy, llen);
			else if (strcmp(sctx->provider, "responses") == 0)
				parse_responses_sse_event(sctx, event, copy, llen);
			else
				sse_parse_event(sctx->provider, copy, llen,
						sctx->callback, sctx->ctx);
			free(copy);
		}
		if (!nl)
			break;
		line = nl + 1;
		remain -= llen + 1;
	}
}

/* agc_http_request_stream chunk callback: 0 continue, <0 abort */
static int ba_sse_pump_chunk(void *vctx, const char *buf, size_t len)
{
	BaSsePump *p = vctx;

	if (p->sctx->cancelled && *(p->sctx->cancelled))
		return -1;
	if (!p->mode_known) {
		/* the response header is in: decide once whether this is
		 * an SSE stream or a plain JSON document.  A large plain
		 * body must not be fed through the SSE line splitter
		 * (its per-line cap does not apply to JSON bodies). */
		const char *ct = p->resp ? p->resp->content_type : NULL;

		p->is_sse = (ct
			     && strncasecmp(ct, "text/event-stream", 17) == 0);
		p->mode_known = 1;
	}
	if (p->is_sse) {
		agc_sse_feed(&p->sse, buf, len, ba_sse_dispatch_event, p);
		if (p->sse.error)
			return -1;
		return 0;
	}
	/* non-SSE body: keep the whole copy for the JSON fallback,
	 * bounded like agc_http_request; crossing the limit fails the
	 * request instead of silently truncating the document */
	if (len > BA_MAX_BODY - p->raw.len)
		return -1;
	sb_appendn(&p->raw, buf, len);
	return 0;
}

/* Streaming POST with SSE pump. Mirrors the old curl semantics:
 * up to 2 retries, 1s delay, 20s total retry window, retry on 5xx.
 * HTTP connect/send/read all go through the shared agc_http core
 * (agent_common.c) - the same client mcpc and oapi use. */
int http_post_sse(const char *url, const char **headers, int header_count,
		  const char *body, size_t body_len,
		  const char *provider,
		  sse_callback_fn callback, void *ctx,
		  volatile int *cancelled)
{
	unsigned start_ms = monotonic_ms();
	int attempt;

	for (attempt = 0; attempt <= BA_MAX_RETRIES; attempt++) {
		StreamCtx sctx;
		BaSsePump pump;
		AgcHttpReq req;
		AgcHttpResp resp;
		int rc, io_err, http_code;

		sse_stream_init(&sctx, provider, callback, ctx, cancelled);
		memset(&pump, 0, sizeof(pump));
		pump.sctx = &sctx;
		pump.resp = &resp;
		sb_init(&pump.raw);
		agc_sse_init(&pump.sse);

		memset(&req, 0, sizeof(req));
		req.method = "POST";
		req.url = url;
		req.headers = headers;
		req.header_count = header_count;
		req.body = body ? body : "";
		req.body_len = body_len;
		req.cancelled = cancelled;
		req.timeout_ms = BA_READ_TIMEOUT_MS;

		rc = agc_http_request_stream(&req, ba_sse_pump_chunk, &pump, &resp);
		io_err = (rc != AGC_HTTP_COMPLETE && rc != AGC_HTTP_STOPPED);
		http_code = resp.status;
		agc_http_resp_free(&resp);

		if (cancelled && *cancelled) {
			sse_stream_free(&sctx);
			agc_sse_free(&pump.sse);
			sb_free(&pump.raw);
			{
				SseEvent st;
				memset(&st, 0, sizeof(st));
				st.type = SSE_STOP;
				st.content = (char *)"interrupted";
				callback(ctx, &st);
			}
			return 0;
		}

		if (!io_err && http_code < 500) {
			/* stream tail: a last unterminated event, the
			 * responses termination check and - when nothing
			 * looked like SSE at all - the plain JSON body */
			agc_sse_finish(&pump.sse, ba_sse_dispatch_event, &pump);
			if (!pump.sse.saw_sse)
				process_residual_json(pump.raw.data ? pump.raw.data : "",
						      provider, callback, ctx);
			else if (strcmp(provider, "responses") == 0
			 && !sctx.responses_terminal) {
				emit_simple_event(callback, ctx, SSE_ERROR,
					"Stream interrupted (no response.completed received)");
				emit_simple_event(callback, ctx, SSE_STOP, "error");
			}
			sse_stream_free(&sctx);
			agc_sse_free(&pump.sse);
			sb_free(&pump.raw);
			if (http_code >= 400)
				return http_code;
			return 0;
		}
		/* 5xx or io error: fall through to retry logic */
		sse_stream_free(&sctx);
		agc_sse_free(&pump.sse);
		sb_free(&pump.raw);

		if (attempt >= BA_MAX_RETRIES)
			return io_err ? -1 : (http_code >= 400 ? http_code : 0);
		if ((unsigned)(monotonic_ms() - start_ms) >= BA_RETRY_MAX_TIME_MS)
			return io_err ? -1 : (http_code >= 400 ? http_code : 0);
		{
			SseEvent retry_evt;
			memset(&retry_evt, 0, sizeof(retry_evt));
			retry_evt.type = SSE_RETRY;
			callback(ctx, &retry_evt);
		}
		/* interruptible backoff: keep checking the cancelled flag so a
		 * Ctrl-C during the wait does not wait out the full second */
		{
			int slept = 0;
			while (slept < 1000 && !(cancelled && *cancelled)) {
				usleep(50 * 1000);
				slept += 50;
			}
		}
	}
	return -1;
}

/* ==== ba_store.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ============================================================
 * SessionPaths
 * ============================================================ */

void store_session_paths_free(SessionPaths *p) {
    if (!p) return;
    FREE_PTR(p->base_dir);
    FREE_PTR(p->session_dir);
    FREE_PTR(p->conversation);
    FREE_PTR(p->events);
    FREE_PTR(p->stats);
    FREE_PTR(p->summary);
    FREE_PTR(p->plan);
    FREE_PTR(p->plan_draft);
}

SessionPaths store_session_paths_dup(const SessionPaths *p) {
    SessionPaths d;
    memset(&d, 0, sizeof(d));
    if (!p) return d;
    d.base_dir     = p->base_dir     ? util_strdup(p->base_dir) : NULL;
    d.session_dir  = p->session_dir  ? util_strdup(p->session_dir) : NULL;
    d.conversation = p->conversation ? util_strdup(p->conversation) : NULL;
    d.events       = p->events       ? util_strdup(p->events) : NULL;
    d.stats        = p->stats        ? util_strdup(p->stats) : NULL;
    d.summary      = p->summary      ? util_strdup(p->summary) : NULL;
    d.plan         = p->plan         ? util_strdup(p->plan) : NULL;
    d.plan_draft   = p->plan_draft   ? util_strdup(p->plan_draft) : NULL;
    return d;
}

char *store_session_project_key(const char *cwd) {
    /* mirrors the bash AWK algorithm:
     *   sub(/^\/+/, "", $0)              - strip leading /
     *   gsub(/\//, "-", $0)              — / → -
     *   gsub(/[^A-Za-z0-9._-]/, "-", $0) - map others to -
     *   gsub(/-+/, "-", $0)              - squeeze runs of -
     *   sub(/^-+/, "", $0)               - strip leading -
     *   sub(/-+$/, "", $0)               - strip trailing -
     *   print "-" $0                      - prefix with -
     */
    if (!cwd || !cwd[0]) return util_strdup("-");

    size_t len = strlen(cwd);
    char *key = malloc(len + 3); /* room for the - prefix */
    if (!key) return NULL;

    /* skip leading / */
    const char *src = cwd;
    while (*src == '/') src++;

    /* convert in one pass */
    size_t ki = 0;
    char prev = '\0';
    for (; *src; src++) {
        char c = *src;
        if (c == '/') c = '-';
        else if (!(  (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            c = '-';
        /* squeeze runs of - */
        if (c == '-' && prev == '-') continue;
        key[ki++] = c;
        prev = c;
    }
    key[ki] = '\0';

    /* strip trailing - */
    while (ki > 0 && key[ki - 1] == '-') key[--ki] = '\0';

    /* prefix with - */
    char *result = malloc(ki + 2);
    if (!result) { free(key); return NULL; }
    result[0] = '-';
    memcpy(result + 1, key, ki + 1);
    free(key);
    return result;
}

SessionPaths store_session_paths_for(const char *home, const char *cwd, const char *session_id) {
    SessionPaths p;
    memset(&p, 0, sizeof(p));

    char *key = store_session_project_key(cwd);
    StrBuf buf;
    sb_init(&buf);

    /* base_dir = $BA_HOME/projects/<key> */
    sb_appendf(&buf, "%s/projects/%s", home, key);
    p.base_dir = util_strdup(buf.data);

    /* session_dir = base_dir/<session-id> */
    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/%s", p.base_dir, session_id);
    p.session_dir = util_strdup(buf.data);

    /* the file paths */
    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/conversation.jsonl", p.session_dir);
    p.conversation = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/events.jsonl", p.session_dir);
    p.events = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/stats.json", p.session_dir);
    p.stats = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/summary.txt", p.session_dir);
    p.summary = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/plan.md", p.session_dir);
    p.plan = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/plan.draft", p.session_dir);
    p.plan_draft = util_strdup(buf.data);

    sb_free(&buf);
    free(key);
    return p;
}

/* touch a file (create when missing) */
static int touch_file(const char *path) {
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    fclose(f);
    return 0;
}

const char *store_session_image_dir(const SessionPaths *paths) {
    static __thread char buf[1024];
    snprintf(buf, sizeof(buf), "%s/images", paths->session_dir);
    return buf;
}

int store_session_init(const SessionPaths *p, int is_new) {
    if (util_mkdirs(p->base_dir, 0755) != 0) return -1;
    if (util_mkdirs(p->session_dir, 0755) != 0) return -1;
    mkdir(store_session_image_dir(p), 0755);
    touch_file(p->conversation);
    touch_file(p->events);
    touch_file(p->summary);
    touch_file(p->plan);
    touch_file(p->plan_draft);

    if (is_new) {
        /* write the initial stats.json */
        FILE *f = fopen(p->stats, "w");
        if (!f) return -1;
        fprintf(f, "{\"current_turn_count\":0,\"agent_request_count\":0,"
                   "\"compact_request_count\":0,\"sub_agent_request_count\":0,"
                   "\"total_input_tokens\":0,"
                   "\"total_output_tokens\":0,\"total_cache_read_tokens\":0,"
                   "\"total_cache_creation_tokens\":0,\"current_context_tokens\":0,"
                   "\"last_updated\":\"\"}\n");
        fclose(f);

        /* write the session_start event (bash parity) */
        {
            StrBuf evt;
            sb_init(&evt);
            sb_append(&evt, "{\"type\":\"session_start\",\"session_id\":");
            /* extract session_id from the session_dir path */
            const char *sid = strrchr(p->session_dir, '/');
            sb_append_json_string(&evt, sid ? sid + 1 : "");
            sb_append_char(&evt, '}');
            store_event_append(p, evt.data);
            sb_free(&evt);
        }
    } else {
        touch_file(p->stats);
    }

    /* create the images directory */
    {
        char imgdir[1024];
        snprintf(imgdir, sizeof(imgdir), "%s/images", p->session_dir);
        util_mkdirs(imgdir, 0755);
    }

    return 0;
}

int store_session_fork(const SessionPaths *parent, const SessionPaths *child) {
    util_mkdirs(child->session_dir, 0755);
    char *parent_conv = util_read_file(parent->conversation);
    if (parent_conv && strlen(parent_conv) > 0) util_write_file(child->conversation, parent_conv);
    free(parent_conv);
    char *parent_summary = util_read_file(parent->summary);
    if (parent_summary && strlen(parent_summary) > 0) util_write_file(child->summary, parent_summary);
    free(parent_summary);
    char *parent_plan = util_read_file(parent->plan);
    if (parent_plan && strlen(parent_plan) > 0) util_write_file(child->plan, parent_plan);
    free(parent_plan);
    return 0;
}

int store_session_init_sub(const SessionPaths *parent_paths, const SessionPaths *sub_paths, int fork) {
    if (fork) {
        store_session_fork(parent_paths, sub_paths);
    }
    if (store_session_init(sub_paths, 1) != 0) return -1;
    return 0;
}

char *session_new_id(void) {
    time_t now = time(NULL);
    static int seeded = 0;
    if (!seeded) { srand((unsigned)(now ^ getpid())); seeded = 1; }
    struct tm *t = localtime(&now);
    unsigned short r = (unsigned short)(rand() % 0xFFFF);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d-%04x",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, r);
    return util_strdup(buf);
}

char *store_session_resolve_continue(const char *home, const char *cwd) {
    char *key = store_session_project_key(cwd);
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "%s/projects/%s", home, key);

    DIR *dir = opendir(buf.data);
    if (!dir) { sb_free(&buf); free(key); return NULL; }

    char *latest_id = NULL;
    long latest_time = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || strncmp(entry->d_name, "sub_", 4) == 0) continue;
        /* try to parse the dir name as a timestamp */
        struct stat st;
        sb_truncate(&buf, 0);
        sb_appendf(&buf, "%s/projects/%s/%s", home, key, entry->d_name);
        if (stat(buf.data, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* prefer events.jsonl mtime, fall back to dir mtime */
        time_t mtime = st.st_mtime;
        size_t base_len = buf.len;
        sb_append(&buf, "/events.jsonl");
        struct stat events_st;
        if (stat(buf.data, &events_st) == 0) {
            mtime = events_st.st_mtime;
        }
        sb_truncate(&buf, base_len);
        if (mtime > latest_time) {
            latest_time = mtime;
            free(latest_id);
            latest_id = util_strdup(entry->d_name);
        }
    }
    closedir(dir);
    sb_free(&buf);
    free(key);
    return latest_id;
}

int store_session_list_rows(const char *home, const char *cwd, StrBuf *out) {
    char *key = store_session_project_key(cwd);
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "%s/projects/%s", home, key);

    struct dirent **namelist;
    int n = scandir(buf.data, &namelist, NULL, alphasort);
    if (n < 0) { sb_free(&buf); free(key); return 0; }

    /* collect valid session names and mtimes */
    char **names = calloc(n, sizeof(char *));
    time_t *mtimes = calloc(n, sizeof(time_t));
    int valid = 0;

    for (int i = 0; i < n; i++) {
        struct dirent *entry = namelist[i];
        if (entry->d_name[0] == '.') { free(entry); continue; }
        struct stat st;
        sb_truncate(&buf, 0);
        sb_appendf(&buf, "%s/projects/%s/%s", home, key, entry->d_name);
        if (stat(buf.data, &st) != 0 || !S_ISDIR(st.st_mode)) { free(entry); continue; }
        names[valid] = util_strdup(entry->d_name);
        mtimes[valid] = st.st_mtime;
        valid++;
        free(entry);
    }
    free(namelist);

    /* sort by mtime, newest first */
    /* indirect sort via an index array */
    int *order = calloc(valid, sizeof(int));
    for (int i = 0; i < valid; i++) order[i] = i;
    /* selection sort (few sessions); compare mtimes[order[i]] */
    for (int i = 0; i < valid - 1; i++) {
        for (int j = i + 1; j < valid; j++) {
            if (mtimes[order[j]] > mtimes[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    int count = 0;
    for (int idx = 0; idx < valid; idx++) {
        int i = order[idx];
        struct stat st;
        sb_truncate(&buf, 0);
        sb_appendf(&buf, "%s/projects/%s/%s", home, key, names[i]);
        stat(buf.data, &st);

        /* modified: dir mtime, YYYY-MM-DD HH:MM (bash parity) */
        char time_buf[32];
        struct tm *tm = localtime(&st.st_mtime);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm);

        /* preview: first non-empty summary line; >60 chars -> 57 + ... */
        char preview[1024];
        preview[0] = '\0';
        size_t base_len = buf.len;
        sb_append(&buf, "/summary.txt");
        FILE *fp = fopen(buf.data, "r");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                /* trim leading/trailing whitespace */
                char *start = line;
                while (*start && isspace((unsigned char)*start)) start++;
                if (*start) {
                    size_t len = strlen(start);
                    while (len > 0 && isspace((unsigned char)start[len - 1])) start[--len] = '\0';
                    strncpy(preview, start, sizeof(preview) - 1);
                    preview[sizeof(preview) - 1] = '\0';
                    break;
                }
            }
            fclose(fp);
        }
        sb_truncate(&buf, base_len);

        /* truncate by UTF-8 char count (bash ${#preview} parity) */
        util_truncate_chars(preview, 60);

        sb_appendf(out, "%-40s %-16s %s\n", names[i], time_buf, preview);
        count++;
    }

    for (int i = 0; i < valid; i++) FREE_PTR(names[i]);
    free(names);
    free(mtimes);
    free(order);
    sb_free(&buf);
    free(key);
    return count;
}

/* ============================================================
 * conversation.jsonl operations
 * ============================================================ */

int store_conv_add_user(const char *path, const char *content) {
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "{\"role\":\"user\",\"content\":");
    sb_append_json_string(&buf, content);
    sb_append(&buf, "}");
    int rc = jsonl_append(path, buf.data);
    sb_free(&buf);
    return rc;
}

int store_conv_add_assistant(const char *path, const char *thinking, const char *text,
                       int tool_count, const char **tool_ids,
                       const char **tool_names, const char **tool_inputs) {
    StrBuf buf;
    int first = 1;
    sb_init(&buf);
    sb_append(&buf, "{\"role\":\"assistant\",\"content\":[");

    /* thinking block - only when non-empty (Claude API rejects empty) */
    if (thinking && thinking[0]) {
        sb_append(&buf, "{\"type\":\"thinking\",\"thinking\":");
        sb_append_json_string(&buf, thinking);
        sb_append(&buf, "}");
        first = 0;
    }

    /* text block - same */
    if (text && text[0]) {
        if (!first) sb_append(&buf, ",");
        sb_append(&buf, "{\"type\":\"text\",\"text\":");
        sb_append_json_string(&buf, text);
        sb_append(&buf, "}");
        first = 0;
    }

    /* tool_use blocks */
    for (int i = 0; i < tool_count; i++) {
        if (!first) sb_append(&buf, ",");
        sb_appendf(&buf, "{\"type\":\"tool_use\",\"id\":");
        sb_append_json_string(&buf, tool_ids[i]);
        sb_append(&buf, ",\"name\":");
        sb_append_json_string(&buf, tool_names[i]);
        sb_append(&buf, ",\"input\":");
        sb_append(&buf, tool_inputs[i]); /* already JSON */
        sb_append(&buf, "}");
        first = 0;
    }

    /* all-empty message gets a placeholder text to stay legal */
    if (first)
        sb_append(&buf, "{\"type\":\"text\",\"text\":\"(empty)\"}");

    sb_append(&buf, "]}");
    int rc = jsonl_append(path, buf.data);
    sb_free(&buf);
    return rc;
}

int store_conv_add_tool_results(const char *path, int count, const char **tool_use_ids,
                          const char **contents) {
    StrBuf buf;
    sb_init(&buf);
    sb_append(&buf, "{\"role\":\"user\",\"content\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0) sb_append(&buf, ",");
        sb_append(&buf, "{\"type\":\"tool_result\",\"tool_use_id\":");
        sb_append_json_string(&buf, tool_use_ids[i]);
        sb_append(&buf, ",\"content\":");
        sb_append_json_string(&buf, contents[i]);
        sb_append(&buf, "}");
    }
    sb_append(&buf, "]}");
    int rc = jsonl_append(path, buf.data);
    sb_free(&buf);
    return rc;
}

int store_conv_line_count(const char *path, char ***out, int *out_count) {
    *out = NULL;
    *out_count = 0;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int cap = 64;
    int count = 0;
    char **lines = malloc(cap * sizeof(char *));
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t read_len;
    if (!lines) goto fail;

    /* getline grows on real newlines; a long JSONL record is one line */
    while ((read_len = getline(&line, &line_cap, f)) != -1) {
        /* strip trailing newline */
        size_t len = (size_t)read_len;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        if (count >= cap) {
            int new_cap = cap * 2;
            char **new_lines = realloc(lines, new_cap * sizeof(char *));
            if (!new_lines) goto fail;
            lines = new_lines;
            cap = new_cap;
        }
        lines[count] = util_strdup(line);
        if (!lines[count]) goto fail;
        count++;
    }

    free(line);
    fclose(f);
    *out = lines;
    *out_count = count;
    return 0;

fail:
    free(line);
    fclose(f);
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return -1;
}

int store_conv_trim_tail(const char *path, int keep_lines) {
    char **lines = NULL;
    int count = 0;
    if (store_conv_line_count(path, &lines, &count) != 0) return -1;
    if (keep_lines >= count) {
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return 0;
    }

    /* rewrite keeping only the last keep_lines lines */
    FILE *f = fopen(path, "w");
    if (!f) {
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return -1;
    }
    int start = count - keep_lines;
    for (int i = start; i < count; i++) {
        fprintf(f, "%s\n", lines[i]);
    }
    fclose(f);
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return 0;
}

int store_conv_user_turn_count(const char *path) {
    char **lines = NULL;
    int count = 0;
    if (store_conv_line_count(path, &lines, &count) != 0) return 0;
    int user_count = 0;
    for (int i = 0; i < count; i++) {
        JsonParse jp = json_parse_root(lines[i]);
        if (jp.error) continue;
        char *role = json_get_string(jp.val, "role");
        if (role && strcmp(role, "user") == 0) {
            JsonVal content = json_get(jp.val, "content");
            if (content.type == JSON_STRING) {
                user_count++;
            }
        }
        free(role);
    }
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return user_count;
}

long store_conv_total_bytes(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* ============================================================
 * stats.json operations
 * ============================================================ */

char *store_stats_read(const char *path) {
    return util_read_file(path);
}

static void stats_write_canonical(const char *path,
                                  int current_turn_count,
                                  int agent_request_count,
                                  int compact_request_count,
                                  int sub_agent_request_count,
                                  int total_input_tokens,
                                  int total_output_tokens,
                                  int total_cache_read_tokens,
                                  int total_cache_creation_tokens,
                                  int current_context_tokens,
                                  const char *last_updated) {
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "{\"current_turn_count\":%d,\"agent_request_count\":%d,"
               "\"compact_request_count\":%d,\"sub_agent_request_count\":%d,"
               "\"total_input_tokens\":%d,\"total_output_tokens\":%d,"
               "\"total_cache_read_tokens\":%d,\"total_cache_creation_tokens\":%d,"
               "\"current_context_tokens\":%d,\"last_updated\":",
               current_turn_count, agent_request_count, compact_request_count,
               sub_agent_request_count, total_input_tokens, total_output_tokens,
               total_cache_read_tokens, total_cache_creation_tokens,
               current_context_tokens);
    sb_append_json_string(&buf, last_updated ? last_updated : "");
    sb_append(&buf, "}\n");
    util_write_file(path, buf.data);
    sb_free(&buf);
}

void store_stats_add_int(JsonVal obj, const char *key, int delta) {
    int cur = store_stats_get_int(obj, key);
    store_stats_set_int(obj, key, cur + delta);
}

void store_stats_set_int(JsonVal obj, const char *key, int value) {
    /* modify a numeric value inside the JSON source in place.
     * Used only as the store_stats_update callback.
     * If the new number fits, overwrite; pad leftover digits with spaces.
     * If it does not fit, skip (cannot grow in place). */
    if (!obj.src) return;
    /* find "key":<number> in the source text */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj.src, search);
    if (!p) return;
    p += strlen(search);
    /* skip whitespace and the colon */
    while (*p == ' ' || *p == ':') p++;
    /* p now points at the start of the value */
    const char *val_start = p;
    /* find the end of the value (comma, } or whitespace) */
    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r') p++;
    int old_len = (int)(p - val_start);
    char new_val[32];
    snprintf(new_val, sizeof(new_val), "%d", value);
    int new_len = (int)strlen(new_val);
    if (new_len > old_len) return; /* cannot grow in place */
    memcpy((char*)val_start, new_val, new_len);
    /* pad leftover positions with spaces */
    for (int i = new_len; i < old_len; i++) ((char*)val_start)[i] = ' ';
}

int store_stats_get_int(JsonVal obj, const char *key) {
    return json_get_int(obj, key);
}

/* simple file-level helper: read an integer field from the stats file */
int store_stats_get_file_int(const char *path, const char *key) {
    char *content = store_stats_read(path);
    if (!content) return 0;
    JsonParse jp = json_parse_root(content);
    int val = jp.error ? 0 : json_get_int(jp.val, key);
    free(content);
    return val;
}

/* Set an integer field in the stats file.
 * Reads existing fields; missing/invalid read as 0; writes canonical stats JSON.
 * Old versions missing fields get them on the next write; no backfill. */
int store_stats_get_int_file(const char *path, const char *key)
{
	return store_stats_get_file_int(path, key);
}

void store_stats_set_int_file(const char *path, const char *key, int value) {
    int current_turn_count = 0, agent_request_count = 0;
    int compact_request_count = 0, sub_agent_request_count = 0;
    int total_input_tokens = 0, total_output_tokens = 0;
    int total_cache_read_tokens = 0, total_cache_creation_tokens = 0;
    int current_context_tokens = 0;

    char *content = store_stats_read(path);
    if (content && content[0]) {
        JsonParse jp = json_parse_root(content);
        if (!jp.error) {
            current_turn_count = json_get_int(jp.val, "current_turn_count");
            agent_request_count = json_get_int(jp.val, "agent_request_count");
            compact_request_count = json_get_int(jp.val, "compact_request_count");
            sub_agent_request_count = json_get_int(jp.val, "sub_agent_request_count");
            total_input_tokens = json_get_int(jp.val, "total_input_tokens");
            total_output_tokens = json_get_int(jp.val, "total_output_tokens");
            total_cache_read_tokens = json_get_int(jp.val, "total_cache_read_tokens");
            total_cache_creation_tokens = json_get_int(jp.val, "total_cache_creation_tokens");
            current_context_tokens = json_get_int(jp.val, "current_context_tokens");
        }
    }

    if (strcmp(key, "current_turn_count") == 0) current_turn_count = value;
    else if (strcmp(key, "agent_request_count") == 0) agent_request_count = value;
    else if (strcmp(key, "compact_request_count") == 0) compact_request_count = value;
    else if (strcmp(key, "sub_agent_request_count") == 0) sub_agent_request_count = value;
    else if (strcmp(key, "total_input_tokens") == 0) total_input_tokens = value;
    else if (strcmp(key, "total_output_tokens") == 0) total_output_tokens = value;
    else if (strcmp(key, "total_cache_read_tokens") == 0) total_cache_read_tokens = value;
    else if (strcmp(key, "total_cache_creation_tokens") == 0) total_cache_creation_tokens = value;
    else if (strcmp(key, "current_context_tokens") == 0) current_context_tokens = value;

    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    stats_write_canonical(path, current_turn_count, agent_request_count,
                          compact_request_count, sub_agent_request_count,
                          total_input_tokens, total_output_tokens,
                          total_cache_read_tokens, total_cache_creation_tokens,
                          current_context_tokens, ts);
    if (content) {
        free(content);
    }
}

/* generic stats update: read -> mutate -> write back */
int store_stats_update(const char *path, stats_update_fn fn, void *ctx) {
    char *content = util_read_file(path);
    if (!content) return -1;

    JsonParse jp = json_parse_root(content);
    if (jp.error) { free(content); return -1; }

    /* let the callback mutate (re-serialize via StrBuf) */
    fn(ctx, jp.val);

    /* re-serialize */
    StrBuf buf;
    sb_init(&buf);
    sb_append_char(&buf, '{');
    JsonObjectIter it;
    json_obj_iter_init(&it, jp.val);
    int first = 1;
    while (json_obj_iter_next(&it)) {
        if (!first) sb_append(&buf, ",");
        first = 0;
        sb_append_json_string(&buf, it.key);
        sb_append_char(&buf, ':');
        /* value taken verbatim from the source text */
        size_t vlen = it.val.end - it.val.start;
        sb_appendn(&buf, jp.val.src + it.val.start, vlen);
    }
    sb_append_char(&buf, '}');
    sb_append_char(&buf, '\n');

    int rc = util_write_file(path, buf.data);
    sb_free(&buf);
    free(content);
    return rc;
}

/* ============================================================
 * events.jsonl operations
 * ============================================================ */

static _Thread_local int g_store_event_stream_json = 0;

void store_event_set_stream_json(int enabled) {
    g_store_event_stream_json = enabled ? 1 : 0;
}

int store_event_stream_json_enabled(void) {
    return g_store_event_stream_json;
}

int store_event_append(const SessionPaths *p, const char *json_str) {
    if (g_store_event_stream_json) {
        printf("%s\n", json_str);
        fflush(stdout);
    }
    return jsonl_append(p->events, json_str);
}

int store_event_lines(const SessionPaths *p, char ***out, int *out_count) {
    return store_conv_line_count(p->events, out, out_count);
}

/* ============================================================
 * summary / plan file operations
 * ============================================================ */

char *store_summary_get(const SessionPaths *p) {
    char *s = util_read_file(p->summary);
    if (s) {
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
            s[--len] = '\0';
        if (len == 0) { free(s); return NULL; }
    }
    return s;
}

int store_summary_set(const SessionPaths *p, const char *content) {
    return util_write_file(p->summary, content);
}

char *store_plan_draft_read(const SessionPaths *p) {
    return util_read_file(p->plan_draft);
}

int store_plan_draft_set(const SessionPaths *p, const char *content) {
    return util_write_file(p->plan_draft, content);
}

int store_plan_draft_clear(const SessionPaths *p) {
    return util_write_file(p->plan_draft, "");
}

int store_plan_set(const SessionPaths *p, const char *content) {
    return util_write_file(p->plan, content);
}

int store_plan_clear(const SessionPaths *p) {
    return util_write_file(p->plan, "");
}

/* ==== ba_display.c ==== */
/*
 * ba_display.c - synchronous display layer, ported from bash-agent
 * display.c / agent_tool_display_summary. Rendering rules, ANSI colors,
 * truncation and stream-json shapes are verbatim; linenoise output calls
 * degrade to plain stdio because busyagent is always non-interactive.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include <string.h>
#include <stdio.h>

/* ---- DisplayState (ported verbatim) ---- */
static void ds_update_last_char(BaDisplay *ds, const char *text) {
    if (!text || !*text) return;
    {
        const char *p = text;
        const char *last = p;
        while (*p) {
            last = p;
            unsigned char c = (unsigned char)*p;
            if (c < 0x80) p++;
            else if (c < 0xE0) p += 2;
            else if (c < 0xF0) p += 3;
            else p += 4;
        }
        size_t len = p - last;
        if (len > 0 && len < 8) {
            memcpy(ds->last_char, last, len);
            ds->last_char[len] = '\0';
        }
    }
}

/* non-interactive equivalent of linenoiseWrite (no raw-mode terminal) */
static void lw_write(const char *s, size_t n) {
    /* bash-agent's linenoiseWrite flushes immediately; streamed deltas
     * must not sit in stdout buffering (would delay until exit) */
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

#include <stdarg.h>
static void lw_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void ensure_newline(BaDisplay *ds) {
    if (ds->last_char[0] != '\n') {
        lw_write("\n", 1);
        ds->last_char[0] = '\n';
        ds->last_char[1] = '\0';
    }
}

void ba_disp_init(BaDisplay *d, BaDisplayFormat fmt)
{
    memset(d, 0, sizeof(*d));
    d->last_char[0] = '\n';
    d->format = fmt;
    d->out = stdout;
}

/* ---- tool-call summary (verbatim port of agent_tool_display_summary) ---- */
char *ba_tool_call_summary(const char *name, const char *input_json)
{
    char *field = NULL;
    JsonParse jp = json_parse_root(input_json && input_json[0] ? input_json : "{}");

    if (!jp.error) {
        if (!strcmp(name, "Read") || !strcmp(name, "Write") || !strcmp(name, "Edit")) {
            field = json_get_string(jp.val, "path");
        } else if (!strcmp(name, "Glob") || !strcmp(name, "Grep")) {
            field = json_get_string(jp.val, "pattern");
        } else if (!strcmp(name, "Bash")) {
            field = json_get_string(jp.val, "command");
            /* newlines to spaces, truncate long commands (bash parity) */
            if (field) {
                char *p;
                while ((p = strchr(field, '\n')) != NULL) *p = ' ';
                size_t flen = strlen(field);
                if (flen > 80) {
                    size_t slen = util_utf8_truncate_len(field, 77);
                    char *trunc = malloc(slen + 4);
                    memcpy(trunc, field, slen);
                    strcpy(trunc + slen, "...");
                    free(field);
                    field = trunc;
                }
            }
        } else if (!strcmp(name, "TodoWrite")) {
            JsonVal todos_arr = json_get(jp.val, "todos");
            if (todos_arr.type == JSON_ARRAY) {
                int total = json_array_len(todos_arr);
                int comp = 0;
                for (int ti = 0; ti < total; ti++) {
                    JsonVal it = json_array_get(todos_arr, ti);
                    char *st = json_get_string(it, "status");
                    if (st && strcmp(st, "completed") == 0) comp++;
                    free(st);
                }
                char buf2[32];
                snprintf(buf2, sizeof(buf2), "%d/%d", comp, total);
                field = xstrdup(buf2);
            }
        } else if (!strcmp(name, "Skill")) {
            field = json_get_string(jp.val, "name");
        } else if (!strcmp(name, "SubAgent")) {
            field = json_get_string(jp.val, "description");
        }
    }

    if (field)
        return field;
    if (input_json && input_json[0]) {
        size_t len = strlen(input_json);
        size_t cut = len > 80 ? util_utf8_truncate_len(input_json, 77) : len;
        char *s = xmalloc(cut + 4);
        memcpy(s, input_json, cut);
        strcpy(s + cut, len > 80 ? "..." : "");
        return s;
    }
    return xstrdup("");
}

char *ba_display_event_json(const BaDisplayMsg *msg, int stream_output);

char *ba_display_event_json(const BaDisplayMsg *msg, int stream_output)
{
    StrBuf buf;

    sb_init(&buf);
    switch (msg->type) {
        case BA_DM_TEXT:
            sb_append(&buf, "{\"type\":\"text\",\"content\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_THINKING:
            sb_append(&buf, "{\"type\":\"thinking\",\"content\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_TOOL_CALL:
            sb_append(&buf, "{\"type\":\"tool_call\",\"name\":");
            sb_append_json_string(&buf, msg->tool_name ? msg->tool_name : "");
            sb_append(&buf, ",\"id\":");
            sb_append_json_string(&buf, msg->tool_id ? msg->tool_id : "");
            sb_append(&buf, ",\"input\":");
            sb_append(&buf, (msg->tool_input && (msg->tool_input[0] || !stream_output))
                      ? msg->tool_input : "{}");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_TOOL_RESULT:
            sb_append(&buf, "{\"type\":\"tool_result\",\"tool_use_id\":");
            sb_append_json_string(&buf, msg->tool_id ? msg->tool_id : "");
            sb_append(&buf, ",\"name\":");
            sb_append_json_string(&buf, msg->tool_name ? msg->tool_name : "");
            sb_append(&buf, ",\"content\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_USAGE:
            sb_appendf(&buf, "{\"type\":\"usage\",\"input_tokens\":%d,\"output_tokens\":%d,"
                       "\"cache_read_input_tokens\":%d,\"cache_creation_input_tokens\":%d,"
                       "\"kind\":\"agent\"}",
                       msg->in_tokens, msg->out_tokens,
                       msg->cache_read_tokens, msg->cache_creation_tokens);
            break;
        case BA_DM_STOP:
            sb_append(&buf, "{\"type\":\"stop\",\"reason\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_ERROR:
            sb_append(&buf, "{\"type\":\"error\",\"message\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_SUB_AGENT_START:
            sb_append(&buf, "{\"type\":\"sub_agent_start\",\"session_id\":");
            sb_append_json_string(&buf, msg->session_id ? msg->session_id : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_SUB_AGENT_RESULT:
            sb_append(&buf, "{\"type\":\"sub_agent_result\",\"session_id\":");
            sb_append_json_string(&buf, msg->session_id ? msg->session_id : "");
            sb_appendf(&buf, ",\"status\":\"%s\"",
                       msg->tool_exit_code == 0 ? "ok" : "failed");
            sb_appendf(&buf, ",\"input_tokens\":%d,\"output_tokens\":%d}",
                       msg->in_tokens, msg->out_tokens);
            break;
        case BA_DM_ASYNC_TASK_RESULT:
            sb_append(&buf, "{\"type\":\"async_task_result\",\"task_id\":");
            sb_append_json_string(&buf, msg->session_id ? msg->session_id : "");
            sb_appendf(&buf, ",\"exit_code\":%d,\"output\":",
                       msg->tool_exit_code);
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        default:
            sb_free(&buf);
            return NULL;
    }
    return buf.data;
}

/* ---- main renderer: verbatim port of the render_message branches ---- */
char *ba_display_push(BaDisplay *ds, const BaDisplayMsg *msg)
{
    if (ds->format == BA_FMT_NONE)
        return NULL;

    if (ds->format == BA_FMT_STREAM_JSON) {
        char *event = ba_display_event_json(msg, 1);
        if (!event)
            return NULL;
        fprintf(ds->out, "%s\n", event);
        fflush(ds->out);
        return event;
    }

    /* human mode (ANSI/truncation rules ported verbatim) */
    switch (msg->type) {
    case BA_DM_THINKING:
        if (msg->content)
            lw_printf("\x1b[90m%s\x1b[0m", msg->content);
        ds_update_last_char(ds, msg->content);
        ds->prev_was_thinking = 1;
        break;

    case BA_DM_TEXT:
        if (msg->content) {
            if (ds->prev_was_thinking && ds->last_char[0] != '\n') {
                lw_write("\n", 1);
                ds->last_char[0] = '\n';
            }
            lw_write(msg->content, strlen(msg->content));
            ds_update_last_char(ds, msg->content);
        }
        ds->prev_was_thinking = 0;
        break;

    case BA_DM_TOOL_CALL: {
        ensure_newline(ds);
        const char *name = msg->tool_name ? msg->tool_name : "unknown";
        const char *summary = msg->content ? msg->content : "";
        lw_printf("\x1b[33m[tool] %s(%s)\x1b[0m\n", name, summary);
        ds->last_char[0] = '\n';
        ds->prev_was_thinking = 0;
        break;
    }

    case BA_DM_TOOL_RESULT:
        if (msg->content && msg->content[0]) {
            if (ds->prev_was_thinking && ds->last_char[0] != '\n')
                lw_write("\n", 1);
            ds->prev_was_thinking = 0;
            /* bash-agent display.c:222-236 - Edit prints in full; Read and
             * Write show only the first line (whole files stay out of the
             * terminal) */
            if (msg->tool_name && strcmp(msg->tool_name, "Edit") == 0) {
                lw_printf("%s\n", msg->content);
            } else if (msg->tool_name
                    && (strcmp(msg->tool_name, "Read") == 0
                     || strcmp(msg->tool_name, "Write") == 0)) {
                const char *nl = strchr(msg->content, '\n');
                if (nl) {
                    lw_write(msg->content, (size_t)(nl - msg->content));
                    lw_write("\n", 1);
                } else {
                    lw_printf("%s\n", msg->content);
                }
            } else {
                lw_printf("%s\n", msg->content);
            }
            ds->last_char[0] = '\n';
        }
        break;

    case BA_DM_USAGE:
        break;

    case BA_DM_STOP:
        ensure_newline(ds);
        if (msg->content && strcmp(msg->content, "interrupted") == 0)
            lw_printf("\x1b[36mInterrupted.\x1b[0m\n");
        ds->last_char[0] = '\n';
        break;

    case BA_DM_ERROR:
        ensure_newline(ds);
        lw_printf("\x1b[31mError: %s\x1b[0m\n",
                msg->content ? msg->content : "unknown");
        ds->last_char[0] = '\n';
        break;

    case BA_DM_CONTEXT_UPDATE:
        ensure_newline(ds);
        lw_printf("\x1b[36mContext compacted (%s).\x1b[0m\n",
                msg->tool_name ? msg->tool_name : "auto");
        ds->last_char[0] = '\n';
        break;

    case BA_DM_SUB_AGENT_START:
        break;   /* no human output (same as display.c:250) */

    case BA_DM_SUB_AGENT_RESULT: {
        ensure_newline(ds);
        if (msg->tool_exit_code == 0)
            lw_printf("\x1b[35m[sub-agent %s] completed (in=%d, out=%d)\x1b[0m\n",
                    msg->session_id ? msg->session_id : "?",
                    msg->in_tokens, msg->out_tokens);
        else
            lw_printf("\x1b[31m[sub-agent %s] failed\x1b[0m\n",
                    msg->session_id ? msg->session_id : "?");
        if (msg->tool_name && msg->tool_name[0]) {
            int tlen = (int)util_utf8_truncate_len(msg->tool_name, 120);
            lw_printf("\x1b[90m%.*s%s\x1b[0m\n",
                    tlen, msg->tool_name,
                    strlen(msg->tool_name) > 120 ? "\xe2\x80\xa6" : "");
        }
        if (msg->content && msg->content[0]) {
            int clen = (int)util_utf8_truncate_len(msg->content, 120);
            lw_printf("%.*s%s\n",
                    clen, msg->content,
                    strlen(msg->content) > 120 ? "\xe2\x80\xa6" : "");
        }
        ds->last_char[0] = '\n';
        ds->prev_was_thinking = 0;
        break;
    }

    case BA_DM_ASYNC_TASK_RESULT: {
        ensure_newline(ds);
        lw_printf("\x1b[%sm[bg-bash %s] exit_code=%d\x1b[0m\n",
                msg->tool_exit_code == 0 ? "36" : "31",
                msg->session_id ? msg->session_id : "?", msg->tool_exit_code);
        if (msg->content && msg->content[0]) {
            int clen = (int)util_utf8_truncate_len(msg->content, 120);
            lw_printf("%.*s%s\n",
                    clen, msg->content,
                    strlen(msg->content) > 120 ? "\xe2\x80\xa6" : "");
        }
        ds->last_char[0] = '\n';
        ds->prev_was_thinking = 0;
        break;
    }

    default:
        break;
    }
    return NULL;
}

/* ==== ba_transport.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>


static void emit_simple_event(sse_callback_fn callback, void *ctx,
                              SseEventType type, const char *content);
static void fill_openai_usage_event(SseEvent *evt, JsonVal usage);

static void streamctx_free_openai_tools(StreamCtx *sctx) {
    for (int i = 0; i < sctx->responses_item_count; i++) FREE_PTR(sctx->responses_item_ids[i]);
    FREE_PTR(sctx->responses_item_ids);
    FREE_PTR(sctx->responses_item_indexes);
    sctx->responses_item_count = 0;
    sctx->responses_item_cap = 0;
    for (int i = 0; i < sctx->openai_tool_count; i++) {
        FREE_PTR(sctx->openai_tools[i].id);
        FREE_PTR(sctx->openai_tools[i].name);
        sb_free(&sctx->openai_tools[i].arguments);
    }
    FREE_PTR(sctx->openai_tools);
    sctx->openai_tool_count = 0;
    sctx->openai_tool_cap = 0;
}

static void streamctx_reset_openai_tool(OpenAIToolAccum *tool) {
    FREE_PTR(tool->id);
    FREE_PTR(tool->name);
    sb_free(&tool->arguments);
    memset(tool, 0, sizeof(*tool));
}

static OpenAIToolAccum *streamctx_ensure_openai_tool(StreamCtx *sctx, int idx) {
    for (int i = 0; i < sctx->openai_tool_count; i++) {
        if (sctx->openai_tools[i].index == idx) return &sctx->openai_tools[i];
    }
    if (sctx->openai_tool_count >= sctx->openai_tool_cap) {
        int old_cap = sctx->openai_tool_cap;
        sctx->openai_tool_cap = sctx->openai_tool_cap ? sctx->openai_tool_cap * 2 : 4;
        sctx->openai_tools = realloc(sctx->openai_tools,
            (size_t)sctx->openai_tool_cap * sizeof(*sctx->openai_tools));
        memset(sctx->openai_tools + old_cap, 0,
            (size_t)(sctx->openai_tool_cap - old_cap) * sizeof(*sctx->openai_tools));
    }
    OpenAIToolAccum *tool = &sctx->openai_tools[sctx->openai_tool_count++];
    tool->index = idx;
    sb_init(&tool->arguments);
    return tool;
}

static void streamctx_emit_openai_tool_calls(StreamCtx *sctx) {
    for (int i = 0; i < sctx->openai_tool_count; i++) {
        OpenAIToolAccum *tool = &sctx->openai_tools[i];
        if (tool->arguments.len == 0) continue;
        SseEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SSE_TOOL_CALL;
        evt.tool_id = tool->id ? tool->id : "";
        evt.tool_name = tool->name ? tool->name : "";
        evt.tool_input = tool->arguments.data ? tool->arguments.data : "{}";
        sctx->callback(sctx->ctx, &evt);
        streamctx_reset_openai_tool(tool);
    }
    sctx->openai_tool_count = 0;
}

static void parse_openai_sse_event(StreamCtx *sctx, const char *data, size_t data_len) {
    if (data_len == 0) return;
    if (strcmp(data, "[DONE]") == 0) return;

    size_t pos = 0;
    JsonParse jp = json_parse(data, &pos);
    if (jp.error) return;

    char *obj_type = json_get_string(jp.val, "object");
    /* strict only when the field is present: several OpenAI-compatible
     * gateways omit "object" on stream chunks */
    if (obj_type && strcmp(obj_type, "chat.completion.chunk") != 0) {
        FREE_PTR(obj_type);
        return;
    }
    FREE_PTR(obj_type);

    JsonVal choices = json_get(jp.val, "choices");
    if (choices.type == JSON_ARRAY) {
        JsonVal choice = json_array_get(choices, 0);
        JsonVal delta = json_get(choice, "delta");
        char *content = json_get_string(delta, "content");
        if (content) {
            emit_simple_event(sctx->callback, sctx->ctx, SSE_TEXT, content);
            FREE_PTR(content);
        }
        char *reasoning = json_get_string(delta, "reasoning_content");
        if (!reasoning) reasoning = json_get_string(delta, "reasoning");
        if (reasoning) {
            emit_simple_event(sctx->callback, sctx->ctx, SSE_THINKING, reasoning);
            FREE_PTR(reasoning);
        }
        JsonVal tool_calls = json_get(delta, "tool_calls");
        if (tool_calls.type == JSON_ARRAY) {
            int tc_len = json_array_len(tool_calls);
            for (int i = 0; i < tc_len; i++) {
                JsonVal tc = json_array_get(tool_calls, i);
                int idx = json_get_int(tc, "index");
                JsonVal fn = json_get(tc, "function");
                OpenAIToolAccum *tool = streamctx_ensure_openai_tool(sctx, idx);
                char *id = json_get_string(tc, "id");
                char *name = json_get_string(fn, "name");
                char *arguments = json_get_string(fn, "arguments");
                /* non-standard OpenAI-compatible APIs (e.g. sensenova) send empty
                 * strings "" instead of omitting fields; [0] guard */
                if (id && id[0]) {
                    FREE_PTR(tool->id);
                    tool->id = id;
                } else {
                    FREE_PTR(id);
                }
                if (name && name[0]) {
                    FREE_PTR(tool->name);
                    tool->name = name;
                } else {
                    FREE_PTR(name);
                }
                if (arguments) {
                    sb_append(&tool->arguments, arguments);
                    FREE_PTR(arguments);
                }
            }
        }
        char *finish = json_get_string(choice, "finish_reason");
        /* non-standard APIs may use "" instead of null (e.g. sensenova);
         * an empty string must not trigger STOP */
        if (finish && finish[0]) {
            if (strcmp(finish, "tool_calls") == 0) {
                streamctx_emit_openai_tool_calls(sctx);
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "tool_use");
            } else if (strcmp(finish, "stop") == 0) {
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "end_turn");
            } else if (strcmp(finish, "length") == 0) {
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "max_tokens");
            } else {
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, finish);
            }
            FREE_PTR(finish);
        }
    }

    JsonVal usage = json_get(jp.val, "usage");
    if (usage.type != JSON_NULL) {
        SseEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SSE_USAGE;
        fill_openai_usage_event(&evt, usage);
        sctx->callback(sctx->ctx, &evt);
    }
}



static int responses_tool_index(StreamCtx *sctx, JsonVal root, JsonVal item) {
    char *item_id = json_get_string(root, "item_id");
    if (!item_id && item.type != JSON_NULL) item_id = json_get_string(item, "id");
    JsonVal output_index = json_get(root, "output_index");
    int idx = output_index.type == JSON_NUMBER ? json_get_int(root, "output_index") : -1;
    if (idx < 0 && item_id) {
        for (int i = 0; i < sctx->responses_item_count; i++) {
            if (strcmp(sctx->responses_item_ids[i], item_id) == 0) { idx = sctx->responses_item_indexes[i]; break; }
        }
    }
    if (idx >= 0 && item_id) {
        int found = 0;
        for (int i = 0; i < sctx->responses_item_count; i++) if (strcmp(sctx->responses_item_ids[i], item_id) == 0) { found = 1; break; }
        if (!found) {
            if (sctx->responses_item_count >= sctx->responses_item_cap) {
                sctx->responses_item_cap = sctx->responses_item_cap ? sctx->responses_item_cap * 2 : 4;
                sctx->responses_item_ids = realloc(sctx->responses_item_ids, (size_t)sctx->responses_item_cap * sizeof(char *));
                sctx->responses_item_indexes = realloc(sctx->responses_item_indexes, (size_t)sctx->responses_item_cap * sizeof(int));
            }
            int pos = sctx->responses_item_count++;
            sctx->responses_item_ids[pos] = util_strdup(item_id);
            sctx->responses_item_indexes[pos] = idx;
        }
    }
    FREE_PTR(item_id);
    return idx;
}

static void responses_record_usage(StreamCtx *sctx, JsonVal response) {
    JsonVal usage = json_get(response, "usage");
    if (usage.type == JSON_NULL) usage = response;
    sctx->responses_output_tokens = json_get_int(usage, "output_tokens");
    JsonVal input_details = json_get(usage, "input_tokens_details");
    int nested_cached = input_details.type == JSON_NULL ? 0 : json_get_int(input_details, "cached_tokens");
    sctx->responses_cache_read_tokens = nested_cached > 0
        ? nested_cached
        : json_get_int(usage, "cached_tokens");
    sctx->responses_input_tokens = json_get_int(usage, "input_tokens") - sctx->responses_cache_read_tokens;
    if (sctx->responses_input_tokens < 0) sctx->responses_input_tokens = 0;
}

static void responses_emit_usage(StreamCtx *sctx) {
    SseEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SSE_USAGE;
    evt.in_tokens = sctx->responses_input_tokens;
    evt.out_tokens = sctx->responses_output_tokens;
    evt.cache_read_tokens = sctx->responses_cache_read_tokens;
    sctx->callback(sctx->ctx, &evt);
}

static void parse_responses_sse_event(StreamCtx *sctx, const char *event, const char *data, size_t data_len) {
    if (data_len == 0) return;
    size_t pos = 0;
    JsonParse jp = json_parse(data, &pos);
    if (jp.error) return;
    JsonVal root = jp.val;
    if (strcmp(event, "response.reasoning_text.delta") == 0) {
        char *delta = json_get_string(root, "delta");
        if (delta && delta[0]) emit_simple_event(sctx->callback, sctx->ctx, SSE_THINKING, delta);
        FREE_PTR(delta);
    } else if (strcmp(event, "response.output_text.delta") == 0) {
        char *delta = json_get_string(root, "delta");
        if (delta) {
            char *text = delta;
            if (!sctx->responses_saw_text) while (*text == '\n' || *text == '\r') text++;
            if (*text) { sctx->responses_saw_text = 1; emit_simple_event(sctx->callback, sctx->ctx, SSE_TEXT, text); }
        }
        FREE_PTR(delta);
    } else if (strcmp(event, "response.output_item.added") == 0 || strcmp(event, "response.output_item.done") == 0) {
        JsonVal item = json_get(root, "item");
        char *type = json_get_string(item, "type");
        if (type && strcmp(type, "function_call") == 0) {
            int idx = responses_tool_index(sctx, root, item);
            if (idx < 0) { FREE_PTR(type); return; }
            OpenAIToolAccum *tool = streamctx_ensure_openai_tool(sctx, idx);
            char *id = json_get_string(item, "call_id");
            char *name = json_get_string(item, "name");
            char *args = json_get_string(item, "arguments");
            if (id && id[0]) { FREE_PTR(tool->id); tool->id = id; } else FREE_PTR(id);
            if (name && name[0]) { FREE_PTR(tool->name); tool->name = name; } else FREE_PTR(name);
            if (args && args[0]) { sb_truncate(&tool->arguments, 0); sb_append(&tool->arguments, args); }
            FREE_PTR(args);
        }
        FREE_PTR(type);
    } else if (strcmp(event, "response.function_call_arguments.delta") == 0) {
        int idx = responses_tool_index(sctx, root, json_get(root, "item"));
        if (idx < 0) return;
        OpenAIToolAccum *tool = streamctx_ensure_openai_tool(sctx, idx);
        char *delta = json_get_string(root, "delta");
        if (delta) { sb_append(&tool->arguments, delta); FREE_PTR(delta); }
    } else if (strcmp(event, "response.completed") == 0) {
        JsonVal response = json_get(root, "response");
        if (response.type == JSON_NULL) response = root;
        responses_record_usage(sctx, response);
        int has_tools = sctx->openai_tool_count > 0;
        streamctx_emit_openai_tool_calls(sctx);
        responses_emit_usage(sctx);
        emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, has_tools ? "tool_use" : "end_turn");
        sctx->responses_terminal = 1;
    } else if (strcmp(event, "response.failed") == 0 || strcmp(event, "response.incomplete") == 0 || strcmp(event, "error") == 0) {
        JsonVal response = json_get(root, "response");
        if (response.type == JSON_NULL) response = root;
        responses_record_usage(sctx, response);
        JsonVal error = json_get(response, "error");
        char *message = json_get_string(error, "message");
        if (!message) message = json_get_string(response, "message");
        if (!message) message = json_get_string(response, "reason");
        if (!message) message = util_strdup(strcmp(event, "response.incomplete") == 0 ? "Response incomplete" : (strcmp(event, "error") == 0 ? "Stream error" : "Response failed"));
        emit_simple_event(sctx->callback, sctx->ctx, SSE_ERROR, message);
        FREE_PTR(message);
        responses_emit_usage(sctx);
        emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "error");
        sctx->responses_terminal = 1;
    }
}

/* init/free StreamCtx (extracted from the old inline init) */
void sse_stream_init(StreamCtx *sctx, const char *provider,
                     sse_callback_fn callback, void *ctx,
                     volatile int *cancelled) {
    memset(sctx, 0, sizeof(*sctx));
    sctx->callback = callback;
    sctx->ctx = ctx;
    sctx->cancelled = cancelled;
    sctx->provider = (char *)provider;
}

void sse_stream_free(StreamCtx *sctx) {
    streamctx_free_openai_tools(sctx);
    sctx->callback = NULL;
    sctx->ctx = NULL;
    sctx->provider = NULL;
    sctx->cancelled = NULL;
}

/* ============================================================
 * HTTP request
 * ============================================================ */

/* forward declaration - defined before sse_parse_event */
static void emit_simple_event(sse_callback_fn callback, void *ctx,
                              SseEventType type, const char *content);

static int openai_cached_tokens(JsonVal usage) {
    int cached = json_get_int(usage, "cached_tokens");
    if (cached > 0) return cached;
    JsonVal details = json_get(usage, "prompt_tokens_details");
    if (details.type != JSON_NULL) cached = json_get_int(details, "cached_tokens");
    return cached;
}

static void fill_openai_usage_event(SseEvent *evt, JsonVal usage) {
    int prompt = json_get_int(usage, "prompt_tokens");
    int cached = openai_cached_tokens(usage);
    evt->out_tokens = json_get_int(usage, "completion_tokens");
    evt->cache_read_tokens = cached;
    if (prompt > 0) {
        evt->in_tokens = prompt - cached;
        if (evt->in_tokens < 0) evt->in_tokens = 0;
    }
}

/* handle non-SSE responses: parse the whole JSON body as a full reply */
static void process_residual_json(const char *residual, const char *provider,
                                  sse_callback_fn callback, void *ctx) {
    if (!residual || !residual[0]) return;
    while (*residual == ' ' || *residual == '\t' || *residual == '\r' || *residual == '\n') residual++;
    if (*residual != '{') return;

    size_t pos = 0;
    JsonParse jp = json_parse(residual, &pos);
    if (jp.error) return;

    char *err_msg = json_get_string(jp.val, "error");
    if (!err_msg) {
        /* provider error objects: {"error":{"message":...}} (claude) */
        JsonVal ev = json_get(jp.val, "error");
        if (ev.type == JSON_OBJECT)
            err_msg = json_get_string(ev, "message");
    }
    if (err_msg) {
        emit_simple_event(callback, ctx, SSE_ERROR, err_msg);
        free(err_msg);
    } else if (strcmp(provider, "claude") == 0) {
        JsonVal content = json_get(jp.val, "content");
        if (content.type == JSON_ARRAY) {
            int clen = json_array_len(content);
            for (int i = 0; i < clen; i++) {
                JsonVal block = json_array_get(content, i);
                char *btype = json_get_string(block, "type");
                if (btype && strcmp(btype, "text") == 0) {
                    char *txt = json_get_string(block, "text");
                    if (txt) { emit_simple_event(callback, ctx, SSE_TEXT, txt); free(txt); }
                } else if (btype && strcmp(btype, "thinking") == 0) {
                    char *txt = json_get_string(block, "thinking");
                    if (txt) { emit_simple_event(callback, ctx, SSE_THINKING, txt); free(txt); }
                } else if (btype && strcmp(btype, "tool_use") == 0) {
                    char *id = json_get_string(block, "id");
                    char *name = json_get_string(block, "name");
                    SseEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = SSE_TOOL_CALL;
                    evt.tool_id = id;
                    evt.tool_name = name;
                    evt.tool_input = "{}";
                    callback(ctx, &evt);
                    free(id); free(name);
                }
                free(btype);
            }
        }
        char *stop_reason = json_get_string(jp.val, "stop_reason");
        if (stop_reason) {
            emit_simple_event(callback, ctx, SSE_STOP, stop_reason);
            free(stop_reason);
        }
        JsonVal usage = json_get(jp.val, "usage");
        if (usage.type != JSON_NULL) {
            SseEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = SSE_USAGE;
            evt.in_tokens = json_get_int(usage, "input_tokens");
            evt.out_tokens = json_get_int(usage, "output_tokens");
            evt.cache_read_tokens = json_get_int(usage, "cache_read_input_tokens");
            evt.cache_creation_tokens = json_get_int(usage, "cache_creation_input_tokens");
            callback(ctx, &evt);
        }
    } else {
        JsonVal choices = json_get(jp.val, "choices");
        if (choices.type == JSON_ARRAY) {
            JsonVal choice = json_array_get(choices, 0);
            JsonVal msg = json_get(choice, "message");
            char *content = json_get_string(msg, "content");
            if (content) {
                emit_simple_event(callback, ctx, SSE_TEXT, content);
                free(content);
            }
            char *reasoning = json_get_string(msg, "reasoning_content");
            if (!reasoning) reasoning = json_get_string(msg, "reasoning");
            if (reasoning) {
                emit_simple_event(callback, ctx, SSE_THINKING, reasoning);
                free(reasoning);
            }
            JsonVal tool_calls = json_get(msg, "tool_calls");
            if (tool_calls.type == JSON_ARRAY) {
                int tc_len = json_array_len(tool_calls);
                for (int i = 0; i < tc_len; i++) {
                    JsonVal tc = json_array_get(tool_calls, i);
                    JsonVal fn = json_get(tc, "function");
                    char *id = json_get_string(tc, "id");
                    char *name = json_get_string(fn, "name");
                    char *arguments = json_get_string(fn, "arguments");
                    SseEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = SSE_TOOL_CALL;
                    evt.tool_id = id;
                    evt.tool_name = name;
                    evt.tool_input = arguments ? arguments : (char *)"{}";
                    callback(ctx, &evt);
                    free(id);
                    free(name);
                    free(arguments);
                }
            }
            char *finish = json_get_string(choice, "finish_reason");
            if (finish) {
                if (strcmp(finish, "tool_calls") == 0) emit_simple_event(callback, ctx, SSE_STOP, "tool_use");
                else if (strcmp(finish, "stop") == 0) emit_simple_event(callback, ctx, SSE_STOP, "end_turn");
                else emit_simple_event(callback, ctx, SSE_STOP, finish);
                free(finish);
            }
        }
        JsonVal usage = json_get(jp.val, "usage");
        if (usage.type != JSON_NULL) {
            SseEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = SSE_USAGE;
            fill_openai_usage_event(&evt, usage);
            callback(ctx, &evt);
        }
    }
}


/* ============================================================
 * SSE event parsing
 * ============================================================ */

static void emit_simple_event(sse_callback_fn callback, void *ctx,
                              SseEventType type, const char *content) {
    SseEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.content = (char *)content;  /* borrowed, not freed */
    callback(ctx, &evt);
}

int sse_parse_event(const char *provider, const char *data, size_t data_len,
                    sse_callback_fn callback, void *ctx) {
    if (data_len == 0) return 0;
    if (strcmp(data, "[DONE]") == 0) {
        if (strcmp(provider, "claude") == 0) emit_simple_event(callback, ctx, SSE_STOP, "end_turn");
        return 0;
    }

    /* parse JSON */
    size_t pos = 0;
    JsonParse jp = json_parse(data, &pos);
    if (jp.error) return 0;

    if (strcmp(provider, "claude") == 0) {
        /* Claude SSE format */
        char *type = json_get_string(jp.val, "type");
        if (!type) return 0;

        if (strcmp(type, "content_block_delta") == 0) {
            JsonVal delta = json_get(jp.val, "delta");
            char *dtype = json_get_string(delta, "type");
            if (dtype && strcmp(dtype, "text_delta") == 0) {
                char *text = json_get_string(delta, "text");
                if (text) { emit_simple_event(callback, ctx, SSE_TEXT, text); free(text); }
            } else if (dtype && strcmp(dtype, "thinking_delta") == 0) {
                char *text = json_get_string(delta, "thinking");
                if (text) { emit_simple_event(callback, ctx, SSE_THINKING, text); free(text); }
            } else if (dtype && strcmp(dtype, "input_json_delta") == 0) {
                /* tool-call input delta */
                char *partial = json_get_string(delta, "partial_json");
                if (partial) {
                    SseEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = SSE_TOOL_INPUT_DELTA;
                    evt.content = partial;
                    evt.tool_id = NULL; /* index is used for matching */
                    callback(ctx, &evt);
                    free(partial);
                }
            }
            free(dtype);
        } else if (strcmp(type, "content_block_start") == 0) {
            JsonVal cb = json_get(jp.val, "content_block");
            char *cb_type = json_get_string(cb, "type");
            if (cb_type && strcmp(cb_type, "tool_use") == 0) {
                char *id = json_get_string(cb, "id");
                char *name = json_get_string(cb, "name");
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_TOOL_CALL_START;
                evt.tool_id = id;
                evt.tool_name = name;
                callback(ctx, &evt);
                /* copied in the callback; free here */
                free(id);
                free(name);
            }
            free(cb_type);
        } else if (strcmp(type, "content_block_stop") == 0) {
            /* tool call complete - the accumulator handles it after stop */
        } else if (strcmp(type, "message_delta") == 0) {
            JsonVal delta = json_get(jp.val, "delta");
            char *stop_reason = json_get_string(delta, "stop_reason");
            if (stop_reason) {
                emit_simple_event(callback, ctx, SSE_STOP, stop_reason);
                free(stop_reason);
            }
            JsonVal usage = json_get(jp.val, "usage");
            if (usage.type != JSON_NULL) {
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_USAGE;
                evt.out_tokens = json_get_int(usage, "output_tokens");
                /* input/cache_* only when message_start did not provide them (Rust parity);
                 * the OpenAI path has no message_start; transport synthesizes it */
                int it = json_get_int(usage, "input_tokens");
                int cr = json_get_int(usage, "cache_read_input_tokens");
                int cc = json_get_int(usage, "cache_creation_input_tokens");
                if (it > 0) evt.in_tokens = it;
                if (cr > 0) evt.cache_read_tokens = cr;
                if (cc > 0) evt.cache_creation_tokens = cc;
                callback(ctx, &evt);
            }
        } else if (strcmp(type, "message_start") == 0) {
            JsonVal msg = json_get(jp.val, "message");
            JsonVal usage = json_get(msg, "usage");
            if (usage.type != JSON_NULL) {
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_USAGE;
                evt.in_tokens = json_get_int(usage, "input_tokens");
                evt.cache_read_tokens = json_get_int(usage, "cache_read_input_tokens");
                evt.cache_creation_tokens = json_get_int(usage, "cache_creation_input_tokens");
                callback(ctx, &evt);
            }
        } else if (strcmp(type, "error") == 0) {
            /* Claude errors look like {"type":"error","error":{...}} */
            char *msg = NULL;
            JsonVal err_obj = json_get(jp.val, "error");
            if (err_obj.type == JSON_OBJECT)
                msg = json_get_string(err_obj, "message");
            if (!msg) msg = json_get_string(jp.val, "error");
            if (!msg) msg = json_get_string(jp.val, "message");
            emit_simple_event(callback, ctx, SSE_ERROR, msg ? msg : "unknown error");
            free(msg);
        }
        free(type);
    } else {
        /* OpenAI SSE format */
        char *obj_type = json_get_string(jp.val, "object");
        if (!obj_type) return 0;

        if (strcmp(obj_type, "chat.completion.chunk") == 0) {
            JsonVal choices = json_get(jp.val, "choices");
            if (choices.type == JSON_ARRAY) {
                JsonVal choice = json_array_get(choices, 0);
                JsonVal delta = json_get(choice, "delta");
                char *content = json_get_string(delta, "content");
                if (content) {
                    emit_simple_event(callback, ctx, SSE_TEXT, content);
                    free(content);
                }
                char *reasoning = json_get_string(delta, "reasoning_content");
                if (!reasoning) reasoning = json_get_string(delta, "reasoning");
                if (reasoning) {
                    emit_simple_event(callback, ctx, SSE_THINKING, reasoning);
                    free(reasoning);
                }
                JsonVal tool_calls = json_get(delta, "tool_calls");
                if (tool_calls.type == JSON_ARRAY) {
                            int tc_len = json_array_len(tool_calls);
                            for (int i = 0; i < tc_len; i++) {
                                JsonVal tc = json_array_get(tool_calls, i);
                                JsonVal fn = json_get(tc, "function");
                                char *id = json_get_string(tc, "id");
                                char *name = json_get_string(fn, "name");
                        char *arguments = json_get_string(fn, "arguments");
                        if (id || name) {
                            SseEvent evt;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = SSE_TOOL_CALL_START;
                            evt.tool_id = id;
                            evt.tool_name = name;
                            callback(ctx, &evt);
                        }
                        if (arguments) {
                            SseEvent evt;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = SSE_TOOL_INPUT_DELTA;
                            evt.content = arguments;
                            callback(ctx, &evt);
                        }
                        free(id);
                        free(name);
                        free(arguments);
                    }
                }
                char *finish = json_get_string(choice, "finish_reason");
                if (finish) {
                    if (strcmp(finish, "tool_calls") == 0) {
                        emit_simple_event(callback, ctx, SSE_STOP, "tool_use");
                    } else if (strcmp(finish, "stop") == 0) {
                        emit_simple_event(callback, ctx, SSE_STOP, "end_turn");
                    } else if (strcmp(finish, "length") == 0) {
                        emit_simple_event(callback, ctx, SSE_STOP, "max_tokens");
                    } else {
                        emit_simple_event(callback, ctx, SSE_STOP, finish);
                    }
                    free(finish);
                }
            }
            JsonVal usage = json_get(jp.val, "usage");
            if (usage.type != JSON_NULL) {
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_USAGE;
                fill_openai_usage_event(&evt, usage);
                callback(ctx, &evt);
            }
        }
        free(obj_type);
    }
    return 0;
}

/* ============================================================
 * SSE accumulator
 * ============================================================ */

void sse_accum_init(SseAccumulator *acc) {
    memset(acc, 0, sizeof(*acc));
    sb_init(&acc->text);
    sb_init(&acc->thinking);
    acc->tool_cap = 8;
    acc->tools = calloc(acc->tool_cap, sizeof(ToolCallAccum));
    acc->tool_count = 0;
    acc->current_block_index = -1;
}

void sse_accum_free(SseAccumulator *acc) {
    sb_free(&acc->text);
    sb_free(&acc->thinking);
    for (int i = 0; i < acc->tool_count; i++) {
        FREE_PTR(acc->tools[i].id);
        FREE_PTR(acc->tools[i].name);
        sb_free(&acc->tools[i].input_json);
    }
    free(acc->tools);
    FREE_PTR(acc->current_block_type);
    FREE_PTR(acc->current_tool_id);
    FREE_PTR(acc->current_tool_name);
    FREE_PTR(acc->stop_reason);
    FREE_PTR(acc->error);
}

void sse_accum_callback(void *ctx, const SseEvent *evt) {
    SseAccumulator *acc = (SseAccumulator *)ctx;

    switch (evt->type) {
    case SSE_TEXT:
        sb_append(&acc->text, evt->content);
        break;

    case SSE_THINKING:
        sb_append(&acc->thinking, evt->content);
        break;

    case SSE_TOOL_CALL_START: {
        if (acc->tool_count >= acc->tool_cap) {
            acc->tool_cap *= 2;
            acc->tools = realloc(acc->tools, acc->tool_cap * sizeof(ToolCallAccum));
        }
        ToolCallAccum *tc = &acc->tools[acc->tool_count];
        memset(tc, 0, sizeof(*tc));
        sb_init(&tc->input_json);
        tc->id = util_strdup(evt->tool_id);
        tc->name = util_strdup(evt->tool_name);
        acc->tool_count++;
        break;
    }

    case SSE_TOOL_INPUT_DELTA: {
        if (acc->tool_count > 0 && evt->content) {
            sb_append(&acc->tools[acc->tool_count - 1].input_json, evt->content);
        }
        break;
    }

    case SSE_TOOL_CALL: {
        /* complete tool call (non-streaming, e.g. OpenAI) */
        if (acc->tool_count >= acc->tool_cap) {
            acc->tool_cap *= 2;
            acc->tools = realloc(acc->tools, acc->tool_cap * sizeof(ToolCallAccum));
        }
        ToolCallAccum *tc = &acc->tools[acc->tool_count];
        memset(tc, 0, sizeof(*tc));
        tc->id = util_strdup(evt->tool_id ? evt->tool_id : "");
        tc->name = util_strdup(evt->tool_name ? evt->tool_name : "");
        sb_init(&tc->input_json);
        sb_append(&tc->input_json, evt->tool_input ? evt->tool_input : "{}");
        acc->tool_count++;
        break;
    }

    case SSE_USAGE:
        if (evt->in_tokens > 0) acc->in_tokens = evt->in_tokens;
        if (evt->out_tokens > 0) acc->out_tokens = evt->out_tokens;
        if (evt->cache_read_tokens > 0) acc->cache_read_tokens = evt->cache_read_tokens;
        if (evt->cache_creation_tokens > 0) acc->cache_creation_tokens = evt->cache_creation_tokens;
        break;

    case SSE_STOP:
        acc->stopped = 1;
        if (evt->content) {
            FREE_PTR(acc->stop_reason);
            acc->stop_reason = util_strdup(evt->content);
        }
        break;

    case SSE_ERROR:
        FREE_PTR(acc->error);
        acc->error = util_strdup(evt->content ? evt->content : "unknown error");
        break;

    case SSE_RETRY:
        /* reset current accumulation (stream_display_callback parity) */
        sb_truncate(&acc->text, 0);
        sb_truncate(&acc->thinking, 0);
        for (int i = 0; i < acc->tool_count; i++) {
            FREE_PTR(acc->tools[i].id);
            FREE_PTR(acc->tools[i].name);
            sb_free(&acc->tools[i].input_json);
        }
        acc->tool_count = 0;
        acc->stopped = 0;
        FREE_PTR(acc->stop_reason);
        acc->in_tokens = 0;
        acc->out_tokens = 0;
        acc->cache_read_tokens = 0;
        acc->cache_creation_tokens = 0;
        break;
    }
}

/* ============================================================
 * request body building
 * ============================================================ */

char *build_claude_request(const char *model, const char *system_prompt,
                           const char *tools_json,
                           char **conv_lines, int conv_line_count,
                           int max_tokens, const char *thinking, const char *effort) {
    StrBuf buf;
    sb_init(&buf);

    /* field order matches the Go/Rust map alphabetical order:
     * max_tokens → messages → model → output_config → stream → system → thinking → tools */
    sb_append(&buf, "{\"max_tokens\":");
    sb_appendf(&buf, "%d", max_tokens);

    /* messages */
    sb_append(&buf, ",\"messages\":[");
    for (int i = 0; i < conv_line_count; i++) {
        if (i > 0) sb_append(&buf, ",");
        sb_append(&buf, conv_lines[i]);
    }
    sb_append(&buf, "]");

    /* model */
    sb_append(&buf, ",\"model\":");
    sb_append_json_string(&buf, model);

    /* output_config (only when thinking != disabled) */
    if (thinking && strcmp(thinking, "disabled") != 0) {
        sb_append(&buf, ",\"output_config\":{\"effort\":");
        sb_append_json_string(&buf, effort ? effort : "high");
        sb_append(&buf, "}");
    }

    /* stream */
    sb_append(&buf, ",\"stream\":true");

    /* system prompt */
    if (system_prompt && system_prompt[0]) {
        sb_append(&buf, ",\"system\":");
        sb_append_json_string(&buf, system_prompt);
    }

    /* thinking (only when thinking != disabled) */
    if (thinking && strcmp(thinking, "disabled") != 0) {
        sb_append(&buf, ",\"thinking\":{\"type\":");
        sb_append_json_string(&buf, thinking);
        sb_append(&buf, "}");
    }

    /* tools */
    if (tools_json) {
        sb_append(&buf, ",\"tools\":");
        sb_append(&buf, tools_json);
    }

    sb_append(&buf, "}");

    char *result = buf.data;
    /* no sb_free: we return buf.data */
    return result;
}

static void sb_append_json_val(StrBuf *sb, JsonVal v) {
    if (v.type == JSON_NULL || !v.src) {
        sb_append(sb, "null");
        return;
    }
    sb_appendn(sb, v.src + v.start, v.end - v.start);
}

static void openai_convert_tools(StrBuf *out, JsonVal tools_val) {
    if (tools_val.type != JSON_ARRAY) {
        sb_append(out, "[]");
        return;
    }
    sb_append_char(out, '[');
    int n = json_array_len(tools_val);
    for (int i = 0; i < n; i++) {
        JsonVal td = json_array_get(tools_val, i);
        if (i > 0) sb_append_char(out, ',');
        char *type = json_get_string(td, "type");
        if (type && strcmp(type, "function") == 0) {
            sb_append_json_val(out, td);
            FREE_PTR(type);
            continue;
        }
        FREE_PTR(type);
        char *name = json_get_string(td, "name");
        char *desc = json_get_string(td, "description");
        JsonVal params = json_get(td, "input_schema");
        if (params.type == JSON_NULL) params = json_get(td, "parameters");
        sb_append(out, "{\"type\":\"function\",\"function\":{\"name\":");
        sb_append_json_string(out, name ? name : "");
        sb_append(out, ",\"description\":");
        sb_append_json_string(out, desc ? desc : "");
        sb_append(out, ",\"parameters\":");
        if (params.type == JSON_NULL) sb_append(out, "{}");
        else sb_append_json_val(out, params);
        sb_append(out, "}}");
        FREE_PTR(name);
        FREE_PTR(desc);
    }
    sb_append_char(out, ']');
}

static void openai_convert_assistant_message(StrBuf *out, JsonVal content_val) {
    StrBuf text, reasoning, tool_calls;
    sb_init(&text);
    sb_init(&reasoning);
    sb_init(&tool_calls);

    int n = json_array_len(content_val);
    for (int i = 0; i < n; i++) {
        JsonVal block = json_array_get(content_val, i);
        char *btype = json_get_string(block, "type");
        if (!btype) continue;
        if (strcmp(btype, "thinking") == 0) {
            char *t = json_get_string(block, "thinking");
            if (t) { sb_append(&reasoning, t); FREE_PTR(t); }
        } else if (strcmp(btype, "text") == 0) {
            char *t = json_get_string(block, "text");
            if (t) { sb_append(&text, t); FREE_PTR(t); }
        } else if (strcmp(btype, "tool_use") == 0) {
            char *id = json_get_string(block, "id");
            char *name = json_get_string(block, "name");
            JsonVal input = json_get(block, "input");
            if (tool_calls.len > 0) sb_append_char(&tool_calls, ',');
            sb_append(&tool_calls, "{\"id\":");
            sb_append_json_string(&tool_calls, id ? id : "");
            sb_append(&tool_calls, ",\"type\":\"function\",\"function\":{\"name\":");
            sb_append_json_string(&tool_calls, name ? name : "");
            sb_append(&tool_calls, ",\"arguments\":");
            if (input.type == JSON_NULL) sb_append_json_string(&tool_calls, "{}");
            else {
                StrBuf arg;
                sb_init(&arg);
                sb_append_json_val(&arg, input);
                sb_append_json_string(&tool_calls, arg.data ? arg.data : "{}");
                sb_free(&arg);
            }
            sb_append(&tool_calls, "}}");
            FREE_PTR(id);
            FREE_PTR(name);
        }
        FREE_PTR(btype);
    }

    sb_append(out, "{\"role\":\"assistant\",\"reasoning_content\":");
    sb_append_json_string(out, reasoning.data ? reasoning.data : "");
    sb_append(out, ",\"content\":");
    sb_append_json_string(out, text.data ? text.data : "");
    if (tool_calls.len > 0) {
        sb_append(out, ",\"tool_calls\":[");
        sb_append(out, tool_calls.data);
        sb_append_char(out, ']');
    }
    sb_append_char(out, '}');

    sb_free(&text);
    sb_free(&reasoning);
    sb_free(&tool_calls);
}

static int openai_convert_tool_results(StrBuf *out, JsonVal content_val) {
    int written = 0;
    int n = json_array_len(content_val);
    for (int i = 0; i < n; i++) {
        JsonVal block = json_array_get(content_val, i);
        char *btype = json_get_string(block, "type");
        if (!btype || strcmp(btype, "tool_result") != 0) {
            FREE_PTR(btype);
            continue;
        }
        char *tool_use_id = json_get_string(block, "tool_use_id");
        char *content = json_get_string(block, "content");
        if (written > 0) sb_append_char(out, ',');
        sb_append(out, "{\"role\":\"tool\",\"tool_call_id\":");
        sb_append_json_string(out, tool_use_id ? tool_use_id : "");
        sb_append(out, ",\"content\":");
        sb_append_json_string(out, content ? content : "");
        sb_append_char(out, '}');
        written++;
        FREE_PTR(tool_use_id);
        FREE_PTR(content);
        FREE_PTR(btype);
    }
    return written;
}

static void openai_convert_messages(StrBuf *out, JsonVal messages_val) {
    sb_append_char(out, '[');
    int wrote = 0;
    int n = json_array_len(messages_val);
    for (int i = 0; i < n; i++) {
        JsonVal msg = json_array_get(messages_val, i);
        char *role = json_get_string(msg, "role");
        JsonVal content = json_get(msg, "content");
        if (role && strcmp(role, "assistant") == 0 && content.type == JSON_ARRAY) {
            if (wrote > 0) sb_append_char(out, ',');
            openai_convert_assistant_message(out, content);
            wrote++;
        } else if (role && strcmp(role, "user") == 0 && content.type == JSON_ARRAY) {
            int before = wrote;
            if (wrote > 0 && json_array_len(content) > 0) {
                /* openai_convert_tool_results handles commas after the first item */
            }
            if (wrote > 0) {
                StrBuf tmp;
                sb_init(&tmp);
                int tool_written = openai_convert_tool_results(&tmp, content);
                if (tool_written > 0) {
                    sb_append_char(out, ',');
                    sb_append(out, tmp.data);
                    wrote += tool_written;
                } else {
                    if (wrote > 0) sb_append_char(out, ',');
                    sb_append_json_val(out, msg);
                    wrote++;
                }
                sb_free(&tmp);
            } else {
                int tool_written = openai_convert_tool_results(out, content);
                if (tool_written > 0) wrote += tool_written;
                else {
                    sb_append_json_val(out, msg);
                    wrote++;
                }
            }
            (void)before;
        } else {
            if (wrote > 0) sb_append_char(out, ',');
            sb_append_json_val(out, msg);
            wrote++;
        }
        FREE_PTR(role);
    }
    sb_append_char(out, ']');
}

char *convert_to_openai(const char *claude_body) {
    JsonParse jp = json_parse_root(claude_body);
    if (jp.error) return util_strdup(claude_body);

    char *model = json_get_string(jp.val, "model");
    int max_tokens = json_get_int(jp.val, "max_tokens");
    JsonVal system_val = json_get(jp.val, "system");
    JsonVal thinking_val = json_get(jp.val, "thinking");
    JsonVal output_config_val = json_get(jp.val, "output_config");
    JsonVal messages_val = json_get(jp.val, "messages");
    JsonVal tools_val = json_get(jp.val, "tools");

    StrBuf messages, tools, result;
    sb_init(&messages);
    sb_init(&tools);
    sb_init(&result);

    openai_convert_messages(&messages, messages_val);
    if (tools_val.type == JSON_ARRAY && json_array_len(tools_val) > 0) {
        openai_convert_tools(&tools, tools_val);
    }

    sb_append(&result, "{\"model\":");
    sb_append_json_string(&result, model ? model : "");
    sb_append(&result, ",\"max_tokens\":");
    sb_appendf(&result, "%d", max_tokens);
    sb_append(&result, ",\"stream\":true,\"stream_options\":{\"include_usage\":true}");

    if (system_val.type != JSON_NULL) {
        char *sys = json_as_string(system_val);
        if (sys && sys[0]) {
            StrBuf with_system;
            sb_init(&with_system);
            sb_append(&with_system, "[{\"role\":\"system\",\"content\":");
            sb_append_json_string(&with_system, sys);
            sb_append_char(&with_system, '}');
            if (messages.len > 2) {
                sb_append_char(&with_system, ',');
                sb_appendn(&with_system, messages.data + 1, messages.len - 2);
            }
            sb_append_char(&with_system, ']');
            sb_free(&messages);
            messages = with_system;
        }
        FREE_PTR(sys);
    }

    char *thinking_type = json_get_string(thinking_val, "type");
    if (thinking_type &&
        (strcmp(thinking_type, "adaptive") == 0 || strcmp(thinking_type, "enabled") == 0)) {
        sb_append(&result, ",\"thinking\":{\"type\":\"enabled\"}");
        char *effort = json_get_string(output_config_val, "effort");
        sb_append(&result, ",\"reasoning_effort\":");
        sb_append_json_string(&result, (effort && effort[0]) ? effort : "high");
        FREE_PTR(effort);
    }
    FREE_PTR(thinking_type);

    if (tools.len > 0 && strcmp(tools.data, "[]") != 0) {
        sb_append(&result, ",\"tools\":");
        sb_append(&result, tools.data);
    }

    sb_append(&result, ",\"messages\":");
    sb_append(&result, messages.data ? messages.data : "[]");
    sb_append_char(&result, '}');

    FREE_PTR(model);
    sb_free(&messages);
    sb_free(&tools);
    return result.data;
}


static void responses_convert_tools(StrBuf *out, JsonVal tools_val) {
    sb_append_char(out, '[');
    int wrote = 0;
    int n = json_array_len(tools_val);
    for (int i = 0; i < n; i++) {
        JsonVal tool = json_array_get(tools_val, i);
        char *name = json_get_string(tool, "name");
        if (!name || !name[0]) { FREE_PTR(name); continue; }
        char *desc = json_get_string(tool, "description");
        JsonVal parameters = json_get(tool, "input_schema");
        if (parameters.type == JSON_NULL) parameters = json_get(tool, "parameters");
        if (wrote++) sb_append_char(out, ',');
        sb_append(out, "{\"type\":\"function\",\"name\":");
        sb_append_json_string(out, name);
        sb_append(out, ",\"description\":");
        sb_append_json_string(out, desc ? desc : "");
        sb_append(out, ",\"parameters\":");
        if (parameters.type == JSON_NULL) sb_append(out, "{}"); else sb_append_json_val(out, parameters);
        sb_append_char(out, '}');
        FREE_PTR(name); FREE_PTR(desc);
    }
    sb_append_char(out, ']');
}

static void responses_convert_messages(StrBuf *out, JsonVal messages_val) {
    sb_append_char(out, '[');
    int wrote = 0;
    int n = json_array_len(messages_val);
    for (int i = 0; i < n; i++) {
        JsonVal msg = json_array_get(messages_val, i);
        char *role = json_get_string(msg, "role");
        JsonVal content = json_get(msg, "content");
        if (role && strcmp(role, "assistant") == 0 && content.type == JSON_ARRAY) {
            StrBuf text; sb_init(&text);
            int blocks = json_array_len(content);
            for (int j = 0; j < blocks; j++) {
                JsonVal block = json_array_get(content, j);
                char *type = json_get_string(block, "type");
                if (type && strcmp(type, "text") == 0) {
                    char *value = json_get_string(block, "text");
                    if (value) { sb_append(&text, value); FREE_PTR(value); }
                } else if (type && strcmp(type, "tool_use") == 0) {
                    char *id = json_get_string(block, "id");
                    char *name = json_get_string(block, "name");
                    JsonVal input = json_get(block, "input");
                    if (wrote++) sb_append_char(out, ',');
                    sb_append(out, "{\"type\":\"function_call\",\"call_id\":"); sb_append_json_string(out, id ? id : "");
                    sb_append(out, ",\"name\":"); sb_append_json_string(out, name ? name : "");
                    sb_append(out, ",\"arguments\":");
                    StrBuf args; sb_init(&args); if (input.type == JSON_NULL) sb_append(&args, "{}"); else sb_append_json_val(&args, input);
                    sb_append_json_string(out, args.data ? args.data : "{}"); sb_free(&args); sb_append_char(out, '}');
                    FREE_PTR(id); FREE_PTR(name);
                }
                FREE_PTR(type);
            }
            if (text.len > 0) { if (wrote++) sb_append_char(out, ','); sb_append(out, "{\"role\":\"assistant\",\"content\":"); sb_append_json_string(out, text.data); sb_append_char(out, '}'); }
            sb_free(&text);
        } else if (role && strcmp(role, "user") == 0 && content.type == JSON_ARRAY) {
            int blocks = json_array_len(content);
            for (int j = 0; j < blocks; j++) {
                JsonVal block = json_array_get(content, j);
                char *type = json_get_string(block, "type");
                if (type && strcmp(type, "tool_result") == 0) {
                    char *id = json_get_string(block, "tool_use_id");
                    char *value = json_get_string(block, "content");
                    if (wrote++)
                        sb_append_char(out, ',');
                    sb_append(out, "{\"type\":\"function_call_output\",\"call_id\":");
                    sb_append_json_string(out, id ? id : "");
                    sb_append(out, ",\"output\":");
                    sb_append_json_string(out, value ? value : "");
                    sb_append_char(out, '}');
                    FREE_PTR(id); FREE_PTR(value);
                } else if (type && strcmp(type, "text") == 0) {
                    char *value = json_get_string(block, "text"); if (wrote++) sb_append_char(out, ','); sb_append(out, "{\"role\":\"user\",\"content\":"); sb_append_json_string(out, value ? value : ""); sb_append_char(out, '}'); FREE_PTR(value);
                }
                FREE_PTR(type);
            }
        } else { if (wrote++) sb_append_char(out, ','); sb_append_json_val(out, msg); }
        FREE_PTR(role);
    }
    sb_append_char(out, ']');
}

char *convert_to_responses(const char *claude_body) {
    JsonParse jp = json_parse_root(claude_body);
    if (jp.error) return util_strdup(claude_body);
    char *model = json_get_string(jp.val, "model");
    int max_tokens = json_get_int(jp.val, "max_tokens");
    JsonVal system = json_get(jp.val, "system");
    JsonVal thinking = json_get(jp.val, "thinking");
    JsonVal output = json_get(jp.val, "output_config");
    JsonVal messages = json_get(jp.val, "messages");
    JsonVal tools_val = json_get(jp.val, "tools");
    StrBuf input, tools, result; sb_init(&input); sb_init(&tools); sb_init(&result);
    responses_convert_messages(&input, messages);
    if (tools_val.type == JSON_ARRAY && json_array_len(tools_val)) responses_convert_tools(&tools, tools_val);
    sb_append(&result, "{\"model\":"); sb_append_json_string(&result, model ? model : "");
    sb_append(&result, ",\"input\":"); sb_append(&result, input.data ? input.data : "[]");
    sb_appendf(&result, ",\"max_output_tokens\":%d,\"stream\":true", max_tokens);
    if (system.type != JSON_NULL) { char *value = json_as_string(system); if (value && value[0]) { sb_append(&result, ",\"instructions\":"); sb_append_json_string(&result, value); } FREE_PTR(value); }
    char *thinking_type = json_get_string(thinking, "type");
    if (thinking_type && (!strcmp(thinking_type, "adaptive") || !strcmp(thinking_type, "enabled"))) { char *effort = json_get_string(output, "effort"); sb_append(&result, ",\"reasoning\":{\"effort\":"); sb_append_json_string(&result, effort && effort[0] ? effort : "high"); sb_append_char(&result, '}'); FREE_PTR(effort); }
    FREE_PTR(thinking_type);
    if (tools.len && strcmp(tools.data, "[]")) { sb_append(&result, ",\"tools\":"); sb_append(&result, tools.data); }
    sb_append_char(&result, '}');
    FREE_PTR(model); sb_free(&input); sb_free(&tools);
    return result.data;
}

/* ==== ba_prompt.c ==== */
/*
 * ba_prompt.c - system prompt construction, ported verbatim from
 * bash-agent's agent_build_prompt suite (agent.c). Section order, XML
 * tags and section text are kept identical; adaptations are limited to:
 *   - agent identity string: bash-agent -> busyagent
 *   - skill dirs: .claude/skills dropped, replaced by generic agent dirs
 *     (cwd/skills, ~/.agents/skills, $BA_HOME/skills)
 *   - Bash background / SubAgent sync guidance lines match busyagent's
 *     actual single-turn semantics
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include <dirent.h>
#include <sys/utsname.h>

/* ============================================================
 * system prompt building - helpers (ported verbatim)
 * ============================================================ */

/* append an XML section: <tag>... or <tag name=...> */
static void prompt_append_attr_escaped(StrBuf *buf, const char *src) {
    if (!src) return;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"':  sb_append(buf, "\\\""); break;
            case '\\': sb_append(buf, "\\\\"); break;
            case '\b': sb_append(buf, "\\b"); break;
            case '\f': sb_append(buf, "\\f"); break;
            case '\n': sb_append(buf, "\\n"); break;
            case '\r': sb_append(buf, "\\r"); break;
            case '\t': sb_append(buf, "\\t"); break;
            default:
                if (c < 0x20) sb_appendf(buf, "\\u%04x", c);
                else sb_append_char(buf, c);
                break;
        }
    }
}

static void prompt_append_section(StrBuf *buf, const char *tag,
                                   const char *content, const char *name) {
    if (!content || !content[0]) return;
    size_t content_len = strlen(content);
    while (content_len > 0 &&
           (content[content_len - 1] == '\n' || content[content_len - 1] == '\r')) {
        content_len--;
    }
    if (content_len == 0) return;
    if (name && name[0]) {
        sb_appendf(buf, "<%s name=\"", tag);
        prompt_append_attr_escaped(buf, name);
        sb_append(buf, "\">\n");
        sb_appendn(buf, content, content_len);
        sb_appendf(buf, "\n</%s>\n", tag);
    } else {
        sb_appendf(buf, "<%s>\n", tag);
        sb_appendn(buf, content, content_len);
        sb_appendf(buf, "\n</%s>\n", tag);
    }
}

/* util_path_join equivalent (busybox has no such helper) */
static char *pj(const char *a, const char *b) {
    return xasprintf("%s/%s", a, b);
}

/* util_read_file equivalent */
static char *read_all(const char *path) {
    int fd = open(path, O_RDONLY);
    long sz;
    char *buf;
    if (fd < 0)
        return NULL;
    sz = xlseek(fd, 0, SEEK_END);
    xlseek(fd, 0, SEEK_SET);
    buf = xzalloc(sz + 1);
    sz = full_read(fd, buf, sz);
    if (sz < 0) sz = 0;
    buf[sz] = '\0';
    close(fd);
    return buf;
}

/* locale detection: LC_ALL -> LC_MESSAGES -> LANG -> "en_US", strip .xxx */
static const char *detect_locale(void) {
    static char buf[128];
    const char *loc = getenv("LC_ALL");
    if (!loc || !loc[0]) loc = getenv("LC_MESSAGES");
    if (!loc || !loc[0]) loc = getenv("LANG");
    if (!loc || !loc[0]) loc = "en_US";
    strncpy(buf, loc, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    {
        char *dot = strchr(buf, '.');
        if (dot) *dot = '\0';
    }
    return buf;
}

/* find an instructions file in dir (AGENTS.md / AGENT.md; generic agent dirs),
 * Returns malloc'd content or NULL. bash-agent also looks for CLAUDE.md
 * variants; this project does not bind Claude dirs. */
static char *find_instruction_file(const char *dir) {
    const char *candidates[] = { "AGENTS.md", "AGENT.md", NULL };
    int i;

    for (i = 0; candidates[i]; i++) {
        char *path = pj(dir, candidates[i]);
        char *content = read_all(path);
        free(path);
        if (content && content[0])
            return content;
        free(content);
    }
    return NULL;
}

/* extract a summary from SKILL.md: prefer description:, else first non-empty non-heading non--- line */
static void extract_skill_summary(const char *md, StrBuf *out) {
    const char *p = md;
    char line[1024];
    int found = 0;
    char fallback[1024] = "";

    while (*p) {
        /* read one line */
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(line) - 1) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') p++;

        /* trim */
        {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            {
                char *e = s + strlen(s);
                while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
                *e = '\0';
                if (*s == '\0') continue;

                /* description: line */
                if (strncmp(s, "description:", 12) == 0) {
                    char *val = s + 12;
                    while (*val == ' ' || *val == '\t') val++;
                    /* strip quotes */
                    size_t vl = strlen(val);
                    if (vl >= 2 && ((val[0] == '"' && val[vl-1] == '"') ||
                                    (val[0] == '\'' && val[vl-1] == '\''))) {
                        val++;
                        vl -= 2;
                    }
                    sb_appendn(out, val, vl);
                    found = 1;
                    return;
                }
                /* fallback: not a heading, not ---, not ``` */
                if (!found && fallback[0] == '\0' && s[0] != '#' &&
                    !(s[0] == '-' && s[1] == '-' && s[2] == '-' && s[3] == '\0') &&
                    !(s[0] == '`' && s[1] == '`' && s[2] == '`')) {
                    strncpy(fallback, s, sizeof(fallback) - 1);
                    fallback[sizeof(fallback) - 1] = '\0';
                }
            }
        }
    }
    if (!found && fallback[0]) {
        sb_append(out, fallback);
    }
}

/* scan the skill dir list (dedup) and build the skill-index.
 * order: cwd/skills > $HOME/.agents/skills > bag_home/skills */
static void build_skill_index(StrBuf *index, const char *cwd,
                              const char *agents_home, const char *bag_home) {
    char *dirs[4];
    int dcount = 0;
    {
        dirs[dcount++] = xasprintf("%s/skills", cwd);
        if (agents_home && agents_home[0])
            dirs[dcount++] = xasprintf("%s/.agents/skills", agents_home);
        if (bag_home && bag_home[0])
            dirs[dcount++] = xasprintf("%s/skills", bag_home);
    }

    /* dedup via the seen list */
    char *seen[256];
    int seen_count = 0;

    for (int d = 0; d < dcount; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            /* already seen? */
            int dup = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen[s], ent->d_name) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            /* SKILL.md present? */
            char *skill_md = xasprintf("%s/%s/SKILL.md", dirs[d], ent->d_name);
            char *md_content = read_all(skill_md);
            free(skill_md);
            if (!md_content || !md_content[0]) { free(md_content); continue; }

            /* mark seen */
            if (seen_count < 256) seen[seen_count++] = util_strdup(ent->d_name);

            /* extract the summary */
            StrBuf summary;
            sb_init(&summary);
            extract_skill_summary(md_content, &summary);
            free(md_content);

            sb_appendf(index, "- %s", ent->d_name);
            if (summary.len > 0) sb_appendf(index, ": %s", summary.data);
            sb_append_char(index, '\n');
            sb_free(&summary);
        }
        closedir(dir);
    }
    for (int d = 0; d < dcount; d++) free(dirs[d]);
    for (int s = 0; s < seen_count; s++) free(seen[s]);
}

char *ba_load_skill(const char *skill_name, const char *cwd,
                    const char *agents_home, const char *bag_home,
                    char **out_skill_dir) {
    char *dirs[4];
    int dcount = 0;
    char *content = NULL;
    int d;

    dirs[dcount++] = xasprintf("%s/skills", cwd);
    if (agents_home && agents_home[0])
        dirs[dcount++] = xasprintf("%s/.agents/skills", agents_home);
    if (bag_home && bag_home[0])
        dirs[dcount++] = xasprintf("%s/skills", bag_home);

    for (d = 0; d < dcount && !content; d++) {
        char *skill_dir_path = xasprintf("%s/%s", dirs[d], skill_name);
        char *md_path = xasprintf("%s/SKILL.md", skill_dir_path);
        content = read_all(md_path);
        free(md_path);
        if (content) {
            /* replace ${BA_AGENT_SKILL_DIR} placeholders */
            const char *placeholder = strstr(content, "${BA_AGENT_SKILL_DIR}");
            if (placeholder) {
                StrBuf replaced;
                sb_init(&replaced);
                size_t prefix_len = placeholder - content;
                sb_appendn(&replaced, content, prefix_len);
                sb_append(&replaced, skill_dir_path);
                sb_append(&replaced, placeholder + strlen("${BA_AGENT_SKILL_DIR}"));
                free(content);
                content = replaced.data;
            }
            /* format: Base directory: <dir>\n\n<content> (bash-agent parity) */
            StrBuf full;
            sb_init(&full);
            sb_appendf(&full, "Base directory: %s\n\n%s", skill_dir_path, content);
            free(content);
            content = full.data;
            if (out_skill_dir) *out_skill_dir = skill_dir_path;
            else free(skill_dir_path);
        } else {
            free(skill_dir_path);
        }
    }
    for (d = 0; d < dcount; d++) free(dirs[d]);
    return content;
}

/* ============================================================
 * system prompt building - main function (block order follows bash-agent)
 * ============================================================ */

char *ba_build_prompt(const BaPromptCtx *ctx) {
    StrBuf buf;
    sb_init(&buf);

    const char *locale = detect_locale();
    int is_zh = (locale[0] == 'z' && locale[1] == 'h');

    /* 1. agent-identity */
    {
        const char *identity = "You are busyagent, a lightweight coding agent that runs as a busybox applet.";
        if (is_zh) identity = "你是 busyagent，一个以 busybox applet 形式运行的轻量级编码智能体。";
        prompt_append_section(&buf, "agent-identity", identity, NULL);
    }

    /* 2. environment */
    {
        struct utsname uts;
        StrBuf env;
        char *sh_env = getenv("SHELL");
        sb_init(&env);
        sb_appendf(&env, "lang: %s\n", detect_locale());
        sb_appendf(&env, "pwd: %s\n", ctx->cwd ? ctx->cwd : "?");
        sb_appendf(&env, "home: %s\n", ctx->home ? ctx->home : "?");
        if (uname(&uts) == 0)
            sb_appendf(&env, "platform: %s\n", uts.sysname);
        else
            sb_append(&env, "platform: unknown\n");
        sb_appendf(&env, "shell: %s", sh_env && sh_env[0] ? sh_env : "/bin/sh");
        prompt_append_section(&buf, "environment", env.data, NULL);
        sb_free(&env);
    }

    /* 3. rules */
    {
        const char *rules = "- Be concise and concrete. Lead with the answer. Use short sections or bullets when they improve readability. No pleasantries, no explanations unless asked. Raw results only.\n"
                            "- Prefer safe, exact edits.\n"
                            "- Report failures clearly.";
        prompt_append_section(&buf, "rules", rules, NULL);
    }

    /* 4. using-your-tools */
    {
        const char *tool_guidance =
            "- Use Read for a single file. If you need multiple files, call Read multiple times.\n"
            "- Read supports optional offset and limit parameters to read specific line ranges (saves tokens for large files). Output includes line numbers.\n"
            "- Use Glob and Grep for one pattern at a time.\n"
            "- Grep supports a context parameter to show surrounding lines — use it to get enough text for Edit directly from Grep output, avoiding a separate Read.\n"
            "- Use multiple tool calls in one response when they are independent.\n"
            "- Prefer dedicated tools over Bash when a dedicated tool fits the task.\n"
            "- For Edit: copy old_string exactly (including whitespace/indent/newlines). If you already know the location from prior context, use Read with offset/limit. If you need to locate the text first, use Grep with context — its output is often sufficient for Edit without an extra Read.\n"
            "- For skills, first check the skill-index section, then use Skill(name) for the matching skill.\n"
            "- Bash supports background=true for long-running commands. Returns task_id immediately; this build delivers the output by writing it to a temp file - read that file with Read to fetch results (bash-agent injects it via async events instead).";
        prompt_append_section(&buf, "using-your-tools", tool_guidance, NULL);
    }

    /* 5. sub-agent-guidance (sync wording adjusted, rest verbatim) */
    {
        const char *sag =
            "- **When to use**: delegating independent sub-tasks that do NOT need your current conversation context — e.g. investigating a separate file, running a focused search, testing a hypothesis in isolation.\n"
            "- **Recursion limit**: only the main agent may launch SubAgent. A child agent must not call SubAgent again; the runtime rejects nested launches.\n"
            "- **When NOT to use**: tasks that depend on your working context, conversation history, or intermediate state. The child agent starts with a blank slate.\n"
            "- **Prompt design**: write a complete, self-contained prompt. Include all file paths, function names, error messages, and constraints the child needs. Assume zero shared context.\n"
            "- **Result handling**: this build runs the sub-agent synchronously; its final answer is returned directly as this call's result. Interpret it in your next turn before acting.";
        prompt_append_section(&buf, "sub-agent-guidance", sag, NULL);
    }

    /* 6. todo-guidance (verbatim) */
    {
        const char *todo =
            "- Use TodoWrite proactively for complex multi-step implementation, debugging, refactoring, review, or multi-file tasks.\n"
            "- Do not use TodoWrite for trivial single-step, single-command, or purely informational requests.\n"
            "- After receiving a non-trivial task, create an initial checklist before or as you begin work.\n"
            "- When you use TodoWrite, write the full updated checklist for the current session, not a partial diff.\n"
            "- Keep the checklist short, concrete, and actionable.\n"
            "- Prefer exactly one in_progress item when work is actively underway.\n"
            "- Mark items completed immediately after finishing them, and remove stale items that no longer matter.";
        prompt_append_section(&buf, "todo-guidance", todo, NULL);
    }

    /* 7. plan-lifecycle-guidance */
    {
        StrBuf plg;
        sb_init(&plg);
        sb_append(&plg, "- **PLANNING WORKFLOW** — For complex multi-step tasks (3+ steps OR multi-file OR user requests planning)\n");
        sb_appendf(&plg, "- **Files**: PLAN_DRAFT_FILE: %s | PLAN_FILE: %s\n",
                   ctx->plan_draft ? ctx->plan_draft : "<not set>",
                   ctx->plan ? ctx->plan : "<not set>");
        sb_append(&plg, "- **Why draft first?** Writing to PLAN_FILE immediately invalidates the system prompt cache. Use PLAN_DRAFT_FILE for all drafting iterations to avoid this cost.\n");
        sb_append(&plg, "- **Drafting phase** (PLAN_DRAFT_FILE non-empty → you are drafting):\n"
                        "  Every user reply MUST be classified as exactly ONE of:\n"
                        "  ① REVISE (any feedback/question/change) → Write/Edit PLAN_DRAFT_FILE → ask confirmation → stay in drafting\n"
                        "  ② CONFIRM (explicit ok/go/confirmed) → call PlanConfirm IMMEDIATELY (before any other action) → TodoWrite checklist → execute\n"
                        "  ③ CANCEL (explicit cancel/forget it) → empty out PLAN_DRAFT_FILE → exit to idle\n"
                        "  ⚠ On CONFIRM you MUST call PlanConfirm first — no edits, no tool calls before it.\n");
        sb_append(&plg, "- **Execution phase**: after PlanConfirm → TodoWrite checklist → execute tasks → PlanClear when all done\n"
                        "- **Plan vs Todo**: PLAN_FILE=locked plan (only via PlanConfirm), PLAN_DRAFT_FILE=draft (edit freely), TodoWrite=progress tracker. Do NOT mix.");
        prompt_append_section(&buf, "plan-lifecycle-guidance", plg.data, NULL);
        sb_free(&plg);
    }

    /* 8. instruction-files */
    {
        StrBuf ifiles;
        char *gc = find_instruction_file(ctx->home);
        char *pc = ctx->cwd ? find_instruction_file(ctx->cwd) : NULL;

        sb_init(&ifiles);
        if (gc && gc[0]) {
            prompt_append_section(&ifiles, "instruction-file", gc, "global");
        }
        if (pc && pc[0]) {
            prompt_append_section(&ifiles, "instruction-file", pc, "project");
        }
        if (ifiles.len > 0 && ifiles.data[ifiles.len - 1] == '\n')
            sb_truncate(&ifiles, ifiles.len - 1);
        prompt_append_section(&buf, "instruction-files", ifiles.data, NULL);
        sb_free(&ifiles);
        free(gc);
        free(pc);
    }

    /* 9. skill-index */
    {
        StrBuf si;
        sb_init(&si);
        build_skill_index(&si, ctx->cwd, getenv("HOME"), ctx->home);
        if (si.len > 0 && si.data[si.len - 1] == '\n')
            sb_truncate(&si, si.len - 1);
        prompt_append_section(&buf, "skill-index", si.data, NULL);
        sb_free(&si);
    }

    /* 11. current-plan */
    {
        char *plan = ctx->plan ? read_all(ctx->plan) : NULL;
        if (plan && plan[0]) {
            /* bash version: name attribute = plan file path */
            prompt_append_section(&buf, "current-plan", plan, ctx->plan);
        }
        free(plan);
    }

    /* 13. output-language */
    {
        StrBuf ol;
        sb_init(&ol);
        if (is_zh) {
            sb_append(&ol, "再次强调：必须使用中文进行所有输出，包括你的思考过程（Chain of Thought/推理/thinking）！严禁在思考或回答中出现任何英文内容！");
        } else {
            sb_appendf(&ol, "MUST use \"%s\" for all output, including your Chain of Thought/reasoning/thinking! Never mix languages! Code, commands, and file content remain as-is.", locale);
        }
        prompt_append_section(&buf, "output-language", ol.data, NULL);
        sb_free(&ol);
    }

    /* drop the trailing \n (bash printf '%s' semantics) */
    if (buf.len > 0 && buf.data[buf.len - 1] == '\n') {
        buf.data[buf.len - 1] = '\0';
        buf.len--;
    }

    return buf.data;
}

/* ==== ba_tools.c ==== */
/*
 * ba_tools - table-driven tool execution for busyagent
 *
 * Tool definitions live in $BA_HOME/tools.json — the only source,
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
#include "busybox.h"   /* for APPLET_IS_NOFORK */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
static char *g_tools_json_text;   /* raw text of the active tools array */
static const SessionPaths *g_paths;   /* session sink for builtin state tools */

static char *read_file_all(const char *path)
{
	int fd = open(path, O_RDONLY);
	char *buf;
	if (fd < 0)
		return NULL;
	buf = xmalloc_read(fd, NULL);   /* libbb: allocates and NUL-terminates */
	close(fd);
	return buf;
}

/* Parse one tools JSON text into the global table. Returns 0 on success.
 * Every entry must carry a non-empty name and a well-formed exec mapping
 * (exec.applet: non-empty string, exec.argv: array of strings) - malformed
 * entries are rejected here instead of crashing later at execution time
 * (a non-string argv template would dereference NULL in expand_token). */
static int ba_tools_parse(const char *json)
{
	JsonParse jp = json_parse_root(json);
	JsonVal arr;
	int n, i;

	if (jp.error)
		return -1;
	arr = jp.val;
	n = json_array_len(arr);
	if (n <= 0)
		return -1;

	g_tools = xzalloc(n * sizeof(BaTool));
	g_tool_count = 0;

	for (i = 0; i < n; i++) {
		JsonVal item, exec, argv_val, fnv;
		BaTool *t = &g_tools[g_tool_count];
		int an, j, bad = 0;

		item = json_array_get(arr, i);
		fnv = json_get(item, "function");
		t->name = (fnv.type != JSON_NULL)
			? json_get_string(fnv, "name")
			: json_get_string(item, "name");
		exec = json_get(item, "exec");
		argv_val = json_get(exec, "argv");
		if (exec.type == JSON_OBJECT)
			t->applet = json_get_string(exec, "applet");
		an = (argv_val.type == JSON_ARRAY) ? json_array_len(argv_val) : 0;

		if (!t->name || !t->name[0]) {
			bb_error_msg("tools.json: entry %d: missing tool name, skipped", i);
			bad = 1;
		} else if (exec.type != JSON_OBJECT
		 || !t->applet || !t->applet[0]) {
			bb_error_msg("tools.json: entry '%s': exec.applet missing, skipped", t->name);
			bad = 1;
		}
		if (!bad && an > 0) {
			t->argv_tpl = xzalloc(an * sizeof(char *));
			t->argv_count = an;
			for (j = 0; j < an; j++) {
				JsonVal av = json_array_get(argv_val, j);
				if (av.type != JSON_STRING) {
					bb_error_msg("tools.json: entry '%s': exec.argv[%d]"
						     " is not a string, skipped", t->name, j);
					bad = 1;
					break;
				}
				t->argv_tpl[j] = json_string_val(av);
			}
		}
		if (bad) {
			free(t->name);
			free(t->applet);
			if (t->argv_tpl) {
				for (j = 0; j < t->argv_count; j++)
					free(t->argv_tpl[j]);
				free(t->argv_tpl);
			}
			memset(t, 0, sizeof(*t));
			continue;
		}
		g_tool_count++;
	}
	if (g_tool_count == 0) {
		free(g_tools);
		g_tools = NULL;
		return -1;
	}
	return 0;
}

/* Load tools from $BA_HOME/tools.json. The file is the only
 * source: nothing is embedded in the binary. Missing or broken file
 * means "no tools" — plain Q&A still works, requests just omit tools. */
void ba_tools_set_paths(const SessionPaths *paths)
{
	g_paths = paths;
}

int ba_tools_init(const char *home, const SessionPaths *paths)
{
	char *path = NULL;
	char *data = NULL;

	/* even on the cached fast path the session sink must point at the
	 * caller's (possibly new) stack frame - a stale pointer here is a
	 * use-after-free once that frame returns */
	g_paths = paths;
	if (g_tools)
		return g_tool_count;

	if (home && home[0])
		path = xasprintf("%s/tools.json", home);
	if (path)
		data = read_file_all(path);
	if (data && ba_tools_parse(data) == 0) {
		g_tools_json_text = data;
	} else {
		if (data) {
			bb_error_msg("tools.json parse failed, dynamic zone ignored (builtins remain)");
			free(data);
		}
		/* missing file is the normal path: builtins only, stay quiet */
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

/* span copy: JsonVal is a (src,start,end) view */
static char *val_span_dup(const char *src, JsonVal v)
{
    size_t n = v.end - v.start;
    char *s = xmalloc(n + 1);
    memcpy(s, src + v.start, n);
    s[n] = '\0';
    return s;
}

/* Append one dynamic tool entry to the LLM-visible tools array.
 * The internal "exec" mapping (applet + argv template) is host-side only:
 * the request is rebuilt from the public fields so the model can neither
 * see nor influence how the tool dispatches. */
static void tools_json_append_entry(StrBuf *sb, const char *src, JsonVal item)
{
    JsonVal fnv = json_get(item, "function");
    int is_fn = (fnv.type != JSON_NULL);
    JsonVal body = is_fn ? fnv : item;
    JsonVal namev = json_get(body, "name");
    JsonVal descv = json_get(body, "description");
    JsonVal schemav = json_get(body, is_fn ? "parameters" : "input_schema");
    char *span;

    if (namev.type != JSON_STRING)
        return;   /* caller already reported unnamed entries */

    sb_append(sb, ",\n  ");
    if (is_fn)
        sb_append(sb, "{\"type\":\"function\",\"function\":{\"name\":");
    else
        sb_append(sb, "{\"name\":");
    span = val_span_dup(src, namev);   /* includes the quotes */
    sb_append(sb, span);
    free(span);
    if (descv.type == JSON_STRING) {
        sb_append(sb, ",\"description\":");
        span = val_span_dup(src, descv);
        sb_append(sb, span);
        free(span);
    }
    if (schemav.type == JSON_OBJECT) {
        sb_append(sb, is_fn ? ",\"parameters\":" : ",\"input_schema\":");
        span = val_span_dup(src, schemav);
        sb_append(sb, span);
        free(span);
    }
    sb_append(sb, is_fn ? "}}" : "}");
}

/* The tools array as the LLM sees it (exec stripped).
 * NULL when no table: the caller omits tools from the request. */
/* LLM-visible tools = 11 builtins + dynamic zone (exec stripped).
 * Without a dynamic zone only the builtins ship (sh anchor). */
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
        sb_truncate(&sb, sb.len - 2);   /* drop trailing "]\n", append dynamic zone */
        for (i = 0; i < n; i++) {
            JsonVal item = json_array_get(jp.val, i);
            JsonVal fnv = json_get(item, "function");
            char *nm = (fnv.type != JSON_NULL)
                     ? json_get_string(fnv, "name")
                     : json_get_string(item, "name");
            /* builtin names may not be shadowed */
            if (nm && (!strcmp(nm, "Read") || !strcmp(nm, "Write") || !strcmp(nm, "Edit")
                    || !strcmp(nm, "Bash") || !strcmp(nm, "Glob") || !strcmp(nm, "Grep")
                    || !strcmp(nm, "TodoWrite") || !strcmp(nm, "PlanConfirm")
                    || !strcmp(nm, "PlanClear") || !strcmp(nm, "Skill")
                    || !strcmp(nm, "SubAgent"))) {
                bb_error_msg("tools.json: '%s' shadows a builtin, skipped", nm);
                free(nm);
                continue;
            }
            tools_json_append_entry(&sb, src, item);
            free(nm);
        }
        sb_append(&sb, "]\n");
    }
    return sb.data;
}

/* dynamic-zone starter template (-i export) */
/* template is derived from ba_builtin_schemas at write time */

/* Export the starter tools table to path. Entry names must not overlap
 * the builtins (enforced at runtime too). 0 ok; -1 exists; -2 write error. */
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
	{
		const char *b = ba_builtin_schemas;
		size_t bl = strlen(b);
		/* strip the trailing "]" of the builtin array, then close with a
		 * worked custom entry so the file is one valid JSON array. The
		 * entry MUST carry an exec mapping (applet + argv template) -
		 * without it the tool would be offered to the model but fail
		 * at execution time. $name placeholders expand from the model's
		 * input JSON at call time. */
		static const char tail[] =
			",\n"
			"  {\n"
			"    \"name\": \"MyApplet\",\n"
			"    \"description\": \"Example custom entry - edit or remove.\",\n"
			"    \"input_schema\": {\n"
			"      \"type\": \"object\",\n"
			"      \"properties\": { \"pattern\": { \"type\": \"string\" } },\n"
			"      \"required\": [\"pattern\"]\n"
			"    },\n"
			"    \"exec\": {\n"
			"      \"applet\": \"grep\",\n"
			"      \"argv\": [\"-rl\", \"-e\", \"$pattern\", \".\"]\n"
			"    }\n"
			"  }]\n";
		if (bl < 2 || full_write(fd, b, bl - 2) < 0
		 || full_write(fd, tail, sizeof(tail) - 1) < 0) {
			close(fd);
			return -2;
		}
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

	/* ---- builtin reserved names (L2/L3): loop-level semantics ---- */

	if (strcmp(name, "Bash") == 0) {
		char *cmd = json_get_string(jp.val, "command");
		int background = json_get_bool(jp.val, "background", false);
		int tmo_sec = json_get_int(jp.val, "timeout");
		int tmo_ms = json_get_int(jp.val, "timeout_ms");

		if (!cmd || !cmd[0]) {
			sb_append(&sb, "Error: Bash requires 'command'");
			free(cmd);
			return sb.data;
		}

		if (background) {
			/* One registered implementation only (ba_background_spawn in
			 * busyagent.c): task gets a task_id + hard deadline, its log
			 * is RLIMIT_FSIZE-capped, and ba_drain_background reaps it
			 * and injects the result. The previous second, unregistered
			 * double-fork variant here leaked processes, temp files and
			 * could never report completion. */
			if (!cmd[0]) {
				sb_append(&sb, "Error: no command provided");
				free(cmd);
				return sb.data;
			}
			{
				char *resp = ba_background_spawn(cmd);
				sb_append(&sb, resp);
				free(resp);
			}
			free(cmd);
			return sb.data;
		}

		{
			char *sh_argv[4];
			int tmo = tmo_ms;
			if (!tmo && tmo_sec > 0)
				tmo = tmo_sec * 1000;   /* bash-agent takes seconds */
			if (!tmo)
				tmo = timeout_ms;

			sh_argv[0] = (char *)"sh";
			sh_argv[1] = (char *)"-c";
			sh_argv[2] = cmd;
			sh_argv[3] = NULL;
			status = run_captured("sh", sh_argv, &out, &err, tmo);
			result_wrap(&sb, status, &out, &err);
		}
		free(cmd);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "Read") == 0) {
		/* cat -n semantics: whole file or offset/limit paging (no fork) */
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
				sb_truncate(&nb, hit - data);   /* head */
				sb_append(&nb, new_s);          /* replacement */
				sb_append(&nb, hit + strlen(old_s)); /* tail */
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
		/* path becomes find(1)'s positional root argument: a leading '-'
		 * would be parsed as an option/action (e.g. "-delete", "-exec") */
		if (gpath && gpath[0] == '-') {
			sb_append(&sb, "Error: Glob 'path' must not start with '-'");
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
		char *glob_f = json_get_string(jp.val, "glob");
		int ctx = json_get_int(jp.val, "context");
		char ctxs[16];
		char *ctx_dup = NULL, *inc_dup = NULL;
		char *gv[12];
		int gi = 0;

		if (!pattern || !pattern[0]) {
			sb_append(&sb, "Error: Grep requires 'pattern'");
			free(pattern); free(gpath); free(glob_f);
			return sb.data;
		}
		/* same option-injection guard as Glob: path is grep's positional
		 * search root and would otherwise be read as an option */
		if (gpath && gpath[0] == '-') {
			sb_append(&sb, "Error: Grep 'path' must not start with '-'");
			free(pattern); free(gpath); free(glob_f);
			return sb.data;
		}
		gv[gi++] = (char *)"grep";
		if (glob_f && glob_f[0]) {
			inc_dup = xasprintf("--include=%s", glob_f);
			gv[gi++] = inc_dup;
		}
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
		/* todos array -> markdown checklist, fed back as the tool result.
		 * Persistence lives in the conversation history (the full call record
		 * IS the state); nothing hits disk or the system prompt. */
		JsonVal todos = json_get(jp.val, "todos");
		int total, t;
		if (todos.type != JSON_ARRAY) {
			sb_append(&sb, "OK");   /* bash-agent parity: no error on bad shapes */
			return sb.data;
		}
		total = json_array_len(todos);
		for (t = 0; t < total; t++) {
			JsonVal item = json_array_get(todos, t);
			char *content = json_get_string(item, "content");
			char *st = json_get_string(item, "status");
			sb_append(&sb, "- [");
			sb_append(&sb, (st && strcmp(st, "completed") == 0) ? "x" : " ");
			sb_append(&sb, "] ");
			sb_append(&sb, content ? content : "");
			sb_append(&sb, "\n");
			free(content); free(st);
		}
		/* drop the trailing newline (bash-agent parity) */
		if (sb.len > 0 && sb.data[sb.len - 1] == '\n') {
			sb.data[sb.len - 1] = '\0';
			sb.len--;
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
		/* note: bash-agent moves the file after compaction; this port
 * is synchronous without compaction, so write it here */
		sb_append(&sb, "Plan confirmed and locked in.");
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
		/* L2 knowledge level: unified search via ba_load_skill
		 * cwd/skills > ~/.agents/skills > $BA_HOME/skills，
		 * supports ${BA_AGENT_SKILL_DIR} substitution */
		char *skill = json_get_string(jp.val, "name");
		const char *agents_home = getenv("HOME");
		const char *bag_home = getenv("BA_HOME");
		char *cwd = xrealloc_getcwd_or_warn(NULL);
		char *content;

		if (!skill || !skill[0]) {
			sb_append(&sb, "Error: Skill requires 'name'");
			free(skill); free(cwd);
			return sb.data;
		}
		content = ba_load_skill(skill, cwd,
					(agents_home && agents_home[0]) ? agents_home : NULL,
					bag_home ? bag_home : "/tmp/busyagent",
					NULL);
		free(cwd);
		if (!content) {
			sb_appendf(&sb, "Error: skill '%s' not found "
				     "(searched $CWD/skills, ~/.agents/skills, $BA_HOME/skills)", skill);
			free(skill);
			return sb.data;
		}
		sb_append(&sb, content);
		free(content);
		free(skill);
		return sb.data;
	}

	if (strcmp(name, "SubAgent") == 0) {
		/* placeholder: intercepted by the turn loop (tools.c parity) */
		sb_append(&sb, "SubAgent handled by agent layer");
		return sb.data;
	}

	/* ---- L0/L1: exec mapping ---- */

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
