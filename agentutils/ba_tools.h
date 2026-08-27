#ifndef BA_TOOLS_H
#define BA_TOOLS_H

#include <stddef.h>

/* 初始化工具表：$BB_AGENT_HOME/tools.json 不存在时用内嵌默认版生成，
 * 之后每次读取该文件；解析失败回退内嵌版（stderr 警告）。
 * 返回工具数量，0 表示不可用。 */
int ba_tools_init(const char *home, const SessionPaths *paths);

void ba_tools_free(void);

/* 导出工具表模板（-i）。0 成功；-1 已存在；-2 写失败。 */
int ba_tools_write_template(const char *path);

/* 当前生效的 tools 数组 JSON 文本（与执行表同源），调用方 free */
char *ba_tools_json(void);

/* 按表执行一次工具调用。返回 malloc 的输出文本，永不返回 NULL。 */
char *ba_tool_execute(const char *name, const char *input_json, int timeout_ms);

#endif /* BA_TOOLS_H */
