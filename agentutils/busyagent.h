#ifndef BUSYAGENT_H
#define BUSYAGENT_H

/* ==== ba_util.h ==== */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/*
 * utility functions - strings, paths, timestamps
 */
/* StrBuf, utils and the JSON parser/serializer moved to
 * agent_common.h */
#include "agent_common.h"

#endif /* UTIL_H */


/* ==== ba_store.h ==== */
#ifndef STORE_H
#define STORE_H


/*
 * session store - session management, conversation I/O, stats updates
 *
 * layout (same as bash/go/rust):
 *   ~/.bash-agent/projects/<project-key>/<session-id>/
 *     conversation.jsonl  - dialogue history
 *     events.jsonl        - event log
 *     stats.json          - statistics
 *     summary.md          - context summary
 *     plan.md             - confirmed plan
 *     plan.draft          - plan draft
 */

/* session path set */
typedef struct {
    char *base_dir;         /* ~/.bash-agent/projects/<key>/ */
    char *session_dir;      /* base_dir/<session-id>/ */
    char *conversation;     /* session_dir/conversation.jsonl */
    char *events;           /* session_dir/events.jsonl */
    char *stats;            /* session_dir/stats.json */
    char *summary;          /* session_dir/summary.md */
    char *plan;             /* session_dir/plan.md */
    char *plan_draft;       /* session_dir/plan.draft */
} SessionPaths;

void store_session_paths_free(SessionPaths *p);

/* deep copy: every path string duplicated (the source can then be freed) */
SessionPaths store_session_paths_dup(const SessionPaths *p);

/* build the path set from home, cwd, session_id */
SessionPaths store_session_paths_for(const char *home, const char *cwd, const char *session_id);

/* ensure the session directory and files exist */
int store_session_init(const SessionPaths *p, int is_new);

/* create a new session for a child agent */
int store_session_init_sub(const SessionPaths *parent_paths, const SessionPaths *sub_paths, int fork);

/* copy conversation/summary/plan into a new session (store_session_fork parity) */
int store_session_fork(const SessionPaths *parent, const SessionPaths *child);

const char *store_session_image_dir(const SessionPaths *paths);

/* project key from cwd (slashes to -) */
char *store_session_project_key(const char *cwd);

/* new session id (YYYYMMDD-HHMMSS-XXXX) */
char *session_new_id(void);

/* find the most recent session (for --continue) */
char *store_session_resolve_continue(const char *home, const char *cwd);

/* list sessions; formatted lines into out, returns count */
int store_session_list_rows(const char *home, const char *cwd, StrBuf *out);

/* ============================================================
 * conversation.jsonl operations
 * ============================================================ */

/* append a user message */
int store_conv_add_user(const char *path, const char *content);

/* append an assistant message (thinking, text, tool_calls) */
int store_conv_add_assistant(const char *path, const char *thinking, const char *text,
                       /* tool_calls: array of {id, name, input_json} */
                       int tool_count, const char **tool_ids,
                       const char **tool_names, const char **tool_inputs);

/* append a tool_result message */
int store_conv_add_tool_results(const char *path, int count, const char **tool_use_ids,
                          const char **contents);

/* read all lines (array of malloc'd strings) */
int store_conv_line_count(const char *path, char ***out, int *out_count);

/* keep only the last keep_lines lines */
int store_conv_trim_tail(const char *path, int keep_lines);

/* count user-input messages */
int store_conv_user_turn_count(const char *path);

/* total bytes */
long store_conv_total_bytes(const char *path);

/* ============================================================
 * stats.json operations
 * ============================================================ */

/* read stats (malloc'd JSON string) */
char *store_stats_read(const char *path);

/* update stats (read -> callback -> write back) */
typedef void (*stats_update_fn)(void *ctx, JsonVal stats);
int store_stats_update(const char *path, stats_update_fn fn, void *ctx);

