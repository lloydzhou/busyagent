#ifndef BUSYAGENT_H
#define BUSYAGENT_H

/* ==== ba_util.h ==== */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/*
 * 工具函数 — 字符串处理、路径拼接、时间戳等
 */

/* 动态字符串缓冲区 */
typedef struct {
    char *data;
    size_t len;      /* 当前字符串长度（不含 '\0'） */
    size_t cap;      /* 缓冲区容量 */
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_ensure(StrBuf *sb, size_t extra);
void sb_append(StrBuf *sb, const char *s);
void sb_appendn(StrBuf *sb, const char *s, size_t n);
void sb_appendf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void sb_append_char(StrBuf *sb, char c);
/* 截断到指定长度 */
void sb_truncate(StrBuf *sb, size_t len);

/* JSON 字符串转义：将 src 转义后追加到 sb */
void sb_append_json_string(StrBuf *sb, const char *src);
/* shell 参数转义：将 src 作为单个 shell 参数安全追加到 sb */
void sb_append_shell_arg(StrBuf *sb, const char *src);

/* 生成 session id: YYYYMMDD-HHMMSS-XXXX */
char *util_new_session_id(void);

/* 路径拼接：a/b （处理尾部/前导斜杠） */
char *util_path_join(const char *a, const char *b);

/* 确保目录存在（递归创建，类似 mkdir -p） */
int util_mkdirs(const char *path, int mode);

/* 获取家目录路径 */
const char *util_home_dir(void);

/* 复制字符串（strdup 的安全版本，NULL 安全） */
char *util_strdup(const char *s);

/* 安全的 free + 置 NULL */
#define FREE_PTR(p) do { free(p); (p) = NULL; } while(0)

/* 从环境变量读取，带默认值 */
const char *util_env(const char *name, const char *defval);

/* 获取当前时间戳字符串 (ISO 8601) */
char *util_timestamp_now(void);

/* 解析带 k/m/g 后缀的数字（对齐 bash 版 util_parse_size） */
long util_parse_size(const char *s);

/* 获取当前 epoch 秒 */
long util_epoch_seconds(void);

/* 计算 UTF-8 字符数（近似 token 计数用） */
int util_utf8_char_count(const char *s);

/* 返回不超过 max_bytes 的安全截断位置（不切断 UTF-8 多字节字符） */
size_t util_utf8_truncate_len(const char *s, size_t max_bytes);

/* 原地截断到 max_total 字节以内（UTF-8 安全），超长时尾部追加 "..." */
void util_truncate_str(char *s, size_t max_total);

/* 原地截断到 max_chars 个 UTF-8 字符以内，超长时尾部追加 "..." */
void util_truncate_chars(char *s, int max_chars);

/* UTF-8 sanitize：将非法字节替换为 \ufffd 字面文本，返回新 malloc'd 字符串 */
char *util_sanitize_utf8(const char *src);

/* trim 尾部空白 */
char *util_rtrim(char *s);

/* 读取整个文件到字符串 */
char *util_read_file(const char *path);

/* 写入整个文件 */
int util_write_file(const char *path, const char *content);

#endif /* UTIL_H */

/* ==== ba_json.h ==== */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdbool.h>

/*
 * 轻量 JSON 解析器 — 仅支持解析 Claude/OpenAI API 响应和 conversation.jsonl
 *
 * 设计：
 *   - 单遍扫描，零拷贝（值指向原始 JSON 字符串内部）
 *   - 提供 json_get_string 等提取函数，返回需要 free 的副本
 *   - 支持 JSON 对象、数组、字符串、数字、布尔、null
 *   - 不支持 JSON 写入（用 StrBuf 手动拼接即可）
 */

/* JSON 值类型 */
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

/* JSON 值 — 指向原始 JSON 文本内部的视图 */
typedef struct {
    JsonType type;
    const char *src;        /* 指向原始 JSON 字符串 */
    size_t start;           /* 值在 src 中的起始位置 */
    size_t end;             /* 值在 src 中的结束位置（不含） */
    /* 对于 STRING：src+start..src+end 是原始值（含引号） */
    /* 对于 OBJECT/ARRAY：src+start..src+end 是整个结构 */
} JsonVal;

/* 解析结果 */
typedef struct {
    JsonVal val;            /* 解析出的值 */
    const char *error;      /* 错误信息，NULL 表示成功 */
} JsonParse;

/* ============================================================
 * 解析函数
 * ============================================================ */

/* 解析 JSON 值，从 src+pos 开始，返回解析结果和更新后的 pos */
JsonParse json_parse(const char *src, size_t *pos);

/* 解析完整的 JSON 字符串（从根开始） */
JsonParse json_parse_root(const char *src);

/* ============================================================
 * 查询函数 — 从 OBJECT 中提取字段
 * ============================================================ */

/* 获取 object 中 key 对应的值，如果不存在返回 type=JSON_NULL */
JsonVal json_get(JsonVal obj, const char *key);

/* 获取字符串值（返回需要 free 的副本，找不到返回 NULL） */
char *json_get_string(JsonVal obj, const char *key);

/* 获取整数值 */
int json_get_int(JsonVal obj, const char *key);

/* 获取 long long 整数值（用于大 token 计数） */
long long json_get_ll(JsonVal obj, const char *key);

/* 获取浮点数值 */
double json_get_double(JsonVal obj, const char *key);

/* 获取布尔值（找不到返回 def） */
bool json_get_bool(JsonVal obj, const char *key, bool def);

/* ============================================================
 * 数组操作
 * ============================================================ */

/* 获取数组长度 */
int json_array_len(JsonVal arr);

/* 获取数组第 i 个元素（越界返回 JSON_NULL） */
JsonVal json_array_get(JsonVal arr, int index);

/* ============================================================
 * 值提取
 * ============================================================ */

/* 从 JSON_STRING 值中提取解码后的字符串（返回需 free 的副本） */
char *json_string_val(JsonVal v);

/* 从 JSON_NUMBER 值中提取 double */
double json_number_val(JsonVal v);

/* 从 JSON_BOOL 值中提取 bool */
bool json_bool_val(JsonVal v);

/* 通用提取：如果 v 是字符串则解码返回，否则返回 NULL */
char *json_as_string(JsonVal v);

/* ============================================================
 * 遍历 OBJECT 的键值对
 * ============================================================ */

typedef struct {
    const char *key;        /* 键名（malloc 分配，_next 自动释放上次） */
    JsonVal val;            /* 值 */
    /* 内部状态 */
    const char *src;
    size_t pos;
    bool first;
} JsonObjectIter;

void json_obj_iter_init(JsonObjectIter *it, JsonVal obj);
bool json_obj_iter_next(JsonObjectIter *it);
void json_obj_iter_cleanup(JsonObjectIter *it);  /* 提前退出循环时调用 */

/* ============================================================
 * JSON Lines 追加写入
 * ============================================================ */

/* 向 JSONL 文件追加一行（自动加 \n） */
int jsonl_append(const char *path, const char *json_line);

#endif /* JSON_H */

/* ==== ba_store.h ==== */
#ifndef STORE_H
#define STORE_H


/*
 * 会话存储 — session 管理、conversation.jsonl 读写、stats.json 更新
 *
 * 目录结构（与 bash/go/rust 一致）：
 *   ~/.bash-agent/projects/<project-key>/<session-id>/
 *     conversation.jsonl  — 对话历史
 *     events.jsonl        — 事件日志
 *     stats.json          — 统计数据
 *     summary.md          — 上下文摘要
 *     plan.md             — 已确认的计划
 *     plan.draft          — 计划草稿
 */

/* 会话路径集 */
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

/* 根据 home, cwd, session_id 生成路径集 */
SessionPaths store_session_paths_for(const char *home, const char *cwd, const char *session_id);

/* 确保会话目录和文件存在 */
int store_session_init(const SessionPaths *p, int is_new);

/* 为子 agent 创建新会话（初始化目录） */
int store_session_init_sub(const SessionPaths *parent_paths, const SessionPaths *sub_paths, int fork);

/* 复制 conversation/summary/plan 到新 session（不 init child，对齐 bash/go/rust 版 store_session_fork） */
int store_session_fork(const SessionPaths *parent, const SessionPaths *child);

const char *store_session_image_dir(const SessionPaths *paths);

/* 从 cwd 生成 project key（简化为路径替换 / 为 -） */
char *store_session_project_key(const char *cwd);

/* 生成新的 session ID（格式: YYYYMMDD-HHMMSS-XXXX） */
char *session_new_id(void);

/* 查找最近一个会话（用于 --continue） */
char *store_session_resolve_continue(const char *home, const char *cwd);

/* 列出所有会话行（用于 --list-sessions），格式化行写入 out，返回行数 */
int store_session_list_rows(const char *home, const char *cwd, StrBuf *out);

/* ============================================================
 * conversation.jsonl 操作
 * ============================================================ */

/* 追加 user 消息 */
int store_conv_add_user(const char *path, const char *content);

/* 追加 assistant 消息（含 thinking, text, tool_calls） */
int store_conv_add_assistant(const char *path, const char *thinking, const char *text,
                       /* tool_calls: 数组 of {id, name, input_json}，count 个 */
                       int tool_count, const char **tool_ids,
                       const char **tool_names, const char **tool_inputs);

/* 追加 tool_result 消息 */
int store_conv_add_tool_results(const char *path, int count, const char **tool_use_ids,
                          const char **contents);

/* 读取所有行（JSON 字符串数组，每个元素需 free） */
int store_conv_line_count(const char *path, char ***out, int *out_count);

/* 保留最后 keep_lines 行 */
int store_conv_trim_tail(const char *path, int keep_lines);

/* 统计 user 输入消息数量 */
int store_conv_user_turn_count(const char *path);

/* 计算总字节数 */
long store_conv_total_bytes(const char *path);

/* ============================================================
 * stats.json 操作
 * ============================================================ */

/* 读取 stats（返回 JSON 字符串，需 free） */
char *store_stats_read(const char *path);

/* 更新 stats（读取→调用 callback→写回） */
typedef void (*stats_update_fn)(void *ctx, JsonVal stats);
int store_stats_update(const char *path, stats_update_fn fn, void *ctx);

/* 常用更新操作 */
void store_stats_add_int(JsonVal obj, const char *key, int delta);
void store_stats_set_int(JsonVal obj, const char *key, int value);

/* 简易文件级操作：直接读写 stats 文件中的整数字段 */
int store_stats_get_file_int(const char *path, const char *key);
void store_stats_set_int_file(const char *path, const char *key, int value);

void store_event_set_stream_json(int enabled);
int store_event_stream_json_enabled(void);

/* 从 stats 中读取整数值 */
int store_stats_get_int(JsonVal obj, const char *key);

/* ============================================================
 * events.jsonl 操作
 * ============================================================ */

/* 追加事件（JSON 字符串） */
int store_event_append(const SessionPaths *p, const char *json_str);

/* 读取所有事件行 */
int store_event_lines(const SessionPaths *p, char ***out, int *out_count);

/* ============================================================
 * summary / plan 文件操作
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
    BA_FMT_NONE,     /* SubAgent 子回合：只写 events.jsonl，不打 stdout */
} BaDisplayFormat;

