# busyagent — 跑在 busybox 里的 AI agent

> **一个 1.3MB 的静态单文件**：409 个 busybox applet + 一个能用这些工具解决真实问题的 LLM agent。
> 无 libc 依赖、FROM scratch、可直接进路由器 / NAS / 工控盒。

```bash
docker run --rm --network host \
  -e BB_AGENT_BASE_URL=http://host:8317/v1 \
  -e BB_AGENT_API_KEY=sk-xxx \
  -e BB_AGENT_MODEL=your-model \
  -it lloydzhou/busyagent
```

进入交互 REPL（busybox lineedit 行编辑，支持中文输入、上/下键历史、会话按目录自动续接）。
也可以单发一条命令直接拿答案：

```bash
docker run --rm --network host \
  -e BB_AGENT_BASE_URL=... -e BB_AGENT_API_KEY=... -e BB_AGENT_MODEL=... \
  lloydzhou/busyagent busyagent "为什么这个构建失败了"
```

---

## 为轻量设备而生

busyagent 的定位不是桌面助手，而是「设备上唯一的东西就是 busybox」的场景：
路由器、NAS、工控网关、救援环境。一个静态二进制带进去，设备上所有
busybox applet 立刻变成 agent 可调用的工具。

### 实测：64MB 内存 / 0.5 核 / 只读根文件系统的设备排查

约束（模拟真实嵌入式环境）：

```bash
docker run --rm --network host \
  --memory 64m --cpus 0.5 --read-only \
  --tmpfs /tmp:rw,size=8m \
  -e BB_AGENT_BASE_URL=... -e BB_AGENT_API_KEY=... -e BB_AGENT_MODEL=... \
  lloydzhou/busyagent sh -c '
    printf <故障脚本> > /tmp/S99demo          # 设备上现成的"坏"启动脚本
    export BB_AGENT_HOME=/tmp/agent           # 可写区 = 会话与历史
    busyagent "这个开机脚本没起来，用本机 busybox 工具排查全部根因"'
```

agent 全程只调用设备上真实存在的 applet（`ls` `wc` `head` `od` `ps` `grep`
`sh -n` `find` `command -v`），给出的结论（真实输出节选）：

1. **脚本不可执行** —— `-rw-r--r--`，没有执行位，开机直接 `Permission denied`
2. **CRLF 换行** —— `od` 显示行尾 `0d 0a`，`sh -n` 报
   `syntax error: unexpected word (expecting "in")`，解释器路径也会变成 `/bin/sh\r`
3. **目标程序不存在** —— `/usr/bin/demo-daemon: No such file or directory`，全盘 `find` 也没有
4. 未注册进任何启动流程（`/etc/init.d` 等目录全不存在）；后台启动失败无任何上报

失败链：`无执行位 → CRLF 语法错误 → daemon 缺失 → 未接入启动流程`。

### 资源占用

| 项 | 值 |
|---|---|
| 镜像（压缩） | ~832KB |
| 静态二进制（含 agent，amd64/arm64） | 1.34MB |
| 运行内存 | 64MB 约束下全流程正常 |
| 会话存储 | 纯 append-only jsonl，断电安全 |

---

## 会话持久化

会话按「工作目录」自动绑定（不传参数即续接最近一次对话），建议挂卷：

```bash
docker run -it --network host \
  -v busyagent-data:/root/.busyagent \
  -e BB_AGENT_BASE_URL=... -e BB_AGENT_API_KEY=... -e BB_AGENT_MODEL=... \
  lloydzhou/busyagent
```

`/root/.busyagent` 下是每个会话的 `conversation.jsonl`（模型上下文）、
`events.jsonl`（完整过程 trace，可用 `jq` 回看）、`stats.json`。

## 环境变量

| 变量 | 说明 |
|---|---|
| `BB_AGENT_BASE_URL` | LLM API 地址（OpenAI 兼容 `/v1`；也支持 claude / responses 协议） |
| `BB_AGENT_API_KEY`  | API key |
| `BB_AGENT_MODEL`    | 模型名 |
| `BB_AGENT_HOME`     | 会话与历史存储目录（默认 `$HOME/.busyagent`，镜像内已设 `/root/.busyagent`） |
| `BB_AGENT_OUTPUT`   | `text`（默认）/ `json`（stream-json，供程序消费） |

## 工具能力

agent 在 **409 个 busybox applet** 之内自由组合：
`sh` `grep` `sed` `awk` `find` `cat` `ls` `od` `ps` `netstat` `wget`。
支持后台任务（`background: true`，完成后自动把退出码与输出回注会话）、
SubAgent 子任务（fork 继承会话上下文）、多轮工具循环直到收敛。

## 构建与架构

```bash
docker buildx build --platform linux/amd64,linux/arm64 \
  -t lloydzhou/busyagent:latest -t lloydzhou/busybox:latest --push .
```

全静态 musl 链接；内置完整 CJK 码表与宽字符宽度（REPL 中文输入/回显）。

## 已知限制

- HTTP only（HTTPS 计划接入 busybox 内置 TLS）；公网 API 建议经内网网关代理，
  设备侧走 `http://内网地址:port/v1`
- 依赖一个 OpenAI 兼容端点；设备直连场景推荐网关聚合（key 不落设备）
