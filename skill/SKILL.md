---
name: xtrace
description: >
  Use when working with xtrace, an NPI-based CLI for tracing RTL signal drivers,
  loads, and control dependencies from a VCS simv.daidir database. Also use when
  the user mentions xtrace open/session/doctor/driver/load, daidir session
  health, NPI signal tracing, or debugging xtrace daemon lifecycle behavior.
---

# xtrace - NPI RTL 信号追踪 CLI

## 概览

xtrace 用于加载 VCS `simv.daidir` 数据库，并查询 RTL 信号的 driver、load
和 control dependency。使用时优先通过 `tools/xtrace-env` 运行命令。

默认使用中文和用户沟通。

## 环境

- `VERDI_HOME` 必须指向可用的 Verdi 安装。
- `tools/xtrace-env` 会自动设置 NPI 运行时库路径。
- 如果当前目录没有 `xtrace` 二进制，先运行 `make`。

```bash
export VERDI_HOME=/path/to/verdi/V-2023.12-SP2
make
tools/xtrace-env open -dbdir /path/to/simv.daidir
```

## 基本用法

```bash
# 1. 加载 VCS daidir；路径必须以 .daidir 结尾。
tools/xtrace-env open -dbdir /path/to/simv.daidir

# 2. 查看和诊断 session。
tools/xtrace-env session list
tools/xtrace-env session doctor -s 1
tools/xtrace-env session doctor -s 1 -json

# 3. 查询 signal driver/load。
tools/xtrace-env driver top.u_dut.signal -s 1
tools/xtrace-env load top.u_dut.signal -s 1

# 4. 需要脚本处理时使用 JSON 输出。
tools/xtrace-env driver top.u_dut.signal -s 1 -json
tools/xtrace-env load top.u_dut.signal -s 1 -json

# 5. 关闭 session。
tools/xtrace-env session kill 1
tools/xtrace-env session kill all
```

## Session 使用

- `open` 只接受 `-dbdir <*.daidir> [other options]`。
- daidir 路径必须存在、必须是目录、且必须以 `.daidir` 结尾。
- 再次打开同一个 daidir 时，xtrace 会复用已有健康 session。
- `session doctor -s <sid> [-json]` 是判断 session 是否可用的首选命令。
- 常见状态包括：`healthy`、`registry_missing`、`dbdir_missing`、
  `dbdir_changed`、`process_exited`、`socket_missing`、`connect_failed`、
  `ping_failed`。

## Trace 输出

- 普通输出适合人工阅读，会显示 signal、role、location 和 source line。
- `-json` 输出适合脚本处理，包含 `query`、`mode`、`results`、
  `control_dependencies`。
- `role` 可能是 `driver`、`load`、`rhs_use`、`lhs_target`、
  `condition_use` 或 `statement_only`。
- `resolution="statement_only"` 表示 NPI 只能定位到语句，不能定位到更具体的
  signal。

JSON 示例：

```json
{
  "query": "top.u_dut.signal",
  "mode": "driver",
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

## 常用排查

```bash
# 没有可用 session
tools/xtrace-env session list

# 命令连接失败或结果异常
tools/xtrace-env session doctor -s 1 -json

# 清理不可用或不再需要的 session
tools/xtrace-env session kill 1
tools/xtrace-env session kill all
```
