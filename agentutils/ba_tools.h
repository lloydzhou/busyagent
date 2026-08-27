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
#include "ba_store.h"

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
