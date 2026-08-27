import re

c = open('agentutils/busyagent.c').read()

# 1) TurnCtx 换为 display 驱动结构
start = c.index('/* ---- per-turn context: accumulator + renderer + trace ---- */')
end = c.index('/* ---- context trimming')
new_mid = '''/* ---- per-turn context: bash-agent stream_display_callback 的同步等价 ---- */

typedef struct {
	SseAccumulator accum;
	BaDisplay disp;
	const SessionPaths *paths;
	int verbose;
} TurnCtx;

/* display_msg_to_event：与 bash-agent 逐字一致的事件序列化 */
static char *ba_display_to_event(const BaDisplayMsg *m)
{
	StrBuf buf;
	sb_init(&buf);
	switch (m->type) {
	case BA_DM_TEXT:
		sb_append(&buf, "{\\"type\\":\\"text\\",\\"content\\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_THINKING:
		sb_append(&buf, "{\\"type\\":\\"thinking\\",\\"content\\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_TOOL_CALL:
		sb_append(&buf, "{\\"type\\":\\"tool_call\\",\\"name\\":");
		sb_append_json_string(&buf, m->tool_name ? m->tool_name : "");
		sb_append(&buf, ",\\"id\\":");
		sb_append_json_string(&buf, m->tool_id ? m->tool_id : "");
		sb_append(&buf, ",\\"input\\":");
		if (m->tool_input) sb_append(&buf, m->tool_input); else sb_append(&buf, "{}");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_TOOL_RESULT:
		sb_append(&buf, "{\\"type\\":\\"tool_result\\",\\"tool_use_id\\":");
		sb_append_json_string(&buf, m->tool_id ? m->tool_id : "");
		sb_append(&buf, ",\\"name\\":");
		sb_append_json_string(&buf, m->tool_name ? m->tool_name : "");
		sb_append(&buf, ",\\"content\\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_USAGE:
		sb_appendf(&buf,
			"{\\"type\\":\\"usage\\",\\"input_tokens\\":%d,\\"output_tokens\\":%d,"
			"\\"cache_read_input_tokens\\":%d,\\"cache_creation_input_tokens\\":%d,"
			"\\"kind\\":\\"agent\\"}",
			m->in_tokens, m->out_tokens,
			m->cache_read_tokens, m->cache_creation_tokens);
		break;
	case BA_DM_STOP:
		sb_append(&buf, "{\\"type\\":\\"stop\\",\\"reason\\":");
		sb_append_json_string(&buf, m->content ? m->content : "");
		sb_append_char(&buf, '}');
		break;
	case BA_DM_ERROR:
		sb_append(&buf, "{\\"type\\":\\"error\\",\\"message\\":");
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

'''
c = c[:start] + new_mid + c[end:]

# 2) 旧 render 函数群 + callback 替换
i0 = c.find('static void ba_render(')
i1 = c.index('/* ---- context trimming', i0) if i0 >= 0 else -1
assert i1 > i0
cb = '''/* SSE 回调：对齐 bash-agent stream_display_callback —— 事件即时推送 */
static void ba_sse_callback(void *ctx, const SseEvent *evt)
{
	TurnCtx *t = (TurnCtx *)ctx;
	BaDisplayMsg dm;

	memset(&dm, 0, sizeof(dm));
	if (evt->type == SSE_TEXT) {
		dm.type = BA_DM_TEXT;
		dm.content = (char *)evt->content;
		ba_display_push(t, &dm);
	} else if (evt->type == SSE_ERROR) {
		dm.type = BA_DM_ERROR;
		dm.content = (char *)(evt->content ? evt->content : "unknown error");
		ba_display_push(t, &dm);
	}
	sse_accum_callback(&t->accum, evt);
}

'''
c = c[:i0] + cb + c[i1:]
open('agentutils/busyagent.c','w').write(c)
print('renderer replaced; total lines:', len(c.splitlines()))
