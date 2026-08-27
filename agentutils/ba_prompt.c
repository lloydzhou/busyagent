/*
 * ba_prompt.c - system prompt construction, ported verbatim from
 * bash-agent's agent_build_prompt suite (agent.c). Section order, XML
 * tags and section text are kept identical; adaptations are limited to:
 *   - agent identity string: bash-agent -> busyagent
 *   - skill dirs: .claude/skills dropped, replaced by generic agent dirs
 *     (cwd/skills, ~/.agents/skills, $BB_AGENT_HOME/skills)
 *   - Bash background / SubAgent sync guidance lines match busyagent's
 *     actual single-turn semantics
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include "libbb.h"
#include <dirent.h>
#include <sys/utsname.h>
#include "ba_util.h"
#include "ba_prompt.h"

/* ============================================================
 * system prompt 构建 — 辅助函数（逐字移植）
 * ============================================================ */

/* 追加 XML section：<tag>\ncontent\n</tag> 或 <tag name="name">\ncontent\n</tag> */
static void prompt_append_attr_escaped(StrBuf *buf, const char *src) {
    if (!src) return;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"':  sb_append(buf, "\\\""); break;
            case '\\': sb_append(buf, "\\\\"); break;
            case '\b': sb_append(buf, "\\b"); break;
            case '\f': sb_append(buf, "\\f"); break;
            case '\n': sb_append(buf, "\\n"); break;
            case '\r': sb_append(buf, "\\r"); break;
            case '\t': sb_append(buf, "\\t"); break;
            default:
                if (c < 0x20) sb_appendf(buf, "\\u%04x", c);
                else sb_append_char(buf, c);
                break;
        }
    }
}

static void prompt_append_section(StrBuf *buf, const char *tag,
                                   const char *content, const char *name) {
    if (!content || !content[0]) return;
    size_t content_len = strlen(content);
    while (content_len > 0 &&
           (content[content_len - 1] == '\n' || content[content_len - 1] == '\r')) {
        content_len--;
    }
    if (content_len == 0) return;
    if (name && name[0]) {
        sb_appendf(buf, "<%s name=\"", tag);
        prompt_append_attr_escaped(buf, name);
        sb_append(buf, "\">\n");
        sb_appendn(buf, content, content_len);
        sb_appendf(buf, "\n</%s>\n", tag);
    } else {
        sb_appendf(buf, "<%s>\n", tag);
        sb_appendn(buf, content, content_len);
        sb_appendf(buf, "\n</%s>\n", tag);
    }
}

/* util_path_join 等价物（busybox 侧无该 helper） */
static char *pj(const char *a, const char *b) {
    return xasprintf("%s/%s", a, b);
}

/* util_read_file 等价物 */
static char *read_all(const char *path) {
    int fd = open(path, O_RDONLY);
    long sz;
    char *buf;
    if (fd < 0)
        return NULL;
    sz = xlseek(fd, 0, SEEK_END);
    xlseek(fd, 0, SEEK_SET);
    buf = xzalloc(sz + 1);
    sz = full_read(fd, buf, sz);
    if (sz < 0) sz = 0;
    buf[sz] = '\0';
    close(fd);
    return buf;
}

/* 检测 locale：LC_ALL → LC_MESSAGES → LANG → "en_US"，去掉 .xxx 后缀 */
static const char *detect_locale(void) {
    static char buf[128];
    const char *loc = getenv("LC_ALL");
    if (!loc || !loc[0]) loc = getenv("LC_MESSAGES");
    if (!loc || !loc[0]) loc = getenv("LANG");
    if (!loc || !loc[0]) loc = "en_US";
    strncpy(buf, loc, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    {
        char *dot = strchr(buf, '.');
        if (dot) *dot = '\0';
    }
    return buf;
}

/* 在 dir 下查找指令文件（AGENTS.md / AGENT.md；通用 agent 目录，不绑定特定工具），
 * 返回内容（需 free），无则 NULL。bash-agent 还会找 CLAUDE.md 变体，此处按
 * 项目决定不绑 Claude 目录。 */
static char *find_instruction_file(const char *dir) {
    const char *candidates[] = { "AGENTS.md", "AGENT.md", NULL };
    int i;

    for (i = 0; candidates[i]; i++) {
        char *path = pj(dir, candidates[i]);
        char *content = read_all(path);
        free(path);
        if (content && content[0])
            return content;
        free(content);
    }
    return NULL;
}

/* 从 SKILL.md 内容中提取摘要：优先 description: 行，否则取第一个非空非标题非---行 */
static void extract_skill_summary(const char *md, StrBuf *out) {
    const char *p = md;
    char line[1024];
    int found = 0;
    char fallback[1024] = "";

    while (*p) {
        /* 读取一行 */
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(line) - 1) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') p++;

        /* trim */
        {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            {
                char *e = s + strlen(s);
                while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
                *e = '\0';
                if (*s == '\0') continue;

                /* description: 行 */
                if (strncmp(s, "description:", 12) == 0) {
                    char *val = s + 12;
                    while (*val == ' ' || *val == '\t') val++;
                    /* 去掉引号 */
                    size_t vl = strlen(val);
                    if (vl >= 2 && ((val[0] == '"' && val[vl-1] == '"') ||
                                    (val[0] == '\'' && val[vl-1] == '\''))) {
                        val++;
                        vl -= 2;
                    }
                    sb_appendn(out, val, vl);
                    found = 1;
                    return;
                }
                /* fallback：非标题、非---、非``` */
                if (!found && fallback[0] == '\0' && s[0] != '#' &&
                    !(s[0] == '-' && s[1] == '-' && s[2] == '-' && s[3] == '\0') &&
                    !(s[0] == '`' && s[1] == '`' && s[2] == '`')) {
                    strncpy(fallback, s, sizeof(fallback) - 1);
                    fallback[sizeof(fallback) - 1] = '\0';
                }
            }
        }
    }
    if (!found && fallback[0]) {
        sb_append(out, fallback);
    }
}

