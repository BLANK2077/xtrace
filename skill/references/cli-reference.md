# xtrace Legacy CLI Reference

This file preserves the human-oriented xtrace CLI reference. The main `SKILL.md` intentionally focuses on the AI JSON entry point.

## xtrace - NPI RTL 信号追踪 CLI

## 概览

xtrace 用于加载 VCS `simv.daidir` 数据库，并查询 RTL 信号的 driver、load 和 control dependency。使用时优先通过 `tools/xtrace-env` 运行命令。

默认使用中文和用户沟通。

## 环境

- `VERDI_HOME` 必须指向可用的 Verdi 安装。
- `tools/xtrace-env` 会自动设置 NPI 运行时库路径。
- 如果当前目录没有 `xtrace` 二进制，先运行 `make`。

```bash
export VERDI_HOME=/path/to/verdi/V-2023.12-SP2
make
tools/xtrace-env session ensure -dbdir /path/to/simv.daidir --name case_a -json
```

## 基本用法

```bash
# 1. 加载 VCS daidir；路径必须以 .daidir 结尾。
tools/xtrace-env open -dbdir /path/to/simv.daidir --name case_a

# Agent 或脚本优先使用 ensure：必须传 --name，重复 name 会失败。
tools/xtrace-env session ensure -dbdir /path/to/simv.daidir --name case_a -json

# 2. 查看和诊断 session。
tools/xtrace-env session list
tools/xtrace-env session doctor -s case_a
tools/xtrace-env session doctor -s case_a -json

# 3. 查询 signal driver/load。
tools/xtrace-env driver top.u_dut.signal -s case_a
tools/xtrace-env load top.u_dut.signal -s case_a

# 4. 需要脚本处理时使用 JSON 输出。
tools/xtrace-env driver top.u_dut.signal -s case_a -json
tools/xtrace-env load top.u_dut.signal -s case_a -json

# 5. 一条命令完成 ensure + trace。
tools/xtrace-env query -dbdir /path/to/simv.daidir --driver top.u_dut.signal -json
tools/xtrace-env query -dbdir /path/to/simv.daidir --load top.u_dut.signal -json

# 6. 关闭 session。
tools/xtrace-env session kill 1
tools/xtrace-env session kill all
```

## Session 使用

- `open` 只接受 `-dbdir <*.daidir> [other options]`。
- daidir 路径必须存在、必须是目录、且必须以 `.daidir` 结尾。
- 再次使用同一个 name 创建 session 会失败；请先 `session kill <name>` 或选择新 name。
- `session ensure -dbdir <*.daidir> --name <name> [-json]` 更适合脚本：创建具名 session；重复 name 会失败。JSON 输出包含 `ok/id/session_id/status/dbdir_path/message`。
- `session doctor -s <name> [-json] [--debug]` 是判断 session 是否可用的首选命令。
- `--debug` 或 `XTRACE_DEBUG=1` 会把 session 生命周期诊断打印到 stderr；server 启动日志位于 `~/.xtrace/sessions/<name>/debug.log`。
- 常见状态包括：`healthy`、`registry_missing`、`dbdir_missing`、`dbdir_changed`、`process_exited`、`socket_missing`、`connect_failed`、`ping_failed`。

维护文件统一放在 `~/.xtrace/`：

```text
~/.xtrace/
├── registry.json
├── registry.lock
└── sessions/
    └── <name>/
        ├── session.json
        ├── socket
        └── debug.log
```

新版只写上述 JSON/目录布局。旧版顶层 `~/.xtrace.registry` 仅作为兼容迁移输入读取，不会删除。

## Trace 输出

- 普通输出适合人工阅读，会显示 signal、role、location 和 source line。
- `-json` 输出适合脚本处理，包含 `query`、`mode`、`results`、`control_dependencies` 和 `ok/result_count/truncated/roles/files` 摘要字段。
- `--limit <N>` 限制结果数量。
- `--role <role>` 只保留指定角色。
- `--no-statement-only` 过滤只定位到语句的弱结果。
- `role` 可能是 `driver`、`load`、`rhs_use`、`lhs_target`、`condition_use` 或 `statement_only`。
- `resolution="statement_only"` 表示 NPI 只能定位到语句，不能定位到更具体的 signal。

JSON 示例：

```json
{
  "ok": true,
  "query": "top.u_dut.signal",
  "mode": "driver",
  "result_count": 1,
  "truncated": false,
  "roles": ["driver"],
  "files": ["/path/to/rtl.sv"],
  "has_statement_only": false,
  "results": [
    {
      "signal": "top.u_dut.src",
      "role": "driver",
      "file": "/path/to/rtl.sv",
      "line": 123,
      "source": "assign signal = src;",
      "resolution": "signal"
    }
  ],
  "control_dependencies": []
}
```

## 信号发现

当不确定完整层级路径时，先用外部 `rg`/grep 在 RTL 源码中查候选，再用 `signal resolve` 验证完整路径：

```bash
tools/xtrace-env signal resolve top.u_dut.signal -s case_a -json
rg -n "signal" /path/to/rtl
```

- `resolve` 按完整名解析。
- xtrace 不在 daidir 文本中实现 search；短名或片段搜索交给源码 grep。
- JSON 输出包含 `ok/query/matches/count/truncated`。
- 找不到信号时返回非 0，`status` 为 `not_found`。

## AI JSON 入口

面向 AI Agent 或脚本时优先使用 `xtrace ai`，旧的人类 CLI 保持不变：

```bash
tools/xtrace-env ai query <request.json>
tools/xtrace-env ai query -
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"trace.driver",...}'
tools/xtrace-env ai schema
tools/xtrace-env ai actions
```

请求 envelope：

```json
{
  "api_version": "xtrace.ai.v1",
  "request_id": "optional-id",
  "action": "trace.driver",
  "target": {"dbdir": "simv.daidir", "auto_ensure": true},
  "args": {},
  "limits": {},
  "output": {}
}
```

响应 envelope 固定包含 `ok/action/tool/session/summary/data/findings/suggested_next_actions/warnings/error/meta`。已实现 action 覆盖 session、trace、signal、graph、control/source、expression/procedural/sequential/FSM/counter、port/interface 和 batch 能力。

## 常用排查

```bash
# 没有可用 session
tools/xtrace-env session list

# 命令连接失败或结果异常
tools/xtrace-env session doctor -s case_a -json

# 清理不可用或不再需要的 session
tools/xtrace-env session kill 1
tools/xtrace-env session kill all
```
