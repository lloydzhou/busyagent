# 对齐矩阵：bash-agent 完整内置 busybox（方向修正版）

> 方向变更：本项目从「参照 bash-agent 重实现」修正为
> **将 bash-agent 完整内置到 busybox（agentutils/），仅替换网络传输层**。
> 本文件是基准矩阵与迁移蓝图，issue #1 中与本表冲突的描述以本表为准。

## A. 文件级映射（搬运策略）

| bash-agent (c/) | 行数 | 迁移方式 | busyagent 目标 |
|---|---|---|---|
| json.c/h | 467/125 | 已 0-diff 移植 | **改名回 json.c/h** |
| util.c/h | 347/87 | 已 0-diff 移植 | util.c/h |
| store.c/h | 761/137 | 已移植(+有意差异) | store.c/h |
| transport.c/h | 1433/127 | 解析/转换=0-diff；**curl 层替换为 bb_http** | transport.c/h |
| protocol.c/h | 165/120 | 未搬（独立请求构建器，transport 内已有等价） | 待对齐：确认是否仍需 |
| tools.c/h | 1025/33 | 部分重写(11 builtin + 动态区) | 回归原名+逐字对齐工具面 |
| agent.c/h | 3046/157 | 大幅简化(ba_run_session) | **核心缺口**：agent_loop 全貌 |
| cagent.c | 460 | 自写 main | **改名回 cagent 形态**、flag 全集 |
| display.c/h | 395/35 | 同步简化版 ba_display | display.c/h；queue 由线程替代方案承载 |
| msgqueue.c/h | 109/59 | 未搬（phase1 砍） | **决策点 D1** |
| readline.c/h (+linenoise) | 279+ | 未搬 | **决策点 D2**：linenoise 可无依赖整体搬入 |
| test_classify/continue/transport | 457 | 未搬 | 搬入后直接编译复用（改名后 include 即通） |

## B. 能力级差距（按严重度）

1. **交互 REPL + linenoise**（interactive = !prompt && isatty）
2. **display/input 线程 & msgqueue**（异步渲染、SubAgent/bg 结果事件回注 <async_task_result>）
3. **SubAgent 异步**：bash-agent 是 detached pthread + sub_result_queue；当前同步阻塞版只能算降级
4. **cagent CLI flag 全集**：--list-sessions / --fork / --serve(serve-port/bind) / --thinking / --effort / --max-context / --max-tokens / --tool-timeout / --tool-result-max-bytes 等
5. **agent.c 完整 loop 细节**：stats.json 每 turn 更新、compact 流程、dp_config、context_update、replay(agent_replay_events)、title 更新等 ~20 个子功能
6. **tools.json 动态表语义**：运行时 truth 的 exec 分发（当前"可选糖"定位需校正）
7. **env 前缀族**：BASH_AGENT_* ↔ BB_AGENT_* 全量映射表
8. **events.jsonl 全事件集**：user_input/text/thinking/tool_call/tool_result/usage(kind=agent|sub_agent)/stop/error/sub_agent_*/async_task_result/context_update/session_start ✓多数已对齐

## C. 网络层替换边界（唯一许可的差异）

- bb_http.c: host2sockaddr/connect/poll + chunked/SSE pump + 5xx 重试
- https：busybox CONFIG_TLS/wget TLS 栈评估（三期既定）
- 其余协议结构（SseAccumulator/OpenAIToolAccum/build_claude_request/
  convert_to_openai/responses）保持 0-diff

## D. 决策点（需拍板）

- **D1 pthread/msgqueue**：
  a) musl 提供 pthread → 直接链接原样搬 msgqueue/display/readline 线程（最忠实）
  b) 单线程化改造队列为同步直调（已部分如此），行为留差异
- **D2 readline**：linenoise(.c/.h) 为自包含 ANSI 实现，可整体随迁；
  或退化为 fgets + 无行编辑
- **D3 命名**：除 BB_AGENT_* env 外全量回归原名（json/util/store/tools/... +
  函数名 tool_dispatch 等）——已在 review-subagent.md 承诺，与 A 表联动执行
- **D4 默认网络库共存**：CONFIG_BUSYAGENT 用 bb_http；是否同时保留 curl 路径作为编译开关以利测试对照

## E. 验收口径（修订）

```
bash-agent(c) 二进制        busybox busyagent
─────────────────────       ─────────────────
同一提示/输入序列    ⟶      相同的 stdout(events/stream-json 亦然)
                            相同的 sessions 目录布局与 stats/events
仅允许不同：网络字节层(curl vs socket)
测试：test_classify/test_continue/test_transport 三件套直接编译复用，
      除 transport case 外全数通过
```