/* common updates */
void store_stats_add_int(JsonVal obj, const char *key, int delta);
void store_stats_set_int(JsonVal obj, const char *key, int value);

/* simple file-level helpers: integer fields in the stats file */
int store_stats_get_file_int(const char *path, const char *key);
void store_stats_set_int_file(const char *path, const char *key, int value);
int store_stats_get_int_file(const char *path, const char *key);

void store_event_set_stream_json(int enabled);
int store_event_stream_json_enabled(void);

/* read an integer from stats */
int store_stats_get_int(JsonVal obj, const char *key);

/* ============================================================
 * events.jsonl operations
 * ============================================================ */

/* append an event (JSON string) */
int store_event_append(const SessionPaths *p, const char *json_str);

/* read all event lines */
int store_event_lines(const SessionPaths *p, char ***out, int *out_count);

/* ============================================================
 * summary / plan file operations
 * ============================================================ */

char *store_summary_get(const SessionPaths *p);
int store_summary_set(const SessionPaths *p, const char *content);

char *store_plan_draft_read(const SessionPaths *p);
int store_plan_draft_set(const SessionPaths *p, const char *content);
int store_plan_draft_clear(const SessionPaths *p);
int store_plan_set(const SessionPaths *p, const char *content);
int store_plan_clear(const SessionPaths *p);

#endif /* STORE_H */

/* ==== ba_display.h ==== */
/*
 * ba_display.h - synchronous port of bash-agent's display layer.
 *
 * Same message types, same human rendering (ANSI colors, cat -n style
 * truncation rules) and same stream-json event shapes as display.c's
 * render_message()/display_msg_to_event(); the pthread/queue machinery is
 * replaced by direct calls (single-turn model has no display thread).
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#ifndef BA_DISPLAY_H
#define BA_DISPLAY_H

#include <stdio.h>

typedef enum {
    BA_DM_TEXT,
    BA_DM_THINKING,
    BA_DM_TOOL_CALL,
    BA_DM_TOOL_RESULT,
    BA_DM_USAGE,
    BA_DM_STOP,
    BA_DM_ERROR,
    BA_DM_SUB_AGENT_START,
    BA_DM_SUB_AGENT_RESULT,
    BA_DM_ASYNC_TASK_RESULT,
    BA_DM_CONTEXT_UPDATE,
} BaDisplayType;

typedef enum {
    BA_FMT_HUMAN,
    BA_FMT_STREAM_JSON,
    BA_FMT_NONE,     /* SubAgent child turn: events.jsonl only */
} BaDisplayFormat;

/* the bash-agent DisplayMessage field subset */
typedef struct {
    int type;
    const char *content;    /* TEXT/THINKING/STOP(reason)/ERROR/TOOL_RESULT content/summary */
    const char *tool_name;
    const char *tool_id;
    const char *tool_input; /* already JSON text */
    const char *session_id; /* SubAgent/bg task id */
    int in_tokens, out_tokens, cache_read_tokens, cache_creation_tokens;
    int tool_exit_code;
} BaDisplayMsg;

typedef struct {
    char last_char[8];
    int prev_was_thinking;
    BaDisplayFormat format;
    FILE *out;          /* stream-json sink (stdout) */
} BaDisplay;

void ba_disp_init(BaDisplay *d, BaDisplayFormat fmt);

/* One message: stream-json emits an event line; human renders with
 * colors and truncation. Returns the events.jsonl text (caller frees). */
char *ba_display_push(BaDisplay *d, const BaDisplayMsg *m);

/* tool-call summary (agent_tool_display_summary parity) */
char *ba_tool_call_summary(const char *name, const char *input_json);

#endif /* BA_DISPLAY_H */

/* ==== ba_transport.h ==== */
#ifndef TRANSPORT_H
#define TRANSPORT_H


/*
 * transport - SSE stream parsing and request building (no libcurl)
 *
 * HTTP I/O uses libbb primitives; this is protocol only.
 */

