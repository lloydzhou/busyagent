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
#include "libbb.h"
#include <string.h>
#include <stdio.h>
#include "ba_util.h"
#include "ba_json.h"
#include "ba_display.h"

/* ---- DisplayState（逐字移植） ---- */
static void ds_init(BaDisplay *ds) {
    memset(ds, 0, sizeof(*ds));
    ds->last_char[0] = '\n';
    ds->last_char[1] = '\0';
    ds->prev_was_thinking = 0;
}

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

/* linenoiseWrite 的非交互等价（busyagent 无 raw-mode 终端） */
static void lw_write(const char *s, size_t n) {
    fwrite(s, 1, n, stdout);
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
    ds_init(d);
    d->format = fmt;
    d->out = stdout;
}

/* ---- tool_call 摘要（逐字移植 agent_tool_display_summary） ---- */
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
            /* 替换换行为空格，截断过长命令（对齐 bash 版行为） */
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

/* ---- 渲染主体：逐字移植 render_message 双分支 ---- */
char *ba_display_push(BaDisplay *ds, const BaDisplayMsg *msg)
{
    StrBuf buf;

    if (ds->format == BA_FMT_NONE)
        return NULL;

    if (ds->format == BA_FMT_STREAM_JSON) {
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
            sb_append(&buf, (msg->tool_input && msg->tool_input[0]) ? msg->tool_input : "{}");
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
        default:
            sb_free(&buf);
            return NULL;
        }
        fprintf(ds->out, "%s\n", buf.data);
        fflush(ds->out);
        return buf.data;
    }

    /* human 模式（ANSI/截断规则逐字移植） */
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
            lw_printf("%s\n", msg->content);
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

    default:
        break;
    }
    return NULL;
}