/* 对齐 bash-agent DisplayMessage 字段子集 */
typedef struct {
    int type;
    char *content;      /* TEXT/THINKING/STOP(reason)/ERROR/TOOL_RESULT content/summary */
    char *tool_name;
    char *tool_id;
    char *tool_input;   /* 已是 JSON 文本 */
    char *session_id;   /* SubAgent/bg task id */
    int in_tokens, out_tokens, cache_read_tokens, cache_creation_tokens;
    int tool_exit_code;
} BaDisplayMsg;

typedef struct {
    char last_char[8];
    int prev_was_thinking;
    BaDisplayFormat format;
    FILE *out;          /* stream-json 目标（stdout） */
} BaDisplay;

void ba_disp_init(BaDisplay *d, BaDisplayFormat fmt);

/* 单条消息：stream-json 输出事件行；human 按颜色/截断规则渲染。
 * 同时返回需要写入 events.jsonl 的 JSON 文本（调用方 free），或 NULL。 */
char *ba_display_push(BaDisplay *d, const BaDisplayMsg *m);

/* 工具调用摘要（对齐 bash-agent agent_tool_display_summary） */
char *ba_tool_call_summary(const char *name, const char *input_json);

#endif /* BA_DISPLAY_H */

/* ==== ba_transport.h ==== */
#ifndef TRANSPORT_H
#define TRANSPORT_H


