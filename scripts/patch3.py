import re

c = open('agentutils/busyagent.c').read()

# 1) 错误分支（sse_rc!=0）
pat_err = re.compile(
    r'\t\t\tif \(!accum->error\)\n'
    r'\t\t\t\tba_render_error\(&tctx, "HTTP request failed"\);\n'
    r'\t\t\tba_render_stop\(&tctx, "error"\);')
rep_err = '''\t\t\tif (!accum->error)
\t\t\t\tba_push_display(&tctx, &(BaDisplayMsg){
\t\t\t\t\t.type = BA_DM_ERROR, .content = (char *)"HTTP request failed"});
\t\t\tba_push_display(&tctx, &(BaDisplayMsg){
\t\t\t\t.type = BA_DM_STOP, .content = (char *)"error"});'''
assert pat_err.search(c), "err branch"
c = pat_err.sub(rep_err, c)

# 2) 工具轮 flush
c = c.replace('\t\t\t\tba_flush_turn_events(&tctx);',
              '\t\t\t\tba_flush_turn_events(&tctx);   /* 中间轮事件也要进流与 trace */')

# 3) tool_result 渲染点
pat_tr = re.compile(
    r'\t\t\t\tba_render_tool_result\(&tctx, accum->tools\[i\]\.id,\s*\n'
    r'\s*accum->tools\[i\]\.name, out\);')
rep_tr = '''\t\t\t\t{
\t\t\t\t\tBaDisplayMsg dm;
\t\t\t\t\tmemset(&dm, 0, sizeof(dm));
\t\t\t\t\tdm.type = BA_DM_TOOL_RESULT;
\t\t\t\t\tdm.tool_id = accum->tools[i].id;
\t\t\t\t\tdm.tool_name = accum->tools[i].name;
\t\t\t\t\tdm.content = out;
\t\t\t\t\tba_push_display(&tctx, &dm);
\t\t\t\t}'''
assert pat_tr.search(c), "tool_result site"
c = pat_tr.sub(rep_tr, c)

# 4) stop 正常分支
pat_stop = re.compile(r'\t\t\tba_render_stop\(&tctx, accum->stop_reason \? accum->stop_reason : "end_turn"\);')
rep_stop = '''\t\t\tba_push_display(&tctx, &(BaDisplayMsg){
\t\t\t\t.type = BA_DM_STOP,
\t\t\t\t.content = accum->stop_reason ? accum->stop_reason : "end_turn" });'''
assert pat_stop.search(c), "stop"
c = pat_stop.sub(rep_stop, c)

open('agentutils/busyagent.c','w').write(c)
print('step3 done')
