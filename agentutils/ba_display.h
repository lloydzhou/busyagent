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
#include "ba_util.h"

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