/*
 * 传输层 — SSE 流式解析与请求体构建（无 libcurl）
 *
 * HTTP 收发在 bb_http.c（libbb 原语，明文）；本文件只做协议层。
 */

/* SSE 事件回调 */
typedef enum {
    SSE_TEXT,               /* 文本增量 */
    SSE_THINKING,           /* 思考增量 */
    SSE_TOOL_CALL_START,    /* 工具调用开始（id + name 已知） */
    SSE_TOOL_INPUT_DELTA,   /* 工具调用 input_json 增量 */
    SSE_TOOL_CALL,          /* 工具调用完成（完整，无增量） */
    SSE_USAGE,              /* token 用量 */
    SSE_STOP,               /* 停止 */
    SSE_ERROR,              /* 错误 */
    SSE_RETRY,              /* 重试（清空当前累积） */
} SseEventType;

typedef struct {
    SseEventType type;
    char *content;           /* TEXT/THINKING/STOP/ERROR: 文本内容 */
    char *tool_id;           /* TOOL_CALL: 工具调用 ID */
    char *tool_name;         /* TOOL_CALL: 工具名 */
    char *tool_input;        /* TOOL_CALL: 工具参数 JSON */
    int in_tokens;           /* USAGE: 输入 token */
    int out_tokens;          /* USAGE: 输出 token */
    int cache_read_tokens;   /* USAGE: 缓存读取 token */
    int cache_creation_tokens; /* USAGE: 缓存创建 token */
} SseEvent;

typedef void (*sse_callback_fn)(void *ctx, const SseEvent *evt);

/* 流式 POST 请求，通过回调传递 SSE 事件 */
int http_post_sse(const char *url, const char **headers, int header_count,
                  const char *body, size_t body_len,
                  const char *provider,
                  sse_callback_fn callback, void *ctx,
                  volatile int *cancelled);