/* 扫描 skill 目录列表（去重），构建 skill-index。
 * busyagent 目录序：cwd/skills > $HOME/.agents/skills > bag_home/skills */
static void build_skill_index(StrBuf *index, const char *cwd,
                              const char *agents_home, const char *bag_home) {
    char *dirs[4];
    int dcount = 0;
    {
        dirs[dcount++] = xasprintf("%s/skills", cwd);
        if (agents_home && agents_home[0])
            dirs[dcount++] = xasprintf("%s/.agents/skills", agents_home);
        if (bag_home && bag_home[0])
            dirs[dcount++] = xasprintf("%s/skills", bag_home);
    }

    /* 去重 seen 列表 */
    char *seen[256];
    int seen_count = 0;

    for (int d = 0; d < dcount; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            /* 检查是否已 seen */
            int dup = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen[s], ent->d_name) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            /* 检查 SKILL.md 是否存在 */
            char *skill_md = xasprintf("%s/%s/SKILL.md", dirs[d], ent->d_name);
            char *md_content = read_all(skill_md);
            free(skill_md);
            if (!md_content || !md_content[0]) { free(md_content); continue; }

            /* 记录 seen */
            if (seen_count < 256) seen[seen_count++] = util_strdup(ent->d_name);

            /* 提取摘要 */
            StrBuf summary;
            sb_init(&summary);
            extract_skill_summary(md_content, &summary);
            free(md_content);

            sb_appendf(index, "- %s", ent->d_name);
            if (summary.len > 0) sb_appendf(index, ": %s", summary.data);
            sb_append_char(index, '\n');
            sb_free(&summary);
        }
        closedir(dir);
    }
    for (int d = 0; d < dcount; d++) free(dirs[d]);
    for (int s = 0; s < seen_count; s++) free(seen[s]);
}

char *ba_load_skill(const char *skill_name, const char *cwd,
                    const char *agents_home, const char *bag_home,
                    char **out_skill_dir) {
    char *dirs[4];
    int dcount = 0;
    char *content = NULL;
    int d;

    dirs[dcount++] = xasprintf("%s/skills", cwd);
    if (agents_home && agents_home[0])
        dirs[dcount++] = xasprintf("%s/.agents/skills", agents_home);
    if (bag_home && bag_home[0])
        dirs[dcount++] = xasprintf("%s/skills", bag_home);

    for (d = 0; d < dcount && !content; d++) {
        char *skill_dir_path = xasprintf("%s/%s", dirs[d], skill_name);
        char *md_path = xasprintf("%s/SKILL.md", skill_dir_path);
        content = read_all(md_path);
        free(md_path);
        if (content) {
            /* 替换 ${BA_AGENT_SKILL_DIR} 占位符 */
            const char *placeholder = strstr(content, "${BA_AGENT_SKILL_DIR}");
            if (placeholder) {
                StrBuf replaced;
                sb_init(&replaced);
                size_t prefix_len = placeholder - content;
                sb_appendn(&replaced, content, prefix_len);
                sb_append(&replaced, skill_dir_path);
                sb_append(&replaced, placeholder + strlen("${BA_AGENT_SKILL_DIR}"));
                free(content);
                content = replaced.data;
            }
            /* 格式: Base directory: <dir>\n\n<content>（与 bash-agent 一致） */
            StrBuf full;
            sb_init(&full);
            sb_appendf(&full, "Base directory: %s\n\n%s", skill_dir_path, content);
            free(content);
            content = full.data;
            if (out_skill_dir) *out_skill_dir = skill_dir_path;
            else free(skill_dir_path);
        } else {
            free(skill_dir_path);
        }
    }
    for (d = 0; d < dcount; d++) free(dirs[d]);
    return content;
}

