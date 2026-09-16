# AGENTS.md — busyagent 开发规约

## 回归门禁（必须）

任何代码改动（功能、修复、重构），在宣布完成或合入前**必须**在 builder 容器内跑全量回归，四套全绿才算完成：

```sh
docker run --rm -v /Users/lloyd/moox/busyagent:/src:ro busyagent-builder:latest sh -c '
set -e
rm -rf /tmp/ba-src && cp -a /src /tmp/ba-src && chmod -R u+w /tmp/ba-src
cd /tmp/ba-src
make -j8 busybox_unstripped > /tmp/build.log 2>&1 || { echo BUILD_FAIL; grep error /tmp/build.log | head; exit 1; }
make busybox >> /tmp/build.log 2>&1
cd testsuite
for t in busyagent oapi mcpc jq; do
    out=$(./runtest $t 2>&1 || true)
    echo "$t: PASS=$(echo "$out" | grep -c "^PASS") FAIL=$(echo "$out" | grep -c "^FAIL")"
done
'
```

要点：
- 源码**只读挂载**，容器内 cp 到 /tmp 再构建；不要在仓库留临时脚本（跑完即删）
- 镜像内只有 `sh`（无 bash），docker 命令避免被安全策略拦截的形式：`sh -c 'cp /src/<script> /tmp/ && sh /tmp/<script>'`
- 测试入口是 `testsuite/runtest <applet>`（不是直接跑 `.tests` 文件——那会缺 applet 符号链接和 OPTIONFLAGS，导致大面积假 FAIL/SKIP）
- 分多个提交的 PR：**每个提交点**都要单独验证构建+busyagent 测试（bisect 健康性）

基线（master de948879e，2026-09-16）：busyagent 52 / oapi 36 / mcpc 26 / jq 33，全 PASS。
