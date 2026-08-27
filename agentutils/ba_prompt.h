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

#include "ba_store.h"

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