/* ============================================================
 * system prompt 构建 — 主函数（块顺序与文本对齐 bash-agent）
 * ============================================================ */

char *ba_build_prompt(const BaPromptCtx *ctx) {
    StrBuf buf;
    sb_init(&buf);

    const char *locale = detect_locale();
    int is_zh = (locale[0] == 'z' && locale[1] == 'h');

    /* 1. agent-identity */
    {
        const char *identity = "You are busyagent, a lightweight coding agent that runs as a busybox applet.";
        if (is_zh) identity = "你是 busyagent，一个以 busybox applet 形式运行的轻量级编码智能体。";
        prompt_append_section(&buf, "agent-identity", identity, NULL);
    }

    /* 2. environment */
    {
        struct utsname uts;
        StrBuf env;
        char *sh_env = getenv("SHELL");
        sb_init(&env);
        sb_appendf(&env, "lang: %s\n", detect_locale());
        sb_appendf(&env, "pwd: %s\n", ctx->cwd ? ctx->cwd : "?");
        sb_appendf(&env, "home: %s\n", ctx->home ? ctx->home : "?");
        if (uname(&uts) == 0)
            sb_appendf(&env, "platform: %s\n", uts.sysname);
        else
            sb_append(&env, "platform: unknown\n");
        sb_appendf(&env, "shell: %s", sh_env && sh_env[0] ? sh_env : "/bin/sh");
        prompt_append_section(&buf, "environment", env.data, NULL);
        sb_free(&env);
    }

    /* 3. rules */
    {
        const char *rules = "- Be concise and concrete. Lead with the answer. Use short sections or bullets when they improve readability. No pleasantries, no explanations unless asked. Raw results only.\n"
                            "- Prefer safe, exact edits.\n"
                            "- Report failures clearly.";
        prompt_append_section(&buf, "rules", rules, NULL);
    }

    /* 4. using-your-tools */
    {
        const char *tool_guidance =
            "- Use Read for a single file. If you need multiple files, call Read multiple times.\n"
            "- Read supports optional offset and limit parameters to read specific line ranges (saves tokens for large files). Output includes line numbers.\n"
            "- Use Glob and Grep for one pattern at a time.\n"
            "- Grep supports a context parameter to show surrounding lines — use it to get enough text for Edit directly from Grep output, avoiding a separate Read.\n"
            "- Use multiple tool calls in one response when they are independent.\n"
            "- Prefer dedicated tools over Bash when a dedicated tool fits the task.\n"
            "- For Edit: copy old_string exactly (including whitespace/indent/newlines). If you already know the location from prior context, use Read with offset/limit. If you need to locate the text first, use Grep with context — its output is often sufficient for Edit without an extra Read.\n"
            "- For skills, first check the skill-index section, then use Skill(name) for the matching skill.\n"
            "- Bash supports background=true for long-running commands. Returns task_id immediately; this build delivers the output by writing it to a temp file - read that file with Read to fetch results (bash-agent injects it via async events instead).";
        prompt_append_section(&buf, "using-your-tools", tool_guidance, NULL);
    }

    /* 5. sub-agent-guidance（同步语义微调，其余逐字） */
    {
        const char *sag =
            "- **When to use**: delegating independent sub-tasks that do NOT need your current conversation context — e.g. investigating a separate file, running a focused search, testing a hypothesis in isolation.\n"
            "- **Recursion limit**: only the main agent may launch SubAgent. A child agent must not call SubAgent again; the runtime rejects nested launches.\n"
            "- **When NOT to use**: tasks that depend on your working context, conversation history, or intermediate state. The child agent starts with a blank slate.\n"
            "- **Prompt design**: write a complete, self-contained prompt. Include all file paths, function names, error messages, and constraints the child needs. Assume zero shared context.\n"
            "- **Result handling**: this build runs the sub-agent synchronously; its final answer is returned directly as this call's result. Interpret it in your next turn before acting.";
        prompt_append_section(&buf, "sub-agent-guidance", sag, NULL);
    }

    /* 6. todo-guidance（逐字） */
    {
        const char *todo =
            "- Use TodoWrite proactively for complex multi-step implementation, debugging, refactoring, review, or multi-file tasks.\n"
            "- Do not use TodoWrite for trivial single-step, single-command, or purely informational requests.\n"
            "- After receiving a non-trivial task, create an initial checklist before or as you begin work.\n"
            "- When you use TodoWrite, write the full updated checklist for the current session, not a partial diff.\n"
            "- Keep the checklist short, concrete, and actionable.\n"
            "- Prefer exactly one in_progress item when work is actively underway.\n"
            "- Mark items completed immediately after finishing them, and remove stale items that no longer matter.";
        prompt_append_section(&buf, "todo-guidance", todo, NULL);
    }

    /* 7. plan-lifecycle-guidance */
    {
        StrBuf plg;
        sb_init(&plg);
        sb_append(&plg, "- **PLANNING WORKFLOW** — For complex multi-step tasks (3+ steps OR multi-file OR user requests planning)\n");
        sb_appendf(&plg, "- **Files**: PLAN_DRAFT_FILE: %s | PLAN_FILE: %s\n",
                   ctx->plan_draft ? ctx->plan_draft : "<not set>",
                   ctx->plan ? ctx->plan : "<not set>");
        sb_append(&plg, "- **Why draft first?** Writing to PLAN_FILE immediately invalidates the system prompt cache. Use PLAN_DRAFT_FILE for all drafting iterations to avoid this cost.\n");
        sb_append(&plg, "- **Drafting phase** (PLAN_DRAFT_FILE non-empty → you are drafting):\n"
                        "  Every user reply MUST be classified as exactly ONE of:\n"
                        "  ① REVISE (any feedback/question/change) → Write/Edit PLAN_DRAFT_FILE → ask confirmation → stay in drafting\n"
                        "  ② CONFIRM (explicit ok/go/confirmed) → call PlanConfirm IMMEDIATELY (before any other action) → TodoWrite checklist → execute\n"
                        "  ③ CANCEL (explicit cancel/forget it) → empty out PLAN_DRAFT_FILE → exit to idle\n"
                        "  ⚠ On CONFIRM you MUST call PlanConfirm first — no edits, no tool calls before it.\n");
        sb_append(&plg, "- **Execution phase**: after PlanConfirm → TodoWrite checklist → execute tasks → PlanClear when all done\n"
                        "- **Plan vs Todo**: PLAN_FILE=locked plan (only via PlanConfirm), PLAN_DRAFT_FILE=draft (edit freely), TodoWrite=progress tracker. Do NOT mix.");
        prompt_append_section(&buf, "plan-lifecycle-guidance", plg.data, NULL);
        sb_free(&plg);
    }

    /* 8. instruction-files */
    {
        StrBuf ifiles;
        char *gc = find_instruction_file(ctx->home);
        char *pc = ctx->cwd ? find_instruction_file(ctx->cwd) : NULL;

        sb_init(&ifiles);
        if (gc && gc[0]) {
            prompt_append_section(&ifiles, "instruction-file", gc, "global");
        }
        if (pc && pc[0]) {
            prompt_append_section(&ifiles, "instruction-file", pc, "project");
        }
        if (ifiles.len > 0 && ifiles.data[ifiles.len - 1] == '\n')
            sb_truncate(&ifiles, ifiles.len - 1);
        prompt_append_section(&buf, "instruction-files", ifiles.data, NULL);
        sb_free(&ifiles);
        free(gc);
        free(pc);
    }

    /* 9. skill-index */
    {
        StrBuf si;
        sb_init(&si);
        build_skill_index(&si, ctx->cwd, getenv("HOME"), ctx->home);
        if (si.len > 0 && si.data[si.len - 1] == '\n')
            sb_truncate(&si, si.len - 1);
        prompt_append_section(&buf, "skill-index", si.data, NULL);
        sb_free(&si);
    }

    /* 11. current-plan */
    {
        char *plan = ctx->plan ? read_all(ctx->plan) : NULL;
        if (plan && plan[0]) {
            /* bash 版 name 属性 = plan 文件路径 */
            prompt_append_section(&buf, "current-plan", plan, ctx->plan);
        }
        free(plan);
    }

    /* 13. output-language */
    {
        StrBuf ol;
        sb_init(&ol);
        if (is_zh) {
            sb_append(&ol, "再次强调：必须使用中文进行所有输出，包括你的思考过程（Chain of Thought/推理/thinking）！严禁在思考或回答中出现任何英文内容！");
        } else {
            sb_appendf(&ol, "MUST use \"%s\" for all output, including your Chain of Thought/reasoning/thinking! Never mix languages! Code, commands, and file content remain as-is.", locale);
        }
        prompt_append_section(&buf, "output-language", ol.data, NULL);
        sb_free(&ol);
    }

    /* 去掉末尾 \n（bash 版 printf '%s' "${output%$'\\n'}"） */
    if (buf.len > 0 && buf.data[buf.len - 1] == '\n') {
        buf.data[buf.len - 1] = '\0';
        buf.len--;
    }

    return buf.data;
}
