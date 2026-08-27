# SubAgent 实现检讨（PR #3）

## 结论
busyagent 当前的 SubAgent 是我自创的 fork+exec 自举方案，bash-agent 里**不存在**
这种设计。正确形状是从 bash-agent 原样移植的进程内调用：

```
tools.c:972   SubAgent 分支 → 占位符 "SubAgent handled by agent layer"
agent.c:1050  agent_loop 工具分发处拦截：
                json_get(prompt/description/fork)
                → agent_handle_sub_agent(agent, prompt, description, fork_mode)
agent.c:1769  agent_handle_sub_agent：
                depth>=1 拒绝（"Error: sub-agent recursion limit reached; ..."）
                sub_sid = "sub_" + session_new_id()
                写 sub_agent_start 事件（含 prompt/description/fork 字段）
                fork=true 时主线程 store_session_init_sub(&agent->paths,&sub_paths,1)
                子线程构造 sub Agent（字段逐一继承：max_turns/max_tokens/
                tool_timeout_secs/sub_agent_depth+1/thinking/effort...）
                → agent_loop(sub, prompt, "user_progress")   ← 纯函数调用
                → 从 sub.paths.conversation 倒序取最后 assistant text 作为结果
```

## 我做错的抽象（及根因）
| 自创物 | bash-agent 对应 | 为什么错 |
|---|---|---|
| fork+exec busyagent 自己 | 进程内 calloc 一个子 Agent 结构再调 agent_loop | 为绕开 turn 循环与全局态耦合的重构，用进程边界掩盖问题 |
| BB_AGENT_OUTPUT / BB_AGENT_DEPTH / BB_AGENT_FORK_FROM / BB_AGENT_SESSION 私有 env 协议 | 结构体成员直接复制 | 同进程的字段传递被迫变成跨进程协议，每补一个能力就多一个发明 |
| SubAgent 逻辑写在 ba_tools.c 工具分支内 | tools.c 只放占位符，实现在 agent_loop 分发层 | 层次放错：委托语义属于 loop/agent 层 |
| `$BB_AGENT_HOME/tasks/<id>.log` 后台日志路径 | mkstemp("/tmp/async_bash_XXXXXX") + msgqueue 回注 | 路径凭感觉起名；回注通道缺失未如实说明 |

## 整改原则（写入 review 备忘）
> **除非 busybox 没有的设施必须新造，否则一律按 bash-agent 的结构原样移植；
> 不确定时先引用源码行号确认形状，再做最小适配。**

已核实的同构映射（后续整改以此为准）：
- `agent_loop(sub)` ↔ 抽取后的 `ba_run_session(&child_ctx)` —— 这是唯一必要的重构
  （把 main 内联的循环提为函数），**不是**引入新的 runner hook/BaRunSpec 泛化层。
- `store_session_init_sub(&parent->paths,&sub_paths,fork)` 直接可用（ba_store 已移植）。
- SubAgent 结果提取（倒序找最后 assistant text）照抄 agent.c:1693-1730。
- 子回合 display 静默需求用 fmt=NONE 表示（display 队列不存在导致的等价替代，
  行为差异点在 PR 描述里声明）。

## 待办清单（按序执行）
1. busyagent.c：提取 ba_run_session(BaRunCtx*)（机械搬运，不改逻辑）；
2. 加 ba_handle_sub_agent()：照抄 agent_handle_sub_agent，同步版（无 display 线程）；
3. turn 循环分发处拦截 SubAgent（先于 ba_tool_execute，含 background bash 特判顺序对齐）；
4. ba_tools.c SubAgent 分支恢复占位符语义；
5. 删除全部私有 env 协议（setenv/unsetenv 四处）；
6. e2e：N1 委托、N2 fork 继承、N3 depth 文案逐字校验。

—— 已作为本 PR review 记录留存，后续任何偏离需在此追加理由。
