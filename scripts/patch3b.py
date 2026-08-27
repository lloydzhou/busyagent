import re

c = open('agentutils/busyagent.c').read()

# 1) 错误分支（精确文本）
old1 = '''\t\t\tif (!accum->error)
\t\t\t\tba_render_error(&tctx, "HTTP request failed");
\t\t\tba_render_stop(&tctx, "error");'''
new1 = '''\t\t\tif (!accum->error)
\t\t\t\tba_push_display(&tctx, &(BaDisplayMsg){
\t\t\t\t\t.type = BA_DM_ERROR, .content = (char *)"HTTP request failed"});
\t\t\tba_push_display(&tctx, &(BaDisplayMsg){
\t\t\t\t.type = BA_DM_STOP, .content = (char *)"error"});'''
assert old1 in c, "err"
c = c.replace(old1, new1)

# 2) tool_result
old2 = '''\t\t\t\tba_render_tool_result(&tctx, accum->tools[i].id,
\t\t\t\t\t\t\t      accum->tools[i].name, out);'''
new2 = '''\t\t\t\t{
\t\t\t\t\tBaDisplayMsg dm;
\t\t\t\t\tmemset(&dm, 0, sizeof(dm));
\t\t\t\t\tdm.type = BA_DM_TOOL_RESULT;
\t\t\t\t\tdm.tool_id = accum->tools[i].id;
\t\t\t\t\tdm.tool_name = accum->tools[i].name;
\t\t\t\t\tdm.content = out;
\t\t\t\t\tba_push_display(&tctx, &dm);
\t\t\t\t}'''
assert old2 in c, "tool_result"
c = c.replace(old2, new2)

# 3) stop 正常分支（同时删掉旧 output_json 尾换行判断——human TEXT 渲染已管理行尾；
#    bash-agent 在 STOP 后由 main 打印尾 \n?保持我们原样在 human 补 \n）
old3 = '''\t\t\tba_render_stop(&tctx, accum->stop_reason ? accum->stop_reason : "end_turn");
\t\t\tif (!tctx.output_json)
\t\t\t\tfwrite("\\n", 1, 1, stdout);'''
new3 = '''\t\t\tba_push_display(&tctx, &(BaDisplayMsg){
\t\t\t\t.type = BA_DM_STOP,
\t\t\t\t.content = accum->stop_reason ? accum->stop_reason : "end_turn" });
\t\t\tif (tctx.disp.format == BA_FMT_HUMAN)
\t\t\t\tfwrite("\\n", 1, 1, stdout);'''
assert old3 in c, "stop"
c = c.replace(old3, new3)

open('agentutils/busyagent.c','w').write(c)
print('step3 done')