/* SSE event callback */
typedef enum {
    SSE_TEXT,               /* text delta */
    SSE_THINKING,           /* thinking delta */
    SSE_TOOL_CALL_START,    /* tool call started (id + name known) */
    SSE_TOOL_INPUT_DELTA,       /* tool-call input_json delta */
    SSE_TOOL_CALL,          /* tool call complete (whole) */
    SSE_USAGE,              /* token usage */
    SSE_STOP,               /* stop */
    SSE_ERROR,              /* error */
    SSE_RETRY,              /* retry (reset the accumulator) */
} SseEventType;

typedef struct {
    SseEventType type;
    char *content;           /* TEXT/THINKING/STOP/ERROR: text */
    const char *tool_id;     /* TOOL_CALL: call id */
    const char *tool_name;   /* TOOL_CALL: tool name */
    const char *tool_input;  /* TOOL_CALL: arguments JSON */
    int in_tokens;           /* USAGE: input tokens */
    int out_tokens;          /* USAGE: output tokens */
    int cache_read_tokens;   /* USAGE: cache-read tokens */
    int cache_creation_tokens; /* USAGE: cache-creation tokens */
} SseEvent;

typedef void (*sse_callback_fn)(void *ctx, const SseEvent *evt);

/* streaming POST; SSE events via the callback */
int http_post_sse(const char *url, const char **headers, int header_count,
                  const char *body, size_t body_len,
                  const char *provider,
                  sse_callback_fn callback, void *ctx,
                  volatile int *cancelled);

/* streaming callback context (line split + provider dispatch) */
typedef struct {
    int index;
    char *id;
    char *name;
    StrBuf arguments;
} OpenAIToolAccum;

typedef struct {
    sse_callback_fn callback;
    void *ctx;
    StrBuf line_buf;        /* accumulating SSE line */
    char *event;            /* Responses SSE event name */
    char *provider;         /* "claude", "openai" or "responses" */
    volatile int *cancelled;
    OpenAIToolAccum *openai_tools;
    int openai_tool_count;
    int openai_tool_cap;
    int responses_saw_text;
    int responses_terminal;
    int responses_input_tokens;
    int responses_output_tokens;
    int responses_cache_read_tokens;
    char **responses_item_ids;
    int *responses_item_indexes;
    int responses_item_count;
    int responses_item_cap;
} StreamCtx;

/* transport-agnostic SSE pump interface */
void sse_stream_init(StreamCtx *sctx, const char *provider,
                     sse_callback_fn callback, void *ctx,
                     volatile int *cancelled);
void sse_stream_free(StreamCtx *sctx);
/* stream tail: leftover JSON + responses termination check */
void sse_stream_finish(StreamCtx *sctx, const char *provider,
                       sse_callback_fn callback, void *ctx);
/* feed one decoded body chunk; 0 means cancelled */
int sse_stream_feed(StreamCtx *sctx, const char *ptr, size_t len);

/* parse an SSE event line (from "data: ..." lines) */
int sse_parse_event(const char *provider, const char *data, size_t data_len,
                    sse_callback_fn callback, void *ctx);

/* ============================================================
 * SSE accumulator - collects streamed events for the turn loop
 * ============================================================ */

/* accumulated tool call */
typedef struct {
    char *id;
    char *name;
    StrBuf input_json;     /* accumulated input_json_delta */
} ToolCallAccum;

typedef struct {
    /* accumulated text */
    StrBuf text;
    StrBuf thinking;

    /* accumulated tool-call list */
    ToolCallAccum *tools;
    int tool_count;
    int tool_cap;

    /* current content_block index (-1 = none) */
    int current_block_index;
    char *current_block_type;  /* "text", "thinking", "tool_use" */
    char *current_tool_id;     /* tool id at content_block_start */
    char *current_tool_name;   /* tool name at content_block_start */

    /* statistics */
    int in_tokens;
    int out_tokens;
    int cache_read_tokens;
    int cache_creation_tokens;

    /* stop reason */
    char *stop_reason;

    /* error */
    char *error;

    /* status flags */
    int stopped;            /* stop event received */
} SseAccumulator;