/* 流式回调上下文（SSE 行切分与 provider 分派的状态） */
typedef struct {
    int index;
    char *id;
    char *name;
    StrBuf arguments;
} OpenAIToolAccum;

typedef struct {
    sse_callback_fn callback;
    void *ctx;
    StrBuf line_buf;        /* 累积 SSE 行 */
    char *event;            /* Responses SSE 事件名 */
    char *provider;         /* "claude"、"openai" 或 "responses" */
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

/* 传输无关的 SSE 数据泵接口（由 ba_transport.c 实现，bb_http.c 调用） */
void sse_stream_init(StreamCtx *sctx, const char *provider,
                     sse_callback_fn callback, void *ctx,
                     volatile int *cancelled);
void sse_stream_free(StreamCtx *sctx);
/* 流结束收尾：残留 JSON 处理 + responses 终止检查 */
void sse_stream_finish(StreamCtx *sctx, const char *provider,
                       sse_callback_fn callback, void *ctx);
/* 喂入一块已解码的响应体字节；返回 0 表示取消，应中止传输 */
int sse_stream_feed(StreamCtx *sctx, const char *ptr, size_t len);

/* 解析 SSE 事件行（从 HTTP 响应体的 "data: ..." 行解析） */
int sse_parse_event(const char *provider, const char *data, size_t data_len,
                    sse_callback_fn callback, void *ctx);

/* ============================================================
 * SSE 累积器 — 用于 agent_loop 中收集流式事件
 * ============================================================ */

/* 累积的工具调用 */
typedef struct {
    char *id;
    char *name;
    StrBuf input_json;     /* 累积 input_json_delta */
} ToolCallAccum;

typedef struct {
    /* 累积的文本 */
    StrBuf text;
    StrBuf thinking;

    /* 累积的工具调用列表 */
    ToolCallAccum *tools;
    int tool_count;
    int tool_cap;

    /* 当前正在累积的 content_block 索引（-1 表示无） */
    int current_block_index;
    char *current_block_type;  /* "text", "thinking", "tool_use" */
    char *current_tool_id;     /* content_block_start 时的 tool id */
    char *current_tool_name;   /* content_block_start 时的 tool name */

    /* 统计 */
    int in_tokens;
    int out_tokens;
    int cache_read_tokens;
    int cache_creation_tokens;

    /* 停止原因 */
    char *stop_reason;

    /* 错误 */
    char *error;

    /* 状态标记 */
    int stopped;            /* 收到 stop 事件 */
} SseAccumulator;

/* 初始化/释放累积器 */
void sse_accum_init(SseAccumulator *acc);
void sse_accum_free(SseAccumulator *acc);

/* SSE 回调 — 累积事件到 SseAccumulator */
void sse_accum_callback(void *ctx, const SseEvent *evt);

/* 构建请求体 */
char *build_claude_request(const char *model, const char *system_prompt,
                           const char *tools_json,
                           char **conv_lines, int conv_line_count,
                           int max_tokens, const char *thinking, const char *effort);

/* 将 Claude 请求体转换为 OpenAI 格式 */
char *convert_to_openai(const char *claude_body);

/* 将 Claude 请求体转换为 Responses API 格式 */
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


/* Prompt 构建上下文（对应 bash-agent 的 Agent 字段子集） */
typedef struct {
    const char *cwd;        /* 当前工作目录 */
    const char *home;       /* $BB_AGENT_HOME（busyagent 根目录） */
    const char *plan;       /* PLAN_FILE 路径（session_dir/plan.md） */
    const char *plan_draft; /* PLAN_DRAFT_FILE 路径 */
} BaPromptCtx;

/* 构建完整 system prompt，返回需 free 的字符串 */
char *ba_build_prompt(const BaPromptCtx *ctx);

/* 加载技能内容（Skill 工具用），返回需 free 的字符串或 NULL。
 * 搜索序与 system prompt 的 skill-index 一致：
 *   $CWD/skills > ~/.agents/skills > $BB_AGENT_HOME/skills
 * agents_home 传 $HOME（可 NULL）；bag_home 传 $BB_AGENT_HOME（不可 NULL）。
 * out_skill_dir 出参为命中目录（可 NULL 不取）。 */
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
 *   - $BB_AGENT_HOME/tools.json (dynamic zone) is loaded on top; a
 *     missing/broken file only loses the dynamic sugar, never the
 *     builtins. Dynamic names may not shadow builtins.
 * paths is used by state tools (Plan*) for session placement. */
int ba_tools_init(const char *home, const SessionPaths *paths);

void ba_tools_free(void);

/* Re-point the built-in state tools (Plan*) at another session. */
void ba_tools_set_paths(const SessionPaths *paths);

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