/* init/free the accumulator */
void sse_accum_init(SseAccumulator *acc);
void sse_accum_free(SseAccumulator *acc);

/* SSE callback - accumulate events into SseAccumulator */
void sse_accum_callback(void *ctx, const SseEvent *evt);

/* build the request body */
char *build_claude_request(const char *model, const char *system_prompt,
                           const char *tools_json,
                           char **conv_lines, int conv_line_count,
                           int max_tokens, const char *thinking, const char *effort);

/* convert a Claude request body to OpenAI format */
char *convert_to_openai(const char *claude_body);

/* convert a Claude request body to Responses API format */
char *convert_to_responses(const char *claude_body);

#endif /* TRANSPORT_H */

/* ==== ba_prompt.h ==== */
/*
 * ba_prompt.h - system prompt construction, ported from bash-agent's
 * agent_build_prompt suite (verbatim structure and section text; only the
 * agent identity string and skill search dirs were adapted for busyagent).
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#ifndef BA_PROMPT_H
#define BA_PROMPT_H


/* prompt build context (the Agent field subset) */
typedef struct {
    const char *cwd;        /* working directory */
    const char *home;       /* $BA_HOME root */
    const char *plan;       /* PLAN_FILE path */
    const char *plan_draft; /* PLAN_DRAFT_FILE path */
} BaPromptCtx;

/* build the full system prompt; caller frees */
char *ba_build_prompt(const BaPromptCtx *ctx);

/* load a skill's content; malloc'd string or NULL.
 * Search order matches the system prompt skill-index:
 *   $CWD/skills > ~/.agents/skills > $BA_HOME/skills
 * agents_home is $HOME (may be NULL); bag_home is $BA_HOME.
 * out_skill_dir receives the hit directory (optional). */
char *ba_load_skill(const char *skill_name, const char *cwd,
                    const char *agents_home, const char *bag_home,
                    char **out_skill_dir);

#endif /* BA_PROMPT_H */

/* ==== ba_tools.h ==== */
/*
 * ba_tools.h - tool table & execution for busyagent
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#ifndef BA_TOOLS_H
#define BA_TOOLS_H

#include <stddef.h>

/* Initialize the tool system:
 *   - the 11 builtin tools are always present (compiled in);
 *   - $BA_HOME/tools.json (dynamic zone) is loaded on top; a
 *     missing/broken file only loses the dynamic sugar, never the
 *     builtins. Dynamic names may not shadow builtins.
 * paths is used by state tools (Plan*) for session placement. */
int ba_tools_init(const char *home, const SessionPaths *paths);

void ba_tools_free(void);

/* Re-point the built-in state tools (Plan*) at another session. */
void ba_tools_set_paths(const SessionPaths *paths);

/* Background Bash: register the task (hard deadline + RLIMIT_FSIZE log cap)
 * and return a malloc'd acknowledgement/error message for the model. */
char *ba_background_spawn(const char *cmd);

/* Kill every live background task and unlink its log file. Call once on
 * process exit so no processes or temp files are left behind. */
void ba_background_cleanup_all(void);

/* Tools array sent to the LLM: builtin schemas + dynamic zone with
 * "exec" mappings stripped (they are internal). Caller frees. */
char *ba_tools_json(void);

/* Execute one tool call by name. Returns malloc'd output text, never
 * NULL. Routing order: builtin reserved names -> dynamic exec mapping. */
char *ba_tool_execute(const char *name, const char *input_json, int timeout_ms);

/* Export a starter tools.json template to path (busyagent -i).
 * Creates parent dirs; refuses to overwrite. 0 ok / -1 exists / -2 fail. */
int ba_tools_write_template(const char *path);

#endif /* BA_TOOLS_H */

#endif /* BUSYAGENT_H */
